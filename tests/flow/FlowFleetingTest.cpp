// ENC-1649 (SPEC 2026-11-17-order-flow-pipelines D5 P4, Q3 settled): the
// fleeting-order rate on hand-built frames through the real IngressApplier,
// OrderBookManager and FlowComponent with its default operator set.
#include "FlowTestRig.hpp"

#include "krn/flow/FleetingOperator.hpp"

#include <gtest/gtest.h>

using namespace krn;
using namespace krn::flowtest;
using M = std::map<std::string, double>;

namespace {

const std::string& kAdds   = flow::FleetingOperator::kAdds;
const std::string& kFleet  = flow::FleetingOperator::kCancelled;
const std::string& kRatio  = flow::FleetingOperator::kRatio;
constexpr uint64_t kMs = 1'000'000;

// Only the P4 keys of one take().
M p4(Rig& r) {
  M out;
  for (const auto& [k, v] : r.take())
    if (k.rfind("flow.fleeting.", 0) == 0) out[k] = v;
  return out;
}

// Every value notified on `key` since the last take()/clear, in order.
std::vector<double> values(const Rig& r, const std::string& key) {
  std::vector<double> out;
  for (const auto& [k, v] : r.log)
    if (k == key) out.push_back(v);
  return out;
}

} // namespace

// Every new order pushes adds=1 AND the ratio — the ratio even when it is 0,
// so a bucket's `last` can read 0 (FlowPublisher::level).
TEST(FlowFleetingTest, EveryAddPublishesAddsAndTheRatioIncludingZero) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100)});
  EXPECT_EQ(values(r, kRatio), (std::vector<double>{0.0})) << "a ratio of 0 is notified";
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kRatio, 0}}));
  r.frame({add(2, Side::Ask, 10.01, 100)});
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kRatio, 0}}));
  EXPECT_FALSE(r.store.get(kSym, kRatio).has_value()) << "pushed, never stored";
}

// Age < maxAgeNs (strict): 1 ns under the default 50 ms is fleeting, exactly
// 50 ms is not.
TEST(FlowFleetingTest, AgeThresholdIsStrict) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  r.frame({add(2, Side::Bid, 10.00, 100, kTs)});
  r.take();
  r.frame({del(1, MutationCause::Cancel, kTs + 50 * kMs - 1)});
  EXPECT_EQ(p4(r), (M{{kFleet, 1}})) << "49.999999 ms old";
  r.frame({del(2, MutationCause::Cancel, kTs + 50 * kMs)});
  EXPECT_EQ(p4(r), M{}) << "exactly 50 ms old is not fleeting";
}

TEST(FlowFleetingTest, MaxAgeIsConfigurable) {
  Rig r;
  r.flow->configSource()->apply("fleeting.maxAgeNs", "1000");
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  r.frame({add(2, Side::Bid, 10.00, 100, kTs)});
  r.take();
  r.frame({del(1, MutationCause::Cancel, kTs + 999)});
  EXPECT_EQ(p4(r), (M{{kFleet, 1}}));
  r.frame({del(2, MutationCause::Cancel, kTs + 1000)});
  EXPECT_EQ(p4(r), M{});
}

// Which removals are pulls: full cancel, cancel-to-zero, a replace (ITCH U
// delete half, MBO modify). Not: partial cancel, execution, unknown cause.
TEST(FlowFleetingTest, OnlyPullsOfTheWholeOrderCount) {
  Rig r;
  for (uint64_t id = 1; id <= 6; ++id) r.frame({add(id, Side::Bid, 10.00, 100)});
  r.take();
  r.frame({upd(1, 40, MutationCause::Cancel)});
  EXPECT_EQ(p4(r), M{}) << "a partial cancel leaves the order resting";
  r.frame({del(1, MutationCause::Cancel)});
  EXPECT_EQ(p4(r), (M{{kFleet, 1}})) << "after a partial cancel, the full cancel is the pull";
  r.frame({upd(2, 30, MutationCause::Execution)});
  r.frame({del(2, MutationCause::Execution)});
  EXPECT_EQ(p4(r), M{}) << "a fill is not a pull";
  // (An update to size 0 never reaches flow: OrderBookManager rejects
  // newSize 0 as malformed, so a full cancel is always a delete. The
  // operator still treats update{Cancel}->0 as a pull, defensively.)
  r.frame({upd(3, 0, MutationCause::Cancel)});
  EXPECT_EQ(p4(r), M{}) << "rejected by the book, so never observed";
  r.frame({del(4, MutationCause::Unknown)});
  EXPECT_EQ(p4(r), M{}) << "no cause, no claim";
  r.frame({upd(5, 100, MutationCause::Replace, kTs, 10.01)});
  EXPECT_EQ(p4(r), (M{{kFleet, 1}})) << "an MBO modify pulls and re-posts";
  r.frame({del(6, MutationCause::Cancel)});
  EXPECT_EQ(p4(r), (M{{kFleet, 1}}));
}

// The add half of a replace is not a new order: no adds, no ratio push. The
// delete half is a pull. Age runs from the chain's origin, so a replace does
// not make the order young again.
TEST(FlowFleetingTest, ReplaceIsAPullNotAnAddAndAgeSurvivesIt) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  r.frame({add(9, Side::Bid, 9.99, 100, kTs)});
  r.take();
  // U at +30 ms: pulled young (fleeting), re-posted as #2.
  r.frame({del(1, MutationCause::Replace, kTs + 30 * kMs),
           add(2, Side::Bid, 10.00, 80, kTs + 30 * kMs, 1)});
  EXPECT_EQ(p4(r), (M{{kFleet, 1}})) << "no adds, no ratio: a replace is not a new order";
  // Cancel #2 at +60 ms: 30 ms after its own add, but 60 ms after the
  // chain's origin — not fleeting.
  r.frame({del(2, MutationCause::Cancel, kTs + 60 * kMs)});
  EXPECT_EQ(p4(r), M{}) << "age is measured from originTsNs";

  // A young chain contributes one fleeting pull per replace.
  r.frame({add(3, Side::Bid, 10.00, 100, kTs + 100 * kMs)});
  r.frame({del(3, MutationCause::Replace, kTs + 110 * kMs),
           add(4, Side::Bid, 10.00, 100, kTs + 110 * kMs, 3)});
  r.frame({del(4, MutationCause::Cancel, kTs + 120 * kMs)});
  EXPECT_EQ(values(r, kRatio).size(), 1u) << "only #3's add pushed a ratio";
  const M got = p4(r);
  EXPECT_EQ(got.at(kFleet), 2);
  EXPECT_EQ(got.at(kAdds), 1);
}

// Orders resting before the shadow started have no age: never fleeting.
// Nor does a chain whose origin the shadow never saw (its true age is
// larger than the measurable one).
TEST(FlowFleetingTest, UnknownOrdersAndUnknownOriginsAreNeverFleeting) {
  Rig r(/*attach=*/false);
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(2, Side::Bid, 10.00, 100)});
  r.attach();
  r.frame({del(1, MutationCause::Cancel)});
  EXPECT_EQ(p4(r), M{}) << "resting before connect";
  r.frame({del(2, MutationCause::Replace), add(3, Side::Bid, 10.00, 50, kTs, 2)});
  EXPECT_EQ(p4(r), M{}) << "replace of an unknown order: no pull, and its add half is no add";
  r.frame({del(3, MutationCause::Cancel)});
  EXPECT_EQ(p4(r), M{}) << "originKnown = false: age unprovable";
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 2);
}

// The ratio is cancels / adds over the last `window` adds: an old add's
// cancels leave the window with it.
TEST(FlowFleetingTest, RatioWindowRollsOver) {
  Rig r;
  r.flow->configSource()->apply("fleeting.window", "4");
  auto ratioAfterAdd = [&](uint64_t id) {
    r.take();
    r.frame({add(id, Side::Bid, 9.00 + 0.01 * static_cast<double>(id), 10)});
    const auto v = values(r, kRatio);
    EXPECT_EQ(v.size(), 1u) << "one ratio per add";
    return v.empty() ? -1.0 : v.back();
  };
  EXPECT_DOUBLE_EQ(ratioAfterAdd(1), 0.0);
  r.frame({del(1, MutationCause::Cancel)});            // charged to add #1's slot
  EXPECT_DOUBLE_EQ(ratioAfterAdd(2), 1.0 / 2);          // warm-up: denominator = adds so far
  EXPECT_DOUBLE_EQ(ratioAfterAdd(3), 1.0 / 3);
  EXPECT_DOUBLE_EQ(ratioAfterAdd(4), 1.0 / 4);
  EXPECT_DOUBLE_EQ(ratioAfterAdd(5), 0.0) << "add #1 (and its cancel) left the window";
  r.frame({del(2, MutationCause::Cancel)});
  r.frame({del(3, MutationCause::Cancel)});
  EXPECT_DOUBLE_EQ(ratioAfterAdd(6), 2.0 / 4);
  EXPECT_DOUBLE_EQ(ratioAfterAdd(7), 2.0 / 4);
  EXPECT_DOUBLE_EQ(ratioAfterAdd(8), 2.0 / 4);
  EXPECT_DOUBLE_EQ(ratioAfterAdd(9), 0.0) << "add #5's slot held both cancels; add #9 evicts it";
}

// The default window is 500 adds.
TEST(FlowFleetingTest, DefaultWindowIs500Adds) {
  Rig r;
  r.frame({add(1, Side::Bid, 9.00, 10)});
  r.frame({del(1, MutationCause::Cancel)});
  for (uint64_t id = 2; id <= 500; ++id) r.frame({add(id, Side::Bid, 9.00, 10)});
  r.take();
  r.frame({add(501, Side::Bid, 9.00, 10)});
  ASSERT_FALSE(values(r, kRatio).empty());
  EXPECT_DOUBLE_EQ(values(r, kRatio).back(), 0.0) << "the 501st add evicts the first";
  r.take();
  r.frame({del(2, MutationCause::Cancel)});
  r.frame({add(502, Side::Bid, 9.00, 10)});
  ASSERT_FALSE(values(r, kRatio).empty());
  EXPECT_DOUBLE_EQ(values(r, kRatio).back(), 1.0 / 500);
}

// A book reset keeps the rate: it is a statistic of the symbol's flow.
TEST(FlowFleetingTest, ResetKeepsTheWindow) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 10)});
  r.frame({del(1, MutationCause::Cancel)});
  // Straight to the observer: through the applier the reset also marks the
  // book stale until a snapshot, which would drop the next add.
  r.flow->onBookReset(feed::ObResetEvent{kSym, 2});
  EXPECT_EQ(r.flow->shadowSize(kSym), 0u);
  r.take();
  r.frame({add(2, Side::Bid, 10.00, 10)});
  ASSERT_FALSE(values(r, kRatio).empty());
  EXPECT_DOUBLE_EQ(values(r, kRatio).back(), 1.0 / 2);
}

// Review F2: a duplicate-id add is delete-then-add — the displaced order is a
// pull (fleeting when young) and the add is a new order.
TEST(FlowFleetingTest, DuplicateIdAddIsAPullAndAnAdd) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  r.take();
  r.frame({add(1, Side::Bid, 10.00, 40, kTs + 10 * kMs)});
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kFleet, 1}, {kRatio, 0}}))
      << "ratio is pushed before the pull is charged: the pull lands in the new add's slot";
  r.frame({add(1, Side::Bid, 10.00, 40, kTs + 70 * kMs)});
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kRatio, 1.0 / 3}})) << "the displaced #1 was 60 ms old";
}
