// ENC-1647 (SPEC 2026-11-17-order-flow-pipelines D5 P2, rev 3; D4; D6; D7):
// sweep detection on hand-built frames through the real IngressApplier, a
// real OrderBookManager and the real FlowComponent.
#include "FlowTestRig.hpp"

#include "krn/flow/FlowOperators.hpp"
#include "krn/flow/SweepOperator.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>

using namespace krn;
using namespace krn::flowtest;
using M = std::map<std::string, double>;
using Sweep = flow::SweepOperator;

namespace {

const std::string kOther = "OTHR";
constexpr uint64_t T1 = kTs + 1'000;
constexpr uint64_t T2 = kTs + 2'000;
constexpr uint64_t T3 = kTs + 3'000;

// Only the flow.sweep.* sums.
M sweepOf(const M& all) {
  M out;
  for (const auto& [k, v] : all)
    if (k.rfind("flow.sweep.", 0) == 0) out[k] = v;
  return out;
}

M sweep(double levels, double notional, double px, double side) {
  return M{{Sweep::kLevels, levels}, {Sweep::kNotional, notional}, {Sweep::kPx, px},
           {Sweep::kSide, side}, {Sweep::kCount, 1}};
}

void expectSweep(const M& got, double levels, double notional, double px, double side) {
  ASSERT_EQ(got.size(), 5u) << "all five sweep keys from one close";
  EXPECT_DOUBLE_EQ(got.at(Sweep::kLevels), levels);
  EXPECT_NEAR(got.at(Sweep::kNotional), notional, 1e-6);
  EXPECT_DOUBLE_EQ(got.at(Sweep::kPx), px);
  EXPECT_DOUBLE_EQ(got.at(Sweep::kSide), side);
  EXPECT_DOUBLE_EQ(got.at(Sweep::kCount), 1);
}

// seed() plus a third ask level: ask 10.01 #4 80, 10.02 #5 300, 10.03 #6 50.
void seedDeep(Rig& r) {
  seed(r);
  r.frame({add(6, Side::Ask, 10.03, 50)});
  r.take();
}

// A fake receive clock and a captured scheduler for the close timer.
struct FakeTimer {
  uint64_t now = 1'000'000'000;
  std::vector<std::pair<uint64_t, std::function<void()>>> pending;   // (delayNs, fire)
  Sweep::Timing timing() {
    return {[this] { return now; },
            [this](uint64_t d, std::function<void()> f) { pending.emplace_back(d, std::move(f)); }};
  }
  // Run every pending callback (each may re-arm).
  void fireAll() {
    auto p = std::move(pending);
    pending.clear();
    for (auto& [d, f] : p) f();
  }
};

// A rig whose component runs `ops` instead of the default set. The rig's
// recorders were registered for the default vocabulary, which includes P2.
void useOperators(Rig& r, std::vector<std::unique_ptr<flow::IFlowOperator>> ops,
                  boost::asio::io_context* io = nullptr) {
  r.flow = std::make_shared<flow::FlowComponent>(
      flow::FlowComponent::Env{&r.dispatcher, &r.store, &r.mgr, io}, std::move(ops));
  r.attach();
}

} // namespace

TEST(FlowSweepTest, IsInTheDefaultOperatorSet) {
  flow::FlowComponent c(flow::FlowComponent::Env{});
  bool found = false;
  for (const auto& op : c.operators()) found |= op->name() == "sweep";
  EXPECT_TRUE(found);
}

// One level executed, however many orders: an aggressive event, not a sweep.
TEST(FlowSweepTest, OneLevelIsNotASweep) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(1, MutationCause::Execution, T1)});
  r.frame({upd(2, 10, MutationCause::Execution, T1)});   // both at bid 10.00
  r.frame({add(9, Side::Bid, 9.90, 1, T2)});              // later tsNs: close
  EXPECT_EQ(sweepOf(r.take()), M{});
}

TEST(FlowSweepTest, ThreeLevelBuySweepPublishesOnceAtClose) {
  Rig r;
  seedDeep(r);
  r.frame({del(4, MutationCause::Execution, T1)});          // 80 @ 10.01
  r.frame({del(5, MutationCause::Execution, T1),            // 300 @ 10.02 (one frame or many)
           upd(6, 20, MutationCause::Execution, T1)});      // 30 @ 10.03
  r.frame({add(7, Side::Bid, 10.00, 5, T1)});               // the remainder rests at T1: no close
  EXPECT_EQ(sweepOf(r.take()), M{}) << "nothing until the run closes";
  r.frame({add(8, Side::Bid, 9.90, 1, T2)});
  expectSweep(sweepOf(r.take()), 3, 80 * 10.01 + 300 * 10.02 + 30 * 10.03, 10.03, +1);
  r.frame({add(9, Side::Bid, 9.80, 1, T3)});
  EXPECT_EQ(sweepOf(r.take()), M{}) << "published once";
}

// Hidden prints (bookNeutral, no resting order) and the displayed-execution
// prints (bookNeutral, resting order set) inside a sweep: skipped, not run-
// breaking, and not counted in the notional.
TEST(FlowSweepTest, HiddenPrintInsideASweepIsSkipped) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(1, MutationCause::Execution, T1),
           trade(10.00, 100, Aggressor::Sell, true, T1, 1)});        // E print
  r.frame({trade(9.995, 400, Aggressor::Sell, true, T1)});           // hidden P print
  r.frame({del(2, MutationCause::Execution, T1),
           trade(10.00, 50, Aggressor::Sell, true, T1, 2)});
  r.frame({upd(3, 150, MutationCause::Execution, T1),
           trade(9.99, 50, Aggressor::Sell, true, T1, 3)});
  EXPECT_EQ(sweepOf(r.take()), M{});
  r.frame({trade(9.98, 10, Aggressor::Sell, true, T2)});             // a later print closes it
  expectSweep(sweepOf(r.take()), 2, 100 * 10.00 + 50 * 10.00 + 50 * 9.99, 9.99, -1);
}

TEST(FlowSweepTest, UnattributedTradesAreSingleLevelExecutions) {
  Rig r;
  seed(r);
  r.take();
  r.frame({trade(10.00, 150, Aggressor::Sell, false, T1)});
  r.frame({trade(9.99, 50, Aggressor::Unknown, false, T1)});   // passive side by price: bid
  r.frame({add(9, Side::Ask, 10.05, 1, T2)});
  expectSweep(sweepOf(r.take()), 2, 150 * 10.00 + 50 * 9.99, 9.99, -1);
}

// Orders resting before the shadow started: side, price and shares come from
// the TobChange.
TEST(FlowSweepTest, UnknownOrdersFallBackOnTheTopOfBook) {
  Rig r(/*attach=*/false);
  seedDeep(r);
  r.attach();
  r.frame({del(4, MutationCause::Execution, T1)});
  r.frame({upd(5, 100, MutationCause::Execution, T1)});
  r.frame({add(9, Side::Bid, 9.90, 1, T2)});
  expectSweep(sweepOf(r.take()), 2, 80 * 10.01 + 200 * 10.02, 10.02, +1);
}

// Two symbols executing at the same tsNs, interleaved: neither breaks the
// other's run, and a later-tsNs event of EITHER closes both (onBeforeEvent).
// An earlier tsNs of another symbol closes nothing.
TEST(FlowSweepTest, TwoSymbolsInterleaved) {
  Rig r;
  r.mgr.setTickSize(kOther, 0.01);
  std::vector<std::pair<std::string, double>> otherLog;
  std::vector<std::shared_ptr<KeyRecorder>> keep;
  for (const auto& k : r.flow->vocabulary()) {
    auto rec = std::make_shared<KeyRecorder>(k.name, &otherLog);
    r.dispatcher.registerListener(kOther, k.name, rec);
    keep.push_back(rec);
  }
  auto oAdd = [&](uint64_t id, Side s, double px, uint64_t sz, uint64_t ts) -> FeedEvent {
    return feed::ObAddEvent{kOther, id, s, px, sz, 0, std::nullopt, ts, std::nullopt};
  };
  auto oDel = [&](uint64_t id, uint64_t ts) -> FeedEvent {
    return feed::ObDeleteEvent{kOther, id, std::nullopt, MutationCause::Execution, ts};
  };
  auto otherSweep = [&] {
    M m;
    for (const auto& [k, v] : otherLog) if (k.rfind("flow.sweep.", 0) == 0) m[k] += v;
    otherLog.clear();
    return m;
  };

  seed(r);
  r.frame({oAdd(101, Side::Bid, 50.00, 10, kTs)});
  r.frame({oAdd(102, Side::Bid, 49.99, 20, kTs)});
  r.take();
  otherSweep();

  r.frame({del(4, MutationCause::Execution, T1)});   // FLOW ask 10.01
  r.frame({oDel(101, T1)});                          // OTHR bid 50.00
  r.frame({del(5, MutationCause::Execution, T1)});   // FLOW ask 10.02
  r.frame({oDel(102, T1)});                          // OTHR bid 49.99
  r.frame({oAdd(103, Side::Ask, 60.00, 1, kTs)});    // OTHR, EARLIER tsNs: closes only its own run
  EXPECT_EQ(sweepOf(r.take()), M{}) << "an earlier tsNs of another symbol closes nothing";
  expectSweep(otherSweep(), 2, 10 * 50.00 + 20 * 49.99, 49.99, -1);

  r.frame({oAdd(104, Side::Ask, 60.01, 1, T2)});     // OTHR, later tsNs: closes FLOW's run
  expectSweep(sweepOf(r.take()), 2, 80 * 10.01 + 300 * 10.02, 10.02, +1);
  EXPECT_EQ(otherSweep(), M{});
}

// Executions on both resting sides at one tsNs are two aggressive events; an
// interleaving does not break either, and each side counts its own levels.
TEST(FlowSweepTest, OppositeSideExecutionsAtOneTs) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(4, MutationCause::Execution, T1)});   // ask 10.01 (opens first)
  r.frame({del(1, MutationCause::Execution, T1)});   // bid 10.00
  r.frame({del(5, MutationCause::Execution, T1)});   // ask 10.02
  r.frame({del(2, MutationCause::Execution, T1)});   // bid 10.00 again: still one bid level
  r.frame({add(9, Side::Bid, 9.90, 1, T2)});
  EXPECT_EQ(sweepOf(r.take()), sweep(2, 80 * 10.01 + 300 * 10.02, 10.02, +1))
      << "the bid run is one level; the ask run is a buy sweep";

  Rig q;
  seed(q);
  q.take();
  q.frame({del(4, MutationCause::Execution, T1)});
  q.frame({del(1, MutationCause::Execution, T1)});
  q.frame({del(5, MutationCause::Execution, T1)});
  q.frame({del(3, MutationCause::Execution, T1)});   // bid 9.99: the bid run is two levels too
  q.log.clear();
  q.frame({add(9, Side::Ask, 10.50, 1, T2)});
  std::vector<double> sides;
  for (const auto& [k, v] : q.log) if (k == Sweep::kSide) sides.push_back(v);
  EXPECT_EQ(sides, (std::vector<double>{+1, -1})) << "two sweeps, published in open order";
  const M m = q.take();
  EXPECT_DOUBLE_EQ(m.at(Sweep::kCount), 2);
  EXPECT_DOUBLE_EQ(m.at(Sweep::kLevels), 4);
}

// A quiet symbol: nothing later arrives, so the closeMs timer closes the run.
TEST(FlowSweepTest, TimerClosesAQuietSymbol) {
  FakeTimer ft;
  auto op = std::make_unique<Sweep>(ft.timing());
  Sweep* sw = op.get();
  std::vector<std::unique_ptr<flow::IFlowOperator>> ops;
  ops.push_back(std::move(op));
  Rig r(/*attach=*/false);
  useOperators(r, std::move(ops));
  seed(r);
  r.take();

  r.frame({del(4, MutationCause::Execution, T1)});
  r.frame({del(5, MutationCause::Execution, T1)});
  ASSERT_EQ(ft.pending.size(), 1u) << "one timer, armed when the run opened";
  EXPECT_EQ(ft.pending[0].first, 50'000'000u) << "flow.sweep.closeMs default 50 ms";
  EXPECT_EQ(sw->openSymbols(), 1u);

  ft.now += 49'000'000;
  ft.fireAll();
  EXPECT_EQ(sweepOf(r.take()), M{}) << "not due yet";
  ASSERT_EQ(ft.pending.size(), 1u) << "re-armed for the remainder";
  EXPECT_EQ(ft.pending[0].first, 1'000'000u);

  ft.now += 1'000'000;
  ft.fireAll();
  expectSweep(sweepOf(r.take()), 2, 80 * 10.01 + 300 * 10.02, 10.02, +1);
  EXPECT_EQ(sw->openSymbols(), 0u);
  EXPECT_TRUE(ft.pending.empty()) << "nothing open, nothing armed";

  r.frame({add(9, Side::Bid, 9.90, 1, T2)});
  EXPECT_EQ(sweepOf(r.take()), M{}) << "the timer's close was the only one";
}

// The production timer: a steady_timer on the component's io_context, with
// flow.sweep.closeMs read from the flow namespace at start().
TEST(FlowSweepTest, IoContextTimerClosesAfterCloseMs) {
  boost::asio::io_context io;
  Rig r(/*attach=*/false);
  useOperators(r, flow::makeFlowOperators(), &io);
  ASSERT_TRUE(r.flow->configSource()->apply("sweep.closeMs", "20"));
  seed(r);
  r.take();
  const auto t0 = std::chrono::steady_clock::now();
  r.frame({del(4, MutationCause::Execution, T1)});
  r.frame({del(5, MutationCause::Execution, T1)});
  EXPECT_EQ(sweepOf(r.take()), M{});
  io.run_for(std::chrono::seconds(2));   // returns once the timer has fired and nothing re-armed
  const auto waited = std::chrono::steady_clock::now() - t0;
  expectSweep(sweepOf(r.take()), 2, 80 * 10.01 + 300 * 10.02, 10.02, +1);
  EXPECT_GE(waited, std::chrono::milliseconds(20));
  EXPECT_LT(waited, std::chrono::seconds(2));
}

// A component destroyed with its close timer pending: the timer fires into
// nothing.
TEST(FlowSweepTest, PendingTimerOutlivesTheComponent) {
  boost::asio::io_context io;
  {
    Rig r(/*attach=*/false);
    useOperators(r, flow::makeFlowOperators(), &io);
    seed(r);
    r.frame({del(4, MutationCause::Execution, T1)});
    r.frame({del(5, MutationCause::Execution, T1)});
    r.reg->clear();
  }
  io.run_for(std::chrono::seconds(1));
  SUCCEED();
}

// D6: tsNs == 0 falls back on the receive clock, which is distinct per event —
// so a fallback execution is an aggressive event of its own (one level).
TEST(FlowSweepTest, ReceiveClockExecutionsAreTheirOwnRun) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(4, MutationCause::Execution, 0)});
  r.frame({del(5, MutationCause::Execution, 0)});
  r.frame({add(9, Side::Bid, 9.90, 1, 0)});
  r.frame({add(10, Side::Bid, 9.80, 1, T2)});
  EXPECT_EQ(sweepOf(r.take()), M{}) << "no shared instant, so no two-level run";
  EXPECT_EQ(r.stored("flow.meta.ts_source"), 0);

  // A fallback event of the SAME symbol ends an exchange-time run (it is a
  // different instant).
  Rig q;
  seedDeep(q);
  q.frame({del(4, MutationCause::Execution, T1)});
  q.frame({del(5, MutationCause::Execution, T1)});
  q.frame({add(9, Side::Bid, 9.90, 1, 0)});
  expectSweep(sweepOf(q.take()), 2, 80 * 10.01 + 300 * 10.02, 10.02, +1);
}

TEST(FlowSweepTest, ReceiveClockWithMinLevelsOnePublishesEachExecution) {
  Rig r(/*attach=*/false);
  useOperators(r, flow::makeFlowOperators());
  ASSERT_TRUE(r.flow->configSource()->apply("sweep.minLevels", "1"));
  seed(r);
  r.take();
  r.frame({del(4, MutationCause::Execution, 0)});
  expectSweep(sweepOf(r.take()), 1, 80 * 10.01, 10.01, +1);
}

TEST(FlowSweepTest, MinLevelsIsConfigurable) {
  Rig r(/*attach=*/false);
  useOperators(r, flow::makeFlowOperators());
  ASSERT_TRUE(r.flow->configSource()->apply("sweep.minLevels", "3"));
  seedDeep(r);
  r.frame({del(4, MutationCause::Execution, T1)});
  r.frame({del(5, MutationCause::Execution, T1)});
  r.frame({add(9, Side::Bid, 9.90, 1, T2)});
  EXPECT_EQ(sweepOf(r.take()), M{}) << "two levels < minLevels 3";
  r.frame({del(1, MutationCause::Execution, T3)});
  r.frame({del(3, MutationCause::Execution, T3)});
  r.frame({upd(2, 0, MutationCause::Execution, T3)});   // a terminal update, at 10.00 again
  r.frame({add(10, Side::Ask, 10.50, 1, T3 + 1)});
  EXPECT_EQ(sweepOf(r.take()), M{}) << "10.00 and 9.99: two distinct prices, however many orders";
}

TEST(FlowSweepTest, BookResetPublishesTheOpenRun) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(1, MutationCause::Execution, T1)});
  r.frame({del(3, MutationCause::Execution, T1)});
  r.flow->onBookReset(feed::ObResetEvent{kSym, 2});
  expectSweep(sweepOf(r.take()), 2, 100 * 10.00 + 200 * 9.99, 9.99, -1);
}
