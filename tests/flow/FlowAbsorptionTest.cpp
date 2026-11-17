// ENC-1648 (SPEC 2026-11-17-order-flow-pipelines D5 P3): absorption and
// refill on hand-built frames driven through the real IngressApplier into a
// real OrderBookManager, observed by the real FlowComponent with its default
// operator set. Only P3's keys are compared (the other operators publish on
// the same frames).
#include "FlowTestRig.hpp"

#include "krn/flow/AbsorptionOperator.hpp"

#include <gtest/gtest.h>

using namespace krn;
using namespace krn::flowtest;
using M = std::map<std::string, double>;
using A = flow::AbsorptionOperator;

namespace {

// P3's keys only, from everything notified since the last call.
M p3(Rig& r) {
  M out;
  for (const auto& [k, v] : r.take())
    if (k.rfind("flow.absorb.", 0) == 0 || k.rfind("flow.refill.", 0) == 0) out[k] = v;
  return out;
}

// How many times `key` was notified since the last take() (does not clear).
int notifies(const Rig& r, const std::string& key) {
  int n = 0;
  for (const auto& [k, v] : r.log) n += k == key;
  return n;
}

// A one-order-a-side book: bid 10.00 = #1 100, ask 10.03 = #4 100.
void thin(Rig& r) {
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(4, Side::Ask, 10.03, 100)});
  r.take();
}

constexpr uint64_t kUs = 1'000;
constexpr uint64_t kMs = 1'000'000;

} // namespace

// The iceberg shape feedsim's regime produces: two displayed 100-share slices
// at a new best bid; each time one is executed away a new slice is added at
// the same price microseconds later. D0 is the level's displayed size when it
// became the touch (the first slice, 100). E reaches 2 x D0 on the second
// execution while the level holds, so absorption fires there — once — and
// every reload is refill.
TEST(FlowAbsorptionTest, RefillingIcebergFiresOnce) {
  Rig r;
  thin(r);
  r.frame({add(10, Side::Bid, 10.01, 100, kTs)});         // new touch: tenure, D0 = 100
  r.frame({add(11, Side::Bid, 10.01, 100, kTs + kUs)});   // second slice: not a refill (no execution yet)
  EXPECT_EQ(p3(r), M{});

  uint64_t t = kTs + kMs;
  // Slice 10 executed away; its E print; reload 12.
  r.frame({del(10, MutationCause::Execution, t),
           trade(10.01, 100, Aggressor::Sell, /*bookNeutral=*/true, t, /*resting=*/10)});
  EXPECT_EQ(p3(r), M{}) << "E = 100 < 2 x 100 (an E print counted again would make it 200 and fire)";
  r.frame({add(12, Side::Bid, 10.01, 100, t + 3 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 100}}));

  t += kMs;
  r.frame({del(11, MutationCause::Execution, t),
           trade(10.01, 100, Aggressor::Sell, true, t, 11)});
  EXPECT_EQ(notifies(r, A::kBidVolume), 1);
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.01}}));
  r.frame({add(13, Side::Bid, 10.01, 100, t + 2 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 100}}));

  // Keeps absorbing: partial and full executions, more reloads — no second fire.
  t += kMs;
  r.frame({upd(12, 40, MutationCause::Execution, t), trade(10.01, 60, Aggressor::Sell, true, t, 12)});
  r.frame({del(13, MutationCause::Execution, t + kUs), trade(10.01, 100, Aggressor::Sell, true, t + kUs, 13)});
  r.frame({add(14, Side::Bid, 10.01, 100, t + 4 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 100}})) << "absorption fires once per tenure";
}

// Hidden (ITCH P) prints at the touch with no displayed execution and no
// refill: E counts the prints, so the level absorbs with D0 untouched.
// The aggressor field is deliberately the "wrong" one (Buy for a print the
// bid absorbed — what real ITCH's resting-side P encoding yields through the
// adapter, ENC-1665): the side comes from the price.
TEST(FlowAbsorptionTest, HiddenPrintAbsorptionWithoutRefill) {
  Rig r;
  thin(r);
  r.frame({trade(10.00, 150, Aggressor::Buy, /*bookNeutral=*/true, kTs)});
  EXPECT_EQ(p3(r), M{}) << "E = 150 < 200";
  r.frame({trade(9.98, 500, Aggressor::Sell, true, kTs + kUs)});
  r.frame({trade(10.02, 500, Aggressor::Sell, true, kTs + kUs)});
  EXPECT_EQ(p3(r), M{}) << "prints away from both touches absorb nothing";
  r.frame({trade(10.00, 50, Aggressor::Unknown, true, kTs + 2 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.00}}));

  // Ask mirror: D0 = 100 at 10.03.
  r.frame({trade(10.03, 250, Aggressor::Sell, true, kTs + 3 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kAskVolume, 250}, {A::kAskRatio, 2.5}, {A::kAskPx, 10.03}}));
}

// A level that reaches the threshold on the event that takes it — and a level
// that is traded through — does not fire; the next level is a new tenure.
TEST(FlowAbsorptionTest, TradedThroughLevelDoesNotFire) {
  Rig r;
  thin(r);
  r.frame({add(2, Side::Bid, 9.99, 100)});
  r.take();
  r.frame({trade(10.00, 150, Aggressor::Sell, true, kTs)});            // hidden: E = 150
  r.frame({del(1, MutationCause::Execution, kTs + kUs),                 // E = 250 >= 200, level gone
           trade(10.00, 100, Aggressor::Sell, true, kTs + kUs, 1)});
  EXPECT_EQ(p3(r), M{}) << "the threshold is crossed only as the level goes";
  // 9.99 is a new tenure with D0 = 100 and E = 0: nothing carried over.
  r.frame({trade(9.99, 150, Aggressor::Sell, true, kTs + 2 * kUs)});
  EXPECT_EQ(p3(r), M{}) << "E of the old tenure does not carry into the new one";
  // An unattributed sweep through 9.99 (consumes it whole): no fire either.
  r.frame({trade(9.99, 100, Aggressor::Sell, /*bookNeutral=*/false, kTs + 3 * kUs)});
  EXPECT_EQ(p3(r), M{});
}

// The refill window is [exec, exec + windowNs]: an add one ns past it is not
// refill; one exactly at it is. Unattributed executions open no window, and a
// replace's add half is not refill.
TEST(FlowAbsorptionTest, RefillOutsideWindowNotCounted) {
  Rig r;
  thin(r);
  r.frame({upd(1, 50, MutationCause::Execution, kTs)});
  r.frame({add(2, Side::Bid, 10.00, 50, kTs + 5 * kMs + 1)});
  EXPECT_EQ(p3(r), M{}) << "5 ms + 1 ns after the execution";
  r.frame({upd(1, 20, MutationCause::Execution, kTs + 10 * kMs)});
  r.frame({add(3, Side::Bid, 10.00, 30, kTs + 15 * kMs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 30}})) << "exactly windowNs: inside";
  r.frame({add(5, Side::Bid, 9.99, 70, kTs + 15 * kMs)});
  EXPECT_EQ(p3(r), M{}) << "an add behind the touch is not refill";
  // A replace's add half at the touch inside the window: shares moved, not reloaded.
  r.frame({upd(1, 10, MutationCause::Execution, kTs + 20 * kMs)});
  r.frame({del(3, MutationCause::Replace, kTs + 20 * kMs), add(6, Side::Bid, 10.00, 30, kTs + 20 * kMs, 3)});
  EXPECT_EQ(p3(r), M{});
  // Unattributed execution at the touch, then an add inside 5 ms of it but
  // more than 5 ms after the last per-order execution: no refill.
  r.frame({trade(10.00, 5, Aggressor::Sell, /*bookNeutral=*/false, kTs + 30 * kMs)});
  r.frame({add(7, Side::Bid, 10.00, 40, kTs + 30 * kMs + kUs)});
  EXPECT_EQ(p3(r), M{}) << "an unattributed execution does not open a refill window";
}

// Ask-side refill, and the iceberg's E print skipped on the ask too.
TEST(FlowAbsorptionTest, AskSideRefillAndAbsorption) {
  Rig r;
  thin(r);
  r.frame({upd(4, 20, MutationCause::Execution, kTs), trade(10.03, 80, Aggressor::Buy, true, kTs, 4)});
  r.frame({add(5, Side::Ask, 10.03, 80, kTs + kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillAsk, 80}}));
  r.frame({upd(5, 0, MutationCause::Execution, kTs + kMs), trade(10.03, 80, Aggressor::Buy, true, kTs + kMs, 5)});
  EXPECT_EQ(p3(r), M{}) << "E = 160 < 200";
  r.frame({upd(4, 0, MutationCause::Execution, kTs + 2 * kMs)});
  EXPECT_EQ(p3(r), M{}) << "E = 180 and the level is gone";
}

// flow.absorb.minRatio and flow.refill.windowNs are read at start (D7).
TEST(FlowAbsorptionTest, ThresholdAndWindowComeFromConfig) {
  Rig r(/*attach=*/false);
  ASSERT_TRUE(r.flow->configSource()->apply("absorb.minRatio", "0.5"));
  ASSERT_TRUE(r.flow->configSource()->apply("refill.windowNs", "1000"));
  r.attach();
  thin(r);
  r.frame({upd(1, 50, MutationCause::Execution, kTs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 50}, {A::kBidRatio, 0.5}, {A::kBidPx, 10.00}}));
  r.frame({add(2, Side::Bid, 10.00, 10, kTs + 1001)});
  EXPECT_EQ(p3(r), M{}) << "window is 1000 ns";
}

// A book reset (FlowComponent::onBookReset -> onReset) ends both tenures.
TEST(FlowAbsorptionTest, ResetEndsTheTenure) {
  Rig r;
  thin(r);
  r.frame({trade(10.00, 150, Aggressor::Sell, true, kTs)});   // E = 150 of D0 100
  // Straight to the observer: through the ingress the manager would also mark
  // the book stale until a snapshot, and no later print would be observed.
  r.flow->onBookReset(feed::ObResetEvent{kSym, 2});
  r.take();
  r.frame({trade(10.00, 100, Aggressor::Sell, true, kTs + 2 * kUs)});
  EXPECT_EQ(p3(r), M{}) << "E restarted at the reset: 100 < 200";
  r.frame({trade(10.00, 100, Aggressor::Sell, true, kTs + 3 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.00}}))
      << "positive control: prints after the reset are observed, against a fresh tenure";
}

// A duplicate-id add (ENC-1646 review F2: delete-then-add of an order already
// resting) re-enters shares; it is not a reload, so not refill.
TEST(FlowAbsorptionTest, DuplicateIdAddIsNotRefill) {
  Rig r;
  thin(r);
  r.frame({upd(1, 50, MutationCause::Execution, kTs)});
  r.frame({add(1, Side::Bid, 10.00, 80, kTs + kUs)});
  EXPECT_EQ(p3(r), M{}) << "same id re-added";
  r.frame({add(2, Side::Bid, 10.00, 30, kTs + 2 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 30}})) << "positive control: a new id inside the window is refill";
}

// An observation older than one already seen (review F1, two ingress strands)
// carries the TobChange of a book that has since moved; it must not end the
// tenure or count toward it.
TEST(FlowAbsorptionTest, ReorderedObservationIsSkipped) {
  Rig r;
  thin(r);
  r.frame({trade(10.00, 150, Aggressor::Sell, true, kTs)});   // E = 150 of D0 100
  r.take();
  const FeedEvent late = trade(9.99, 400, Aggressor::Sell, true, kTs + kUs);
  TobChange stale{};
  stale.pre.bid  = TobLevel{true, r.mgr.toTicks(kSym, 9.99), 9.99, 100};
  stale.pre.ask  = TobLevel{true, r.mgr.toTicks(kSym, 10.03), 10.03, 100};
  stale.post     = stale.pre;
  stale.mutationSeq = 1;   // thin() stamped 1 and 2
  r.flow->onFlowEvent(feed::FlowObservation{late, stale, false});
  EXPECT_EQ(r.stored("flow.meta.reordered"), 1) << "the observation really was reordered";
  EXPECT_EQ(p3(r), M{});
  r.frame({trade(10.00, 50, Aggressor::Sell, true, kTs + 2 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.00}}))
      << "the tenure survived the stale observation";
}

// ── Tenure grace (a lone reserve order reloading across an empty level) ──

// Bid 10.00 = #1 100 alone, 9.99 = #2 100 behind it; ask 10.03 = #4 100.
// Tenure at 10.00 with D0 = 100.
void lone(Rig& r) {
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(2, Side::Bid, 9.99, 100)});
  r.frame({add(4, Side::Ask, 10.03, 100)});
  r.take();
}

// The canonical real-ITCH iceberg: the displayed slice fully executed (the
// level is empty for one event, the touch drops to 9.99), then reloaded at
// the same price under a new order ref microseconds later. The tenure
// continues across the gap; each reload is refill; it fires on the reload
// that re-establishes the level once E >= 2 x D0.
TEST(FlowAbsorptionTest, LoneIcebergReloadAcrossAnEmptyLevel) {
  Rig r;
  lone(r);
  uint64_t t = kTs;
  r.frame({del(1, MutationCause::Execution, t), trade(10.00, 100, Aggressor::Sell, true, t, 1)});
  EXPECT_EQ(p3(r), M{}) << "E = 100; the level is gone (touch 9.99)";
  r.frame({add(3, Side::Bid, 10.00, 100, t + 2 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 100}})) << "the reload is refill; the tenure resumes";
  t += kMs;
  r.frame({del(3, MutationCause::Execution, t), trade(10.00, 100, Aggressor::Sell, true, t, 3)});
  EXPECT_EQ(p3(r), M{}) << "E = 200 crosses as the level empties: no fire yet";
  r.frame({add(5, Side::Bid, 10.00, 100, t + 2 * kUs)});
  EXPECT_EQ(notifies(r, A::kBidVolume), 1);
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 100}, {A::kBidVolume, 200}, {A::kBidRatio, 2.0},
                      {A::kBidPx, 10.00}}))
      << "E carried across both gaps; fires as the reload re-establishes the level";
  t += kMs;
  r.frame({del(5, MutationCause::Execution, t), trade(10.00, 100, Aggressor::Sell, true, t, 5)});
  r.frame({add(6, Side::Bid, 10.00, 100, t + 2 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kRefillBid, 100}})) << "still one tenure: no second fire";
}

// A return to the same price after the grace window is a new tenure: no
// refill, and E starts again from zero.
TEST(FlowAbsorptionTest, ReturnAfterTheWindowIsANewTenure) {
  Rig r;
  lone(r);
  r.frame({del(1, MutationCause::Execution, kTs), trade(10.00, 100, Aggressor::Sell, true, kTs, 1)});
  r.frame({add(3, Side::Bid, 10.00, 100, kTs + 5 * kMs + 1)});
  EXPECT_EQ(p3(r), M{}) << "5 ms + 1 ns after the execution: not refill, no grace";
  r.frame({trade(10.00, 150, Aggressor::Sell, true, kTs + 6 * kMs)});
  EXPECT_EQ(p3(r), M{}) << "new tenure D0 = 100, E = 150 (a carried E would be 250 and fire)";
  r.frame({trade(10.00, 50, Aggressor::Sell, true, kTs + 6 * kMs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.00}}))
      << "positive control: the new tenure absorbs on its own E";
}

// A touch that comes back at a DIFFERENT price is a new tenure; and a better
// touch in front ends the suspended level's grace, so its later return
// within the window is new too.
TEST(FlowAbsorptionTest, ReturnAtADifferentPriceIsANewTenure) {
  Rig r;
  lone(r);
  r.frame({del(1, MutationCause::Execution, kTs), trade(10.00, 100, Aggressor::Sell, true, kTs, 1)});
  r.frame({add(3, Side::Bid, 10.01, 100, kTs + kUs)});   // a better level, not the reload
  r.frame({add(7, Side::Bid, 10.00, 100, kTs + 2 * kUs)});   // behind the touch now
  r.frame({del(3, MutationCause::Cancel, kTs + 3 * kUs)});   // touch back to 10.00, inside 5 ms
  EXPECT_EQ(p3(r), M{}) << "no refill: 10.01 stepped ahead of the suspended level";
  r.frame({trade(10.00, 150, Aggressor::Sell, true, kTs + 4 * kUs)});
  EXPECT_EQ(p3(r), M{}) << "new tenure at 10.00: E = 150 of D0 100 (a resumed one would be 250 and fire)";
  r.frame({trade(10.00, 50, Aggressor::Sell, true, kTs + 5 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.00}}));
}
