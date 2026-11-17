// ENC-1646 (SPEC 2026-11-17-order-flow-pipelines D4): every key the flow
// vocabulary declares Pushed is actually notified, and every Stored key is
// never notified but is in the AtomicStore — so no flow key is the ENC-1575
// hazard (a Listener that registers and never fires).
//
// The subscribers are real nodes::Listener instances built with
// Listener::Create, which is also the proof that a Listener on flow.* is
// accepted (flow is not pipeline-only).
//
// OPERATOR TICKETS: this iterates the WHOLE vocabulary, so a new operator's
// pushed keys fail here until you extend driveEveryKey() with frames that
// make your operator publish each of them.
#include "FlowTestRig.hpp"

#include "krn/atomic/AtomicProviderRegistry.hpp"
#include "krn/engine/ConfigNamespaceRegistry.hpp"
#include "krn/nodes/Listener.hpp"
#include "krn/rt/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

using namespace krn;
using namespace krn::flowtest;

namespace {

class CountingNode final : public INode {
public:
  void onValue(const StreamValue&) override { n.fetch_add(1); }
  void shutdown() noexcept override {}
  std::atomic<int> n{0};
};

// Frames that make every V1 operator publish every pushed key at least once.
void driveEveryKey(Rig& r) {
  seed(r);                                                        // touch.bid.added, ofi
  r.frame({upd(1, 60, MutationCause::Execution)});                // touch.bid.filled
  r.frame({upd(2, 20, MutationCause::Cancel)});                   // touch.bid.cancelled
  r.frame({add(6, Side::Ask, 10.01, 20)});                        // touch.ask.added
  r.frame({upd(4, 50, MutationCause::Execution)});                // touch.ask.filled
  r.frame({del(6, MutationCause::Cancel)});                       // touch.ask.cancelled
  r.frame({trade(10.00, 10, Aggressor::Sell, false)});            // meta.unattributed
  r.frame({del(2, MutationCause::Cancel)});                       // (#2 is still known: the trade consumed #1's FIFO front)
  r.frame({del(99, MutationCause::Unknown)});                     // (unknown to the book: not observed)
  r.frame({del(3, MutationCause::Unknown, 0)});                   // meta.unknown_cause, ts_source
  // ── P2..P5 (ENC-1647..1649): add frames for your keys here. ──
  // P3 (ENC-1648). Bid 10.00 = #1 50 (the shadow forgot it at the
  // unattributed trade), tenure D0 = 100; ask 10.01 = #4 50, D0 = 80.
  r.frame({upd(1, 40, MutationCause::Execution)});                // opens a bid refill window
  r.frame({add(20, Side::Bid, 10.00, 25)});                       // refill.bid
  r.frame({trade(10.00, 200, Aggressor::Sell, true)});            // hidden print: absorb.bid.*
  r.frame({add(21, Side::Ask, 10.01, 40)});                       // refill.ask (after upd(4) above)
  r.frame({trade(10.01, 200, Aggressor::Buy, true)});             // hidden print: absorb.ask.*
}

} // namespace

TEST(FlowPushedKeysTest, ListenerOnFlowIsAccepted) {
  EXPECT_FALSE(AtomicProviderRegistry::isPipelineOnlyKey("flow.ofi"));
  EXPECT_FALSE(AtomicProviderRegistry::isPipelineOnlyKey("flow.touch.bid.cancelled"));
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  auto res = nodes::Listener::Create("NEXO", "flow.touch.bid.cancelled",
                                     std::make_shared<CountingNode>(), &pool, &dispatcher);
  ASSERT_TRUE(res.has_value()) << res.error().message;
  (*res)->shutdown();
  pool.shutdown();
}

TEST(FlowPushedKeysTest, MarketConnectorRegistersTheFlowConfigNamespace) {
  // The test bootstrap ran MarketConnector::registerWith once.
  EXPECT_TRUE(engine::ConfigNamespaceRegistry::contains("flow"));
  EXPECT_TRUE(engine::ConfigNamespaceRegistry::dispatch("flow.sweep.minLevels", "3"));
  EXPECT_FALSE(engine::ConfigNamespaceRegistry::dispatch("flow.sweep.minLevelz", "3"))
      << "a misspelt flow key is left unclaimed, so the engine warns";
}

TEST(FlowPushedKeysTest, EveryPushedKeyIsNotifiedAndNoStoredKeyIs) {
  rt::ThreadPool pool(1);
  Rig r;
  // Real Listeners on every vocabulary key, on r's dispatcher.
  std::map<std::string, std::shared_ptr<CountingNode>> counts;
  std::vector<std::shared_ptr<nodes::Listener>> listeners;
  for (const auto& k : r.flow->vocabulary()) {
    auto node = std::make_shared<CountingNode>();
    auto res = nodes::Listener::Create(kSym, k.name, node, &pool, &r.dispatcher);
    ASSERT_TRUE(res.has_value()) << k.name << ": " << res.error().message;
    listeners.push_back(*res);
    counts[k.name] = node;
  }

  driveEveryKey(r);

  // Listener posts to the pool; let it drain.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto allPushedSeen = [&] {
    for (const auto& k : r.flow->vocabulary())
      if (k.publication == flow::Publication::Pushed && counts[k.name]->n.load() == 0) return false;
    return true;
  };
  while (!allPushedSeen() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));   // any stray stored-key notify lands too

  std::size_t pushed = 0, stored = 0;
  for (const auto& k : r.flow->vocabulary()) {
    const int n = counts[k.name]->n.load();
    if (k.publication == flow::Publication::Pushed) {
      ++pushed;
      EXPECT_GT(n, 0) << k.name << " is documented pushed but was never notified";
      EXPECT_FALSE(r.store.get(kSym, k.name).has_value())
          << k.name << " is pushed and must not be written to the store";
    } else {
      ++stored;
      EXPECT_EQ(n, 0) << k.name << " is AtomicAccessor-only but was notified";
      EXPECT_TRUE(r.store.get(kSym, k.name).has_value()) << k.name << " was never stored";
    }
  }
  EXPECT_GE(pushed, 7u);
  EXPECT_GE(stored, 6u);

  for (auto& l : listeners) l->shutdown();
  pool.shutdown();
}
