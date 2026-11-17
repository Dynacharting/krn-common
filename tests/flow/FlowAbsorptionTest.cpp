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

// A book reset ends both tenures.
TEST(FlowAbsorptionTest, ResetEndsTheTenure) {
  Rig r;
  thin(r);
  r.frame({trade(10.00, 150, Aggressor::Sell, true, kTs)});   // E = 150 of D0 100
  r.frame({feed::ObResetEvent{kSym, 2}});
  r.take();
  r.frame({trade(10.00, 100, Aggressor::Sell, true, kTs + 2 * kUs)});
  EXPECT_EQ(p3(r), M{}) << "E restarted at the reset: 100 < 200";
  r.frame({trade(10.00, 100, Aggressor::Sell, true, kTs + 3 * kUs)});
  EXPECT_EQ(p3(r), (M{{A::kBidVolume, 200}, {A::kBidRatio, 2.0}, {A::kBidPx, 10.00}}))
      << "positive control: prints after the reset are observed, against a fresh tenure";
}
