// ENC-1649 (SPEC 2026-11-17-order-flow-pipelines D5 P5): the large-order
// lifecycle on hand-built frames through the real IngressApplier,
// OrderBookManager and FlowComponent with its default operator set.
#include "FlowTestRig.hpp"

#include "krn/flow/LargeOrderOperator.hpp"

#include <gtest/gtest.h>

using namespace krn;
using namespace krn::flowtest;
using M = std::map<std::string, double>;
using flow::LargeOrderOperator;
using flow::RollingMedian;

namespace {

const std::string& kFilled = LargeOrderOperator::kFilled;
const std::string& kCanc   = LargeOrderOperator::kCancelled;
const std::string& kLife   = LargeOrderOperator::kLifetimeMs;
const std::string& kEndF   = LargeOrderOperator::kEndFill;
const std::string& kEndC   = LargeOrderOperator::kEndCancel;
const std::string& kActive = LargeOrderOperator::kActive;
constexpr uint64_t kMs = 1'000'000;

M p5(Rig& r) {
  M out;
  for (const auto& [k, v] : r.take())
    if (k.rfind("flow.large.", 0) == 0) out[k] = v;
  return out;
}

} // namespace

TEST(FlowLargeOrderTest, RollingMedianOddEvenAndEviction) {
  RollingMedian m(4);
  EXPECT_EQ(m.median(), 0.0);
  m.push(5);
  EXPECT_EQ(m.median(), 5.0);
  m.push(1);
  EXPECT_EQ(m.median(), 3.0);
  m.push(9);
  EXPECT_EQ(m.median(), 5.0);
  m.push(9);
  EXPECT_TRUE(m.full());
  EXPECT_EQ(m.median(), 7.0);   // {1,5,9,9}
  m.push(2);                    // evicts 5 -> {1,2,9,9}
  EXPECT_EQ(m.median(), 5.5);
  m.push(2);                    // evicts 1 -> {2,2,9,9}
  m.push(2);                    // evicts 9 -> {2,2,2,9}
  EXPECT_EQ(m.median(), 2.0);
  EXPECT_EQ(m.size(), 4u);
  // The threshold: minShares until full, then max(minShares, mult x median).
  RollingMedian w(3);
  w.push(1000); w.push(1000);
  EXPECT_EQ(LargeOrderOperator::threshold(w, 5000, 10.0), 5000.0) << "warm-up";
  w.push(1000);
  EXPECT_EQ(LargeOrderOperator::threshold(w, 5000, 10.0), 10000.0);
  EXPECT_EQ(LargeOrderOperator::threshold(w, 20000, 10.0), 20000.0);
}

// size >= minShares (default 5000) during warm-up: 4999 is not large, 5000 is.
TEST(FlowLargeOrderTest, MinSharesEdge) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 4999)});
  EXPECT_EQ(r.stored(kActive), 0) << "stored from the first event";
  r.frame({add(2, Side::Bid, 10.00, 5000)});
  EXPECT_EQ(r.stored(kActive), 1);
  r.take();
  r.frame({del(1, MutationCause::Cancel)});
  EXPECT_EQ(p5(r), M{}) << "4999 was never large";
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(p5(r), (M{{kCanc, 5000}, {kLife, 0}, {kEndC, 1}}))
      << "lifetime 0 is still notified (a level)";
  EXPECT_EQ(r.stored(kActive), 0);
  EXPECT_FALSE(r.store.get(kSym, kEndC).has_value()) << "pushed, never stored";
}

TEST(FlowLargeOrderTest, MinSharesIsConfigurable) {
  Rig r;
  r.flow->configSource()->apply("large.minShares", "300");
  r.frame({add(1, Side::Bid, 10.00, 299)});
  r.frame({add(2, Side::Bid, 10.00, 300)});
  EXPECT_EQ(r.stored(kActive), 1);
}

// Filled vs pulled shares accumulate over the whole life; the end reason is
// the final removal's cause.
TEST(FlowLargeOrderTest, LifecycleEndingInAFill) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 6000, kTs)});
  r.frame({upd(1, 4000, MutationCause::Execution, kTs + 1 * kMs)});
  r.frame({upd(1, 3000, MutationCause::Cancel, kTs + 2 * kMs)});
  r.take();
  r.frame({del(1, MutationCause::Execution, kTs + 3'500'000)});
  EXPECT_EQ(p5(r), (M{{kFilled, 5000}, {kCanc, 1000}, {kLife, 3.5}, {kEndF, 1}}));
  EXPECT_EQ(r.stored(kActive), 0);
}

TEST(FlowLargeOrderTest, LifecycleEndingInACancel) {
  Rig r;
  r.frame({add(1, Side::Ask, 10.01, 8000, kTs)});
  r.frame({upd(1, 7500, MutationCause::Execution, kTs + 1 * kMs)});
  r.take();
  r.frame({del(1, MutationCause::Cancel, kTs + 1500 * kMs)});
  EXPECT_EQ(p5(r), (M{{kFilled, 500}, {kCanc, 7500}, {kLife, 1500}, {kEndC, 1}}));
}

TEST(FlowLargeOrderTest, UnknownCauseEndsWithNeitherFlag) {
  Rig r;
  r.frame({add(3, Side::Bid, 10.00, 6000, kTs)});
  r.frame({upd(3, 5000, MutationCause::Execution, kTs + 1 * kMs)});
  r.take();
  r.frame({del(3, MutationCause::Unknown, kTs + 3 * kMs)});
  EXPECT_EQ(p5(r), (M{{kFilled, 1000}, {kLife, 3}}))
      << "cause Unknown: neither end flag, the remainder is claimed by neither side";
  EXPECT_EQ(r.stored(kActive), 0);
  // An update to size 0 is rejected by OrderBookManager (malformed) and so is
  // never observed: it cannot end a chain.
  r.frame({add(4, Side::Bid, 10.00, 6000, kTs)});
  r.frame({upd(4, 0, MutationCause::Execution, kTs)});
  EXPECT_EQ(p5(r), M{});
  EXPECT_EQ(r.stored(kActive), 1);
}

// ITCH U keeps the chain: the new id inherits largeness (even when re-posted
// small), the lifetime runs from the origin, and the shares carry over.
TEST(FlowLargeOrderTest, ReplaceContinuity) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 6000, kTs)});
  r.frame({upd(1, 5000, MutationCause::Execution, kTs + 1 * kMs)});
  r.take();
  r.frame({del(1, MutationCause::Replace, kTs + 10 * kMs),
           add(2, Side::Bid, 9.99, 100, kTs + 10 * kMs, 1)});
  EXPECT_EQ(p5(r), M{}) << "a replace is not an end";
  EXPECT_EQ(r.stored(kActive), 1) << "no flicker across the U";
  r.frame({del(2, MutationCause::Replace, kTs + 20 * kMs),
           add(3, Side::Bid, 9.98, 200, kTs + 20 * kMs, 2)});
  EXPECT_EQ(r.stored(kActive), 1);
  r.take();
  r.frame({del(3, MutationCause::Cancel, kTs + 25 * kMs)});
  EXPECT_EQ(p5(r), (M{{kFilled, 1000}, {kCanc, 200}, {kLife, 25}, {kEndC, 1}}))
      << "replaced shares move, they are not cancelled; lifetime from the origin";
  EXPECT_EQ(r.stored(kActive), 0);
}

// A replace UP past the threshold makes the chain large; its lifetime still
// runs from the chain's origin.
TEST(FlowLargeOrderTest, ReplacedUpPastTheThresholdBecomesLarge) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  EXPECT_EQ(r.stored(kActive), 0);
  r.frame({del(1, MutationCause::Replace, kTs + 5 * kMs),
           add(2, Side::Bid, 10.00, 9000, kTs + 5 * kMs, 1)});
  EXPECT_EQ(r.stored(kActive), 1);
  r.take();
  r.frame({del(2, MutationCause::Execution, kTs + 7 * kMs)});
  EXPECT_EQ(p5(r), (M{{kFilled, 9000}, {kLife, 7}, {kEndF, 1}}));
}

// A delete{Replace} whose add half never comes is a pull for good: ended as a
// cancel, with its remaining shares cancelled, on the next event.
TEST(FlowLargeOrderTest, ReplaceWithoutItsAddHalfEndsAsACancel) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 6000, kTs)});
  r.frame({add(9, Side::Bid, 9.90, 10, kTs)});
  r.take();
  r.frame({del(1, MutationCause::Replace, kTs + 4 * kMs)});
  EXPECT_EQ(p5(r), M{});
  r.frame({del(9, MutationCause::Cancel, kTs + 9 * kMs)});
  EXPECT_EQ(p5(r), (M{{kCanc, 6000}, {kLife, 4}, {kEndC, 1}}));
  EXPECT_EQ(r.stored(kActive), 0);
}

// Warm-up: until 1000 new-order adds are seen only minShares applies; then
// mult x median. A replace's add half is not a sample.
TEST(FlowLargeOrderTest, MedianWarmUp) {
  Rig r;
  uint64_t id = 1;
  for (; id <= 999; ++id) r.frame({add(id, Side::Bid, 9.00, 1000)});
  // A U on #1: its add half (#5000) must not count as the 1000th sample.
  r.frame({del(1, MutationCause::Replace), add(5000, Side::Bid, 9.00, 1000, kTs, 1)});
  EXPECT_EQ(r.stored(kActive), 0);
  r.frame({add(6000, Side::Ask, 11.00, 6000)});
  EXPECT_EQ(r.stored(kActive), 1) << "999 samples: still warm-up, 6000 >= 5000";
  // That add was sample 1000: the threshold is now max(5000, 10 x 1000).
  r.frame({add(6001, Side::Ask, 11.00, 6000)});
  EXPECT_EQ(r.stored(kActive), 1) << "6000 < 10 x median 1000";
  r.frame({add(6002, Side::Ask, 11.00, 9999)});
  EXPECT_EQ(r.stored(kActive), 1) << "9999 < 10000";
  r.frame({add(6003, Side::Ask, 11.00, 10000)});
  EXPECT_EQ(r.stored(kActive), 2) << "10000 >= 10000";
}

// Small orders: after warm-up minShares still dominates mult x median.
TEST(FlowLargeOrderTest, MinSharesDominatesASmallMedian) {
  Rig r;
  for (uint64_t id = 1; id <= 1000; ++id) r.frame({add(id, Side::Bid, 9.00, 100)});
  r.frame({add(2000, Side::Ask, 11.00, 4999)});
  EXPECT_EQ(r.stored(kActive), 0) << "10 x 100 = 1000 < minShares";
  r.frame({add(2001, Side::Ask, 11.00, 5000)});
  EXPECT_EQ(r.stored(kActive), 1);
}

// Orders resting before connect are unknown and never large; a replace of one
// is judged on its own size, with lifetime from the add the shadow saw.
TEST(FlowLargeOrderTest, UnknownOrders) {
  Rig r(/*attach=*/false);
  r.frame({add(1, Side::Bid, 10.00, 9000, kTs)});
  r.frame({add(2, Side::Bid, 10.00, 9000, kTs)});
  r.attach();
  r.frame({del(1, MutationCause::Cancel, kTs + 1 * kMs)});
  EXPECT_EQ(p5(r), M{});
  EXPECT_EQ(r.stored(kActive), 0);
  r.frame({del(2, MutationCause::Replace, kTs + 2 * kMs),
           add(3, Side::Bid, 10.00, 9000, kTs + 2 * kMs, 2)});
  EXPECT_EQ(r.stored(kActive), 1);
  r.take();
  r.frame({del(3, MutationCause::Cancel, kTs + 5 * kMs)});
  EXPECT_EQ(p5(r), (M{{kCanc, 9000}, {kLife, 3}, {kEndC, 1}})) << "a lower bound";
}

// D3 + review F4: an unattributed trade consumes the level FIFO by
// quantity. A partial fill leaves a large order live; the trade that fills it
// completely ends it as a fill, with every share it took. A book reset loses
// the rest without an end.
TEST(FlowLargeOrderTest, UnattributedTradeFillsFifoAndResetLoses) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 6000, kTs)});
  r.frame({add(2, Side::Bid, 10.00, 6000, kTs)});   // behind #1 in the queue
  r.frame({add(3, Side::Ask, 10.05, 6000, kTs)});
  EXPECT_EQ(r.stored(kActive), 3);
  r.take();
  r.frame({trade(10.00, 100, Aggressor::Sell, /*bookNeutral=*/false, kTs + 1 * kMs)});
  EXPECT_EQ(p5(r), M{}) << "#1 partially filled: still live";
  EXPECT_EQ(r.stored(kActive), 3);
  r.frame({trade(10.00, 6400, Aggressor::Sell, /*bookNeutral=*/false, kTs + 4 * kMs)});
  EXPECT_EQ(p5(r), (M{{kFilled, 6000}, {kLife, 4}, {kEndF, 1}}))
      << "#1 filled out (100 + 5900); #2 took 500 and stays";
  EXPECT_EQ(r.stored(kActive), 2);
  r.frame({del(2, MutationCause::Cancel, kTs + 6 * kMs)});
  EXPECT_EQ(p5(r), (M{{kFilled, 500}, {kCanc, 5500}, {kLife, 6}, {kEndC, 1}}));
  EXPECT_EQ(r.stored(kActive), 1);
  std::vector<FeedEvent> f{feed::ObResetEvent{kSym, 2}};
  r.applier.apply(f);
  EXPECT_EQ(p5(r), M{});
  EXPECT_EQ(r.stored(kActive), 0);
}

// Review F2: an add of a resting id is delete-then-add. A large order it
// displaces ends as a pull; the add is judged afresh (here: small).
TEST(FlowLargeOrderTest, DuplicateIdAddEndsTheDisplacedOrder) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 6000, kTs)});
  r.take();
  r.frame({add(1, Side::Bid, 10.00, 100, kTs + 2 * kMs)});
  EXPECT_EQ(p5(r), (M{{kCanc, 6000}, {kLife, 2}, {kEndC, 1}}));
  EXPECT_EQ(r.stored(kActive), 0);
  r.frame({del(1, MutationCause::Cancel, kTs + 3 * kMs)});
  EXPECT_EQ(p5(r), M{}) << "the new #1 was never large";
}
