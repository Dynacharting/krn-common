// ENC-1645 (SPEC 2026-11-17-order-flow-pipelines D3): the IngressApplier seam.
//
// Drives real ITCH frames (through ItchAdapter) and real FeedServer JSON lines
// (through translateObJson) into a real OrderBookManager via IngressApplier,
// with a recording IFlowObserver, and pins: observation order, cause, the
// TobChange pre/post of every event, the unattributed flag, and that nothing
// is observed for a rejected, stale-gated or seq-dropped event.

#include "krn/book/OrderBookManager.hpp"
#include "krn/feed/FeedEvent.hpp"
#include "krn/feed/IFlowObserver.hpp"
#include "krn/feed/IngressApplier.hpp"
#include "krn/feed/ItchAdapter.hpp"
#include "krn/feed/ObJsonTranslator.hpp"
#include "krn/market/FeedServer.hpp"
#include "krn/market/MarketConnector.hpp"

#include "krn/util/Metrics.hpp"

#include <boost/asio.hpp>

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace krn;
using namespace krn::feed;

namespace {

constexpr uint64_t kSec      = 1'000'000'000ULL;
constexpr uint64_t kMidnight = 1'794'873'600ULL * kSec;   // 2026-11-17T00:00:00Z
constexpr uint64_t kT0       = 34'200 * kSec;             // 09:30:00 since midnight

ItchAdapter clockedAdapter() {
  ItchAdapter::Options o;
  o.wallClockNs = [] { return kMidnight + 52'200 * kSec; };
  return ItchAdapter(std::move(o));
}

struct Seen {
  FeedEvent event;
  TobChange tob;
  bool      unattributed;
  bool      insideDeltaHandler;  // was a subscribeDeltas handler on the stack?
  uint64_t  deltasSoFar;         // deltas published before this observation
};

// Shared with the delta handler so the observer can tell where it is called.
struct DeltaProbe {
  bool     inHandler = false;
  uint64_t count = 0;
};

class Recorder final : public IFlowObserver {
public:
  explicit Recorder(const DeltaProbe* probe = nullptr) : probe_(probe) {}
  void onFlowEvent(const FlowObservation& o) override {
    seen.push_back(Seen{o.event, o.tob, o.unattributed,
                        probe_ ? probe_->inHandler : false,
                        probe_ ? probe_->count : 0});
  }
  void onBookReset(const ObResetEvent& r) override { resets.push_back(r); }

  std::vector<Seen>         seen;
  std::vector<ObResetEvent> resets;

private:
  const DeltaProbe* probe_;
};

struct Rig {
  OrderBookManager                      mgr;
  std::shared_ptr<FlowObserverRegistry> reg = std::make_shared<FlowObserverRegistry>();
  DeltaProbe                            probe;
  std::shared_ptr<Recorder>             rec = std::make_shared<Recorder>(&probe);
  IngressApplier                        applier{nullptr, &mgr, reg};
  ItchAdapter                           itch = clockedAdapter();

  Rig() {
    reg->add(rec);
    mgr.subscribeDeltas("FLOW", [this](const BookDelta&) {
      probe.inHandler = true;
      ++probe.count;
      probe.inHandler = false;
    });
  }

  IngressApplier::FrameResult itchFrame(const std::string& msg) {
    auto frame = itch.translate(msg);
    return applier.apply(frame);
  }

  // FeedServer's path for an `ob` line: translate, (no seq here), apply.
  IngressApplier::FrameResult jsonLine(const std::string& line) {
    rapidjson::Document d;
    d.Parse(line.c_str());
    auto f = translateObJson(d);
    EXPECT_EQ(f.status, ObJsonFrame::Status::Ok) << line;
    return applier.applyGated(f.events, false);
  }
};

std::string add(uint64_t ref, const char* side, uint64_t shares, double px, uint64_t ts) {
  return std::string(R"({"type":"add_order","stock":"FLOW","timestamp":)") + std::to_string(ts) +
         R"(,"orderRef":)" + std::to_string(ref) + R"(,"side":")" + side +
         R"(","shares":)" + std::to_string(shares) + R"(,"price":)" + std::to_string(px) + "}";
}

void expectSide(const TobLevel& l, bool present, int64_t ticks, uint64_t size, const char* what) {
  EXPECT_EQ(l.present, present) << what;
  if (present) {
    EXPECT_EQ(l.price.ticks, ticks) << what;
    EXPECT_EQ(l.size, size) << what;
  }
}

template <typename T> const T& as(const Seen& s) { return std::get<T>(s.event); }

ObTradeEvent trade(double px, uint64_t sz, Aggressor a, bool bookNeutral) {
  return ObTradeEvent{"FLOW", px, sz, a, 0, bookNeutral, std::nullopt, std::nullopt, std::nullopt};
}

} // namespace

// ---------------------------------------------------------------------------
// A full ITCH order lifecycle: every applied event is observed once, in frame
// order, with its cause and the exact top of book before and after.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, ItchLifecycleObservedInOrderWithCauseAndTob) {
  Rig r;
  r.itchFrame(R"({"type":"stock_directory","stockLocate":1,"stock":"FLOW    "})");  // tick 0.01
  ASSERT_DOUBLE_EQ(r.mgr.getTickSize("FLOW"), 0.01);

  r.itchFrame(add(1, "B", 100, 10.00, kT0 + 1));   // bid 1000 x100
  r.itchFrame(add(2, "S", 50, 10.01, kT0 + 2));    // ask 1001 x50
  r.itchFrame(add(3, "B", 30, 10.00, kT0 + 3));    // bid 1000 x130
  ASSERT_EQ(r.rec->seen.size(), 3u);

  // E: partial execution of order 1 → ObUpdate{Execution} + bookNeutral trade.
  auto er = r.itchFrame(R"({"type":"order_executed","orderRef":1,"shares":40,"matchNumber":77,"timestamp":)" +
                        std::to_string(kT0 + 4) + "}");
  EXPECT_EQ(er.applied, 1u);   // the update; the print applies nothing
  EXPECT_EQ(er.observed, 2u);

  // X: partial cancel of order 3; D: delete order 2 (the whole ask).
  r.itchFrame(R"({"type":"order_cancel","orderRef":3,"shares":10,"timestamp":)" + std::to_string(kT0 + 5) + "}");
  r.itchFrame(R"({"type":"order_delete","orderRef":2,"timestamp":)" + std::to_string(kT0 + 6) + "}");
  // U: replace order 1 (60 left) by order 4 @10.02 x60 — crosses nothing (ask is empty).
  r.itchFrame(R"({"type":"order_replace","origOrderRef":1,"orderRef":4,"shares":60,"price":10.02,"timestamp":)" +
              std::to_string(kT0 + 7) + "}");

  const auto& s = r.rec->seen;
  ASSERT_EQ(s.size(), 9u);

  // [0] add 1: empty → bid 1000x100.
  ASSERT_TRUE(std::holds_alternative<ObAddEvent>(s[0].event));
  EXPECT_EQ(as<ObAddEvent>(s[0]).tsNs, kMidnight + kT0 + 1);
  expectSide(s[0].tob.pre.bid, false, 0, 0, "add1 pre bid");
  expectSide(s[0].tob.post.bid, true, 1000, 100, "add1 post bid");
  EXPECT_DOUBLE_EQ(s[0].tob.post.bid.priceD, 10.00);
  expectSide(s[0].tob.post.ask, false, 0, 0, "add1 post ask");

  // [1] add 2: ask appears.
  expectSide(s[1].tob.pre.ask, false, 0, 0, "add2 pre ask");
  expectSide(s[1].tob.post.ask, true, 1001, 50, "add2 post ask");
  expectSide(s[1].tob.post.bid, true, 1000, 100, "add2 post bid");

  // [2] add 3 joins the bid.
  expectSide(s[2].tob.pre.bid, true, 1000, 100, "add3 pre");
  expectSide(s[2].tob.post.bid, true, 1000, 130, "add3 post");

  // [3] execution update: cause Execution, touch 130 → 90.
  ASSERT_TRUE(std::holds_alternative<ObUpdateEvent>(s[3].event));
  EXPECT_EQ(as<ObUpdateEvent>(s[3]).cause, MutationCause::Execution);
  EXPECT_EQ(as<ObUpdateEvent>(s[3]).tsNs, kMidnight + kT0 + 4);
  expectSide(s[3].tob.pre.bid, true, 1000, 130, "exec pre");
  expectSide(s[3].tob.post.bid, true, 1000, 90, "exec post");
  EXPECT_FALSE(s[3].unattributed);

  // [4] the companion print: bookNeutral, observed, pre == post == the book now.
  ASSERT_TRUE(std::holds_alternative<ObTradeEvent>(s[4].event));
  const auto& tr = as<ObTradeEvent>(s[4]);
  EXPECT_TRUE(tr.bookNeutral);
  EXPECT_EQ(tr.restingOrderId.value(), 1u);
  EXPECT_EQ(tr.matchNumber.value(), 77u);
  EXPECT_FALSE(s[4].unattributed);
  expectSide(s[4].tob.pre.bid, true, 1000, 90, "print pre");
  expectSide(s[4].tob.post.bid, true, 1000, 90, "print post");
  expectSide(s[4].tob.post.ask, true, 1001, 50, "print post ask");

  // [5] partial cancel: Cancel, 90 → 80.
  EXPECT_EQ(as<ObUpdateEvent>(s[5]).cause, MutationCause::Cancel);
  expectSide(s[5].tob.pre.bid, true, 1000, 90, "cancel pre");
  expectSide(s[5].tob.post.bid, true, 1000, 80, "cancel post");

  // [6] delete order 2: Cancel, ask disappears.
  ASSERT_TRUE(std::holds_alternative<ObDeleteEvent>(s[6].event));
  EXPECT_EQ(as<ObDeleteEvent>(s[6]).cause, MutationCause::Cancel);
  expectSide(s[6].tob.pre.ask, true, 1001, 50, "delete pre ask");
  expectSide(s[6].tob.post.ask, false, 0, 0, "delete post ask");

  // [7] replace delete half: Replace, bid 80 → 20 (order 3's 20 remain).
  EXPECT_EQ(as<ObDeleteEvent>(s[7]).cause, MutationCause::Replace);
  EXPECT_EQ(as<ObDeleteEvent>(s[7]).orderId, 1u);
  expectSide(s[7].tob.pre.bid, true, 1000, 80, "replace-del pre");
  expectSide(s[7].tob.post.bid, true, 1000, 20, "replace-del post");

  // [8] replace add half: carries the original id; new best bid 1002x60.
  EXPECT_EQ(as<ObAddEvent>(s[8]).replacesOrderId.value(), 1u);
  expectSide(s[8].tob.pre.bid, true, 1000, 20, "replace-add pre");
  expectSide(s[8].tob.post.bid, true, 1002, 60, "replace-add post");

  // The ITCH execution consumed the level exactly once (ENC-785/807's C1),
  // now checkable end-to-end: 100+30 -40 -10 -60(replaced) = 20 at 10.00.
  std::vector<std::pair<double, uint64_t>> bids, asks;
  r.mgr.depthN("FLOW", 5, bids, asks);
  ASSERT_EQ(bids.size(), 2u);
  EXPECT_EQ(bids[1].second, 20u);
}

// ---------------------------------------------------------------------------
// Observers run after the mutator returned: never inside a subscribeDeltas
// handler, and only after that event's delta was published.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, ObserverRunsAfterTheDeltaNeverInsideTheHandler) {
  Rig r;
  r.itchFrame(add(1, "B", 100, 10.00, kT0));
  r.itchFrame(add(2, "B", 10, 10.00, kT0));
  ASSERT_EQ(r.rec->seen.size(), 2u);
  for (size_t i = 0; i < r.rec->seen.size(); ++i) {
    EXPECT_FALSE(r.rec->seen[i].insideDeltaHandler) << i;
    EXPECT_EQ(r.rec->seen[i].deltasSoFar, i + 1) << "event " << i << "'s delta precedes its observation";
  }
}

// ---------------------------------------------------------------------------
// Nothing is observed for an event the manager rejected.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, RejectedEventsAreNotObserved) {
  Rig r;
  r.mgr.setTickSize("FLOW", 0.01);
  std::vector<FeedEvent> frame;
  frame.emplace_back(ObUpdateEvent{"FLOW", 999, std::nullopt, uint64_t{5}, std::nullopt,
                                   MutationCause::Cancel, 1});          // unknown order
  frame.emplace_back(ObDeleteEvent{"FLOW", 998, std::nullopt, MutationCause::Cancel, 1});  // unknown
  frame.emplace_back(ObAddEvent{"FLOW", 1, Side::Bid, 10.005, 100, 0, std::nullopt, 0, std::nullopt});  // off-grid
  frame.emplace_back(ObAddEvent{"FLOW", 2, Side::Bid, 10.00, 0, 0, std::nullopt, 0, std::nullopt});     // zero size
  frame.emplace_back(trade(10.00, 5, Aggressor::Sell, false));                                           // nothing to consume
  auto res = r.applier.apply(frame);
  EXPECT_EQ(res.applied, 0u);
  EXPECT_EQ(res.observed, 0u);
  EXPECT_TRUE(r.rec->seen.empty());
}

// ---------------------------------------------------------------------------
// A stale symbol (after a reset, before a snapshot) is gated: no observation,
// not even for a bookNeutral print. The reset itself reaches onBookReset.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, StaleGatedEventsAreNotObserved) {
  Rig r;
  r.itchFrame(add(1, "B", 100, 10.00, kT0));
  ASSERT_EQ(r.rec->seen.size(), 1u);

  std::vector<FeedEvent> reset;
  reset.emplace_back(ObResetEvent{"FLOW", 7});
  r.applier.apply(reset);
  ASSERT_TRUE(r.mgr.isStale("FLOW"));
  ASSERT_EQ(r.rec->resets.size(), 1u);
  EXPECT_EQ(r.rec->resets[0].epoch, 7u);

  std::vector<FeedEvent> frame;
  frame.emplace_back(ObAddEvent{"FLOW", 2, Side::Bid, 10.00, 50, 0, std::nullopt, 0, std::nullopt});
  frame.emplace_back(trade(10.00, 5, Aggressor::Sell, /*bookNeutral=*/true));
  frame.emplace_back(trade(10.00, 5, Aggressor::Sell, false));
  auto res = r.applier.apply(frame);
  EXPECT_EQ(res.applied, 0u);
  EXPECT_EQ(res.observed, 0u);
  EXPECT_EQ(r.rec->seen.size(), 1u);
}

// ---------------------------------------------------------------------------
// A seq gap drops the whole frame's book events: nothing applied or observed.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, SeqDroppedFrameIsNotObserved) {
  Rig r;
  auto f1 = r.itchFrame(R"({"type":"add_order","stock":"FLOW","orderRef":1,"side":"B","shares":100,"price":10.0,"seq":1})");
  EXPECT_FALSE(f1.dropBook);
  ASSERT_EQ(r.rec->seen.size(), 1u);

  // seq 3 after 1: gap. The replace frame (delete + add) is dropped whole.
  auto f3 = r.itchFrame(R"({"type":"order_replace","origOrderRef":1,"orderRef":2,"shares":10,"price":10.01,"seq":3})");
  EXPECT_TRUE(f3.dropBook);
  EXPECT_EQ(f3.applied, 0u);
  EXPECT_EQ(f3.observed, 0u);
  EXPECT_EQ(r.rec->seen.size(), 1u);
  EXPECT_EQ(r.mgr.bestBidSize("FLOW"), 100u);   // book untouched

  // dropBook covers the whole frame, not only the gapped symbol: a
  // bookNeutral print for a healthy symbol riding in a dropped frame is not
  // observed either (it would be, were only per-symbol staleness checked).
  std::vector<FeedEvent> mixed;
  mixed.emplace_back(ObAddEvent{"FLOW", 9, Side::Bid, 10.00, 1, 0, uint64_t{10}, 0, std::nullopt});
  ObTradeEvent other = trade(10.00, 3, Aggressor::Buy, /*bookNeutral=*/true);
  other.symbol = "OTHER";
  mixed.emplace_back(other);
  auto fm = r.applier.apply(mixed);
  EXPECT_TRUE(fm.dropBook);
  EXPECT_FALSE(r.mgr.isStale("OTHER"));
  EXPECT_EQ(fm.observed, 0u);
  EXPECT_EQ(r.rec->seen.size(), 1u);

  // And the symbol is now stale, so even an unsequenced frame is gated.
  auto f4 = r.itchFrame(add(5, "B", 10, 10.00, kT0));
  EXPECT_EQ(f4.observed, 0u);
  EXPECT_EQ(r.rec->seen.size(), 1u);
}

// ---------------------------------------------------------------------------
// FeedServer JSON lines through the same seam. `ob trade` consumes the book
// via applyTrade with no per-order event: observed with unattributed=true.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, FeedServerJsonLinesObservedTradeIsUnattributed) {
  Rig r;
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"ticksize","tickSize":0.01})");
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"add","id":1,"side":"bid","price":10.00,"size":100,"tsNs":111})");
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"add","id":2,"side":"ask","price":10.02,"size":40})");
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"trade","price":10.02,"size":15,"aggressor":"buy","tsNs":222})");
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"update","id":1,"size":60,"cause":"cancel","tsNs":333})");
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"delete","id":2,"cause":"execution","tsNs":444})");

  const auto& s = r.rec->seen;
  ASSERT_EQ(s.size(), 5u);   // ticksize is not a book event

  EXPECT_EQ(as<ObAddEvent>(s[0]).tsNs, 111u);
  expectSide(s[0].tob.post.bid, true, 1000, 100, "json add1");

  ASSERT_TRUE(std::holds_alternative<ObTradeEvent>(s[2].event));
  EXPECT_TRUE(s[2].unattributed);
  EXPECT_FALSE(as<ObTradeEvent>(s[2]).bookNeutral);
  EXPECT_EQ(as<ObTradeEvent>(s[2]).timestampNs, 222u);
  expectSide(s[2].tob.pre.ask, true, 1002, 40, "trade pre");
  expectSide(s[2].tob.post.ask, true, 1002, 25, "trade post");

  EXPECT_FALSE(s[3].unattributed);
  EXPECT_EQ(as<ObUpdateEvent>(s[3]).cause, MutationCause::Cancel);
  EXPECT_EQ(as<ObUpdateEvent>(s[3]).tsNs, 333u);
  expectSide(s[3].tob.post.bid, true, 1000, 60, "update post");

  EXPECT_EQ(as<ObDeleteEvent>(s[4]).cause, MutationCause::Execution);
  EXPECT_EQ(as<ObDeleteEvent>(s[4]).tsNs, 444u);
  expectSide(s[4].tob.post.ask, false, 0, 0, "delete post");

  // A trade with nothing at its price consumes nothing: not observed.
  r.jsonLine(R"({"type":"ob","symbol":"FLOW","action":"trade","price":10.05,"size":1,"aggressor":"buy"})");
  EXPECT_EQ(s.size(), 5u);
}

// ---------------------------------------------------------------------------
// The translation step: required/optional fields and what each failure is.
// ---------------------------------------------------------------------------
TEST(ObJsonTranslatorTest, StatusesAndOptionalFields) {
  auto tr = [](const char* line) {
    rapidjson::Document d;
    d.Parse(line);
    return translateObJson(d);
  };
  using St = ObJsonFrame::Status;

  EXPECT_EQ(tr(R"({"type":"ob","action":"add"})").status, St::BadHeader);
  EXPECT_EQ(tr(R"({"type":"ob","symbol":"","action":"add"})").status, St::BadHeader);
  EXPECT_EQ(tr(R"({"type":"ob","symbol":"X"})").status, St::BadHeader);
  EXPECT_FALSE(tr(R"({"type":"ob","symbol":"","action":"add","seq":4})").seq.has_value());

  auto body = tr(R"({"type":"ob","symbol":"X","action":"add","id":1,"seq":4})");
  EXPECT_EQ(body.status, St::BadBody);
  EXPECT_EQ(body.seq.value(), 4u);   // header fine: FeedServer still gates the seq

  auto unk = tr(R"({"type":"ob","symbol":"X","action":"frob","seq":5})");
  EXPECT_EQ(unk.status, St::UnknownAction);
  EXPECT_EQ(unk.action, "frob");

  auto up = tr(R"({"type":"ob","symbol":"X","action":"update","id":3,"price":1.5,"cause":"replace","tsNs":9,"seq":6})");
  ASSERT_EQ(up.status, St::Ok);
  const auto& u = std::get<ObUpdateEvent>(up.events.at(0));
  EXPECT_EQ(u.cause, MutationCause::Replace);
  EXPECT_EQ(u.tsNs, 9u);
  EXPECT_EQ(u.seq.value(), 6u);
  EXPECT_DOUBLE_EQ(u.newPrice.value(), 1.5);
  EXPECT_FALSE(u.newSize.has_value());

  // Optional fields of the wrong type are ignored, like "priority" always was.
  auto lax = tr(R"({"type":"ob","symbol":"X","action":"delete","id":3,"cause":"fill","tsNs":"soon"})");
  ASSERT_EQ(lax.status, St::Ok);
  EXPECT_EQ(std::get<ObDeleteEvent>(lax.events.at(0)).cause, MutationCause::Unknown);
  EXPECT_EQ(std::get<ObDeleteEvent>(lax.events.at(0)).tsNs, 0u);

  auto add = tr(R"({"type":"ob","symbol":"X","action":"add","id":1,"side":"sell","price":2,"size":3})");
  ASSERT_EQ(add.status, St::Ok);
  EXPECT_EQ(std::get<ObAddEvent>(add.events.at(0)).side, Side::Bid);   // only "ask" is ask

  auto trd = tr(R"({"type":"ob","symbol":"X","action":"trade","price":2,"size":3,"aggressor":"sell"})");
  ASSERT_EQ(trd.status, St::Ok);
  EXPECT_FALSE(std::get<ObTradeEvent>(trd.events.at(0)).bookNeutral);
  EXPECT_EQ(std::get<ObTradeEvent>(trd.events.at(0)).aggressor, Aggressor::Sell);
}

// ---------------------------------------------------------------------------
// With no observer the seam applies identically and computes no TobChange;
// the TobChange out-param on the manager is filled only when asked.
// ---------------------------------------------------------------------------
TEST(IngressApplierTest, NoObserverAppliesIdentically) {
  OrderBookManager a, b;
  auto reg = std::make_shared<FlowObserverRegistry>();
  auto rec = std::make_shared<Recorder>();
  reg->add(rec);
  IngressApplier plain(nullptr, &a, nullptr);
  IngressApplier watched(nullptr, &b, reg);
  ItchAdapter ia = clockedAdapter(), ib = clockedAdapter();

  const std::vector<std::string> msgs = {
    add(1, "B", 100, 10.00, kT0), add(2, "S", 50, 10.01, kT0),
    R"({"type":"order_executed","orderRef":2,"shares":50,"matchNumber":1})",
    R"({"type":"order_cancel","orderRef":1,"shares":25})",
  };
  size_t appliedA = 0, appliedB = 0;
  for (const auto& m : msgs) {
    auto fa = ia.translate(m); appliedA += plain.apply(fa).applied;
    auto fb = ib.translate(m); appliedB += watched.apply(fb).applied;
  }
  EXPECT_EQ(appliedA, appliedB);
  EXPECT_EQ(a.dumpLadder("FLOW"), b.dumpLadder("FLOW"));
  EXPECT_EQ(rec->seen.size(), 5u);   // 4 mutations + 1 bookNeutral print

  // Registry removal stops observation.
  reg->remove(rec.get());
  EXPECT_TRUE(reg->empty());
  auto fb = ib.translate(add(3, "B", 1, 10.00, kT0));
  EXPECT_EQ(watched.apply(fb).observed, 0u);
  EXPECT_EQ(rec->seen.size(), 5u);
}

TEST(OrderBookManagerTobTest, OutParamUntouchedWhenGatedFilledWhenApplied) {
  OrderBookManager m;
  m.setTickSize("S", 0.5);
  TobChange tc;
  tc.pre.bid.size = 12345;   // sentinel
  EXPECT_FALSE(m.onAdd("S", 1, Side::Bid, 1.25, 10, 0, {}, false, &tc));   // off-grid: rejected
  EXPECT_EQ(tc.pre.bid.size, 12345u);

  EXPECT_TRUE(m.onAdd("S", 1, Side::Bid, 1.5, 10, 0, {}, false, &tc));
  EXPECT_FALSE(tc.pre.bid.present);
  EXPECT_TRUE(tc.post.bid.present);
  EXPECT_EQ(tc.post.bid.price.ticks, 3);
  EXPECT_DOUBLE_EQ(tc.post.bid.priceD, 1.5);
  EXPECT_EQ(tc.post.bid.size, 10u);

  // A mutator that reaches the book but changes nothing: post == pre.
  TobChange none;
  EXPECT_FALSE(m.onDelete("S", 77, FeedScope{}, false, &none));
  EXPECT_TRUE(none.pre.bid.present);
  EXPECT_EQ(none.post.bid.size, none.pre.bid.size);

  auto t = m.topOfBook("S");
  EXPECT_TRUE(t.bid.present);
  EXPECT_FALSE(t.ask.present);
  EXPECT_FALSE(m.topOfBook("nope").bid.present);
}

// ---------------------------------------------------------------------------
// The real FeedServer over TCP, observer registered: lines reach the book and
// the observer through the seam, and the pre-ENC-1645 seq behaviour holds — a
// bad body still consumes its seq, a gap drops the line and goes stale.
// ---------------------------------------------------------------------------
namespace {

class LockedRecorder final : public IFlowObserver {
public:
  void onFlowEvent(const FlowObservation& o) override {
    std::lock_guard<std::mutex> lk(mu);
    seen.push_back(Seen{o.event, o.tob, o.unattributed, false, 0});
  }
  size_t count() { std::lock_guard<std::mutex> lk(mu); return seen.size(); }
  std::mutex        mu;
  std::vector<Seen> seen;
};

unsigned short freePort() {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor a(io, {boost::asio::ip::tcp::v4(), 0});
  return a.local_endpoint().port();
}

} // namespace

TEST(IngressApplierTest, FeedServerOverTcpRoutesThroughTheSeam) {
  OrderBookManager mgr;
  auto reg = std::make_shared<FlowObserverRegistry>();
  auto rec = std::make_shared<LockedRecorder>();
  reg->add(rec);

  boost::asio::io_context ioc;
  const unsigned short port = freePort();
  FeedServer server(ioc, nullptr, &mgr, port, reg);
  server.run();
  std::thread io([&] { ioc.run(); });

  boost::asio::io_context cio;
  boost::asio::ip::tcp::socket sock(cio);
  sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});
  const std::string lines =
      R"({"type":"ob","symbol":"TCP","action":"ticksize","tickSize":0.01})" "\n"
      R"({"type":"ob","symbol":"TCP","action":"add","id":1,"side":"bid","price":5.00,"size":10,"seq":1})" "\n"
      R"({"type":"ob","symbol":"TCP","action":"add","id":2,"seq":2})" "\n"   // bad body, seq consumed
      R"({"type":"ob","symbol":"TCP","action":"add","id":3,"side":"ask","price":5.01,"size":7,"seq":3})" "\n"
      R"({"type":"ob","symbol":"TCP","action":"trade","price":5.01,"size":2,"aggressor":"buy","seq":4})" "\n"
      R"({"type":"ob","symbol":"TCP","action":"delete","id":1,"cause":"cancel","tsNs":5,"seq":5})" "\n";
  boost::asio::write(sock, boost::asio::buffer(lines));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (rec->count() < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // A gap (seq 9 after 5): dropped, symbol stale, nothing observed.
  boost::asio::write(sock, boost::asio::buffer(std::string(
      R"({"type":"ob","symbol":"TCP","action":"add","id":4,"side":"bid","price":5.00,"size":1,"seq":9})" "\n")));
  const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!mgr.isStale("TCP") && std::chrono::steady_clock::now() < d2)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  sock.close();
  server.stop();
  ioc.stop();
  io.join();

  ASSERT_EQ(rec->seen.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<ObAddEvent>(rec->seen[0].event));
  EXPECT_EQ(std::get<ObAddEvent>(rec->seen[1].event).orderId, 3u);
  EXPECT_TRUE(rec->seen[2].unattributed);
  EXPECT_EQ(rec->seen[2].tob.pre.ask.size, 7u);
  EXPECT_EQ(rec->seen[2].tob.post.ask.size, 5u);
  EXPECT_EQ(std::get<ObDeleteEvent>(rec->seen[3].event).cause, MutationCause::Cancel);
  EXPECT_EQ(std::get<ObDeleteEvent>(rec->seen[3].event).tsNs, 5u);

  EXPECT_TRUE(mgr.isStale("TCP"));
  EXPECT_FALSE(mgr.bestBid("TCP").has_value());     // id 1 deleted, id 4 never applied
  EXPECT_EQ(mgr.bestAskSize("TCP"), 5u);
  EXPECT_EQ(mgr.getFeedState("TCP").lastSeq, 5u);
}

TEST(IngressApplierTest, MarketConnectorOwnsAFlowObserverRegistry) {
  krn::market::MarketConnector c;
  ASSERT_TRUE(c.flowObservers());
  EXPECT_TRUE(c.flowObservers()->empty());
  auto rec = std::make_shared<Recorder>();
  c.flowObservers()->add(rec);
  EXPECT_FALSE(c.flowObservers()->empty());
}

// ---------------------------------------------------------------------------
// ENC-1645 review F2: an ITCH E print (bookNeutral, restingOrderId set) whose
// Execution mutation in the same frame was rejected is an orphan: the book
// never took the fill, so observing the print would count a fill flow never
// saw. It is suppressed and counted in feed.flow_orphan_exec.
// ---------------------------------------------------------------------------
namespace {
double counter(const char* name) {
  auto c = krn::util::MetricRegistry::instance().snapshotCounters();
  auto it = c.find(name);
  return it == c.end() ? 0.0 : it->second;
}
ObTradeEvent execPrint(uint64_t restingId, uint64_t match) {
  return ObTradeEvent{"FLOW", 10.00, 5, Aggressor::Sell, 1, /*bookNeutral=*/true,
                      std::nullopt, restingId, match};
}
} // namespace

TEST(IngressApplierTest, OrphanedExecutionPrintIsNotObserved) {
  Rig r;
  r.mgr.setTickSize("FLOW", 0.01);
  const double before = counter("feed.flow_orphan_exec");
  std::vector<FeedEvent> frame;
  frame.emplace_back(ObUpdateEvent{"FLOW", 42, std::nullopt, uint64_t{5}, std::nullopt,
                                   MutationCause::Execution, 1});   // unknown order: rejected
  frame.emplace_back(execPrint(42, 7));
  auto res = r.applier.apply(frame);
  EXPECT_EQ(res.applied, 0u);
  EXPECT_EQ(res.observed, 0u);
  EXPECT_TRUE(r.rec->seen.empty())
      << "a print whose Execution mutation was rejected was observed";
  EXPECT_EQ(counter("feed.flow_orphan_exec"), before + 1);

  // Same for a full-fill delete of an unknown order.
  std::vector<FeedEvent> frame2;
  frame2.emplace_back(ObDeleteEvent{"FLOW", 43, std::nullopt, MutationCause::Execution, 1});
  frame2.emplace_back(execPrint(43, 8));
  EXPECT_EQ(r.applier.apply(frame2).observed, 0u);
  EXPECT_EQ(counter("feed.flow_orphan_exec"), before + 2);
}

TEST(IngressApplierTest, ExecutionPrintIsObservedWhenItsMutationApplied) {
  Rig r;
  r.mgr.setTickSize("FLOW", 0.01);
  r.itchFrame(add(1, "B", 100, 10.00, kT0));
  r.rec->seen.clear();
  const double before = counter("feed.flow_orphan_exec");
  // A rejected Execution for another order in the same frame does not
  // orphan this one's print.
  std::vector<FeedEvent> frame;
  frame.emplace_back(ObUpdateEvent{"FLOW", 99, std::nullopt, uint64_t{5}, std::nullopt,
                                   MutationCause::Execution, 1});   // rejected
  frame.emplace_back(ObUpdateEvent{"FLOW", 1, std::nullopt, uint64_t{95}, std::nullopt,
                                   MutationCause::Execution, 1});   // applied
  frame.emplace_back(execPrint(1, 9));
  auto res = r.applier.apply(frame);
  EXPECT_EQ(res.applied, 1u);
  ASSERT_EQ(r.rec->seen.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<ObUpdateEvent>(r.rec->seen[0].event));
  const auto& tr = as<ObTradeEvent>(r.rec->seen[1]);
  EXPECT_EQ(tr.restingOrderId.value(), 1u);
  // The print's tob is the post-fill book (pre == post); the fill's touch is
  // the Execution's TobChange.pre.
  expectSide(r.rec->seen[1].tob.pre.bid, true, 1000, 95, "print pre = post-fill");
  expectSide(r.rec->seen[1].tob.post.bid, true, 1000, 95, "print post");
  expectSide(r.rec->seen[0].tob.pre.bid, true, 1000, 100, "execution pre = fill's touch");
  EXPECT_EQ(counter("feed.flow_orphan_exec"), before);
}

TEST(IngressApplierTest, PrintWithNoMutationInItsFrameIsObserved) {
  // An adapter that emits the print before (or without) the book change —
  // the Databento MBO ordering, where the C follows the T+F pair — is not
  // orphaned: only a mutation the book rejected in this frame orphans.
  Rig r;
  r.mgr.setTickSize("FLOW", 0.01);
  r.itchFrame(add(1, "B", 100, 10.00, kT0));
  r.rec->seen.clear();
  std::vector<FeedEvent> frame;
  frame.emplace_back(execPrint(1, 10));
  EXPECT_EQ(r.applier.apply(frame).observed, 1u);
  // A hidden (P) print, no restingOrderId, is observed as before.
  std::vector<FeedEvent> hidden;
  hidden.emplace_back(trade(10.00, 3, Aggressor::Sell, true));
  EXPECT_EQ(r.applier.apply(hidden).observed, 1u);
}

// ---------------------------------------------------------------------------
// ENC-1645 review F3: OrderBookManager::topOfBook called from a
// subscribeDeltas handler used to self-deadlock (symbolLock_ is held across
// the delta publish). It now reads the book without re-locking and sees the
// post-mutation state. A re-entrant *mutation* of the same symbol from a
// handler throws std::logic_error instead of hanging. Each case runs on a
// detached thread under a timeout so a regression fails rather than hangs
// (the manager is leaked on purpose in that case).
// ---------------------------------------------------------------------------
TEST(OrderBookManagerTobTest, TopOfBookFromADeltaHandlerDoesNotDeadlock) {
  auto* mgr = new OrderBookManager();
  auto seen = std::make_shared<std::promise<TopOfBook>>();
  auto fut = seen->get_future();
  auto once = std::make_shared<std::atomic<bool>>(false);
  mgr->subscribeDeltas("S", [mgr, seen, once](const BookDelta&) {
    auto t = mgr->topOfBook("S");
    if (!once->exchange(true)) seen->set_value(t);
  });
  auto other = std::make_shared<std::promise<TopOfBook>>();
  auto otherFut = other->get_future();
  std::thread([mgr, other] {
    mgr->onAdd("S", 1, Side::Bid, 10.0, 5, 0);
    // Outside any handler, topOfBook still takes the lock as before.
    other->set_value(mgr->topOfBook("S"));
  }).detach();
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "topOfBook inside a subscribeDeltas handler deadlocked";
  auto t = fut.get();
  EXPECT_TRUE(t.bid.present);           // the post-mutation book
  EXPECT_EQ(t.bid.size, 5u);
  ASSERT_EQ(otherFut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(otherFut.get().bid.size, 5u);
  delete mgr;
}

TEST(OrderBookManagerTobTest, TopOfBookOfAnotherSymbolFromADeltaHandlerLocks) {
  // The handler for S reads T: T's lock is not held, so it is taken.
  auto* mgr = new OrderBookManager();
  mgr->onAdd("T", 1, Side::Ask, 11.0, 7, 0);
  auto seen = std::make_shared<std::promise<uint64_t>>();
  auto fut = seen->get_future();
  auto once = std::make_shared<std::atomic<bool>>(false);
  mgr->subscribeDeltas("S", [mgr, seen, once](const BookDelta&) {
    auto t = mgr->topOfBook("T");
    if (!once->exchange(true)) seen->set_value(t.ask.size);
  });
  auto done = std::make_shared<std::promise<void>>();
  auto doneFut = done->get_future();
  std::thread([mgr, done] {
    mgr->onAdd("S", 1, Side::Bid, 10.0, 5, 0);
    done->set_value();
  }).detach();
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(fut.get(), 7u);
  ASSERT_EQ(doneFut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  delete mgr;
}

TEST(OrderBookManagerTobTest, MutationFromADeltaHandlerOfTheSameSymbolThrows) {
  auto* mgr = new OrderBookManager();
  auto reentered = std::make_shared<std::atomic<bool>>(false);
  mgr->subscribeDeltas("S", [mgr, reentered](const BookDelta&) {
    if (reentered->exchange(true)) return;
    mgr->onAdd("S", 2, Side::Bid, 9.0, 1, 0);   // same symbol: must not hang
  });
  auto outcome = std::make_shared<std::promise<std::string>>();
  auto fut = outcome->get_future();
  std::thread([mgr, outcome] {
    try {
      mgr->onAdd("S", 1, Side::Bid, 10.0, 5, 0);
      outcome->set_value("no throw");
    } catch (const std::logic_error& e) {
      outcome->set_value(e.what());
    }
  }).detach();
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "a same-symbol mutation from a delta handler deadlocked";
  EXPECT_NE(fut.get().find("subscribeDeltas handler"), std::string::npos);
  // The outer mutation was applied; the re-entrant one was not.
  EXPECT_EQ(mgr->topOfBook("S").bid.size, 5u);
  delete mgr;
}
