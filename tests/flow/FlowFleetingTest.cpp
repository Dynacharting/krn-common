// ENC-1649 (SPEC 2026-11-17-order-flow-pipelines D5 P4, Q3 settled): the
// fleeting-order rate on hand-built frames through the real IngressApplier,
// OrderBookManager and FlowComponent with its default operator set.
#include "FlowTestRig.hpp"

#include "krn/flow/FleetingOperator.hpp"

#include <gtest/gtest.h>

#include <random>

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

}

// Once per chain: an order replaced three times within 50 ms and then
// cancelled is ONE fleeting chain, charged to its originating add's slot.
TEST(FlowFleetingTest, AChainIsFleetingAtMostOnce) {
  Rig r;
  r.frame({add(3, Side::Bid, 10.00, 100, kTs)});
  ASSERT_FALSE(values(r, kRatio).empty());
  EXPECT_DOUBLE_EQ(values(r, kRatio).back(), 0.0);
  r.frame({del(3, MutationCause::Replace, kTs + 10 * kMs),
           add(4, Side::Bid, 10.00, 100, kTs + 10 * kMs, 3)});
  r.frame({del(4, MutationCause::Replace, kTs + 20 * kMs),
           add(5, Side::Bid, 10.00, 90, kTs + 20 * kMs, 4)});
  r.frame({upd(5, 90, MutationCause::Replace, kTs + 30 * kMs, 9.99)});   // MBO-style modify
  r.frame({del(5, MutationCause::Cancel, kTs + 40 * kMs)});
  EXPECT_EQ(values(r, kRatio).size(), 1u) << "only #3's add pushed a ratio";
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kFleet, 1}, {kRatio, 0}}))
      << "three replaces and a cancel, all young: one fleeting chain";
  r.frame({add(6, Side::Bid, 10.00, 10, kTs + 50 * kMs)});
  ASSERT_FALSE(values(r, kRatio).empty());
  EXPECT_DOUBLE_EQ(values(r, kRatio).back(), 1.0 / 2) << "#3's slot is marked once, never twice";
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
  // Cancels are charged to their ORIGINATING add's slot (#2, #3), not the newest.
  r.frame({del(2, MutationCause::Cancel)});
  r.frame({del(3, MutationCause::Cancel)});
  EXPECT_DOUBLE_EQ(ratioAfterAdd(6), 1.0 / 4) << "add #6 evicts #2's marked slot";
  EXPECT_DOUBLE_EQ(ratioAfterAdd(7), 0.0) << "add #7 evicts #3's";
  // A chain whose add already left the ring: counted, but not in the ratio.
  r.take();
  r.frame({del(4, MutationCause::Cancel)});   // ring is #4..#7: in
  r.frame({add(8, Side::Bid, 9.08, 10)});     // evicts #4
  ASSERT_FALSE(values(r, kRatio).empty());
  EXPECT_DOUBLE_EQ(values(r, kRatio).back(), 0.0);
  r.frame({add(9, Side::Bid, 9.09, 10)});
  r.frame({add(10, Side::Bid, 9.10, 10)});    // ring is #7..#10
  r.take();
  r.frame({del(5, MutationCause::Cancel)});   // #5 left the ring
  EXPECT_EQ(p4(r), (M{{kFleet, 1}}));
  EXPECT_DOUBLE_EQ(ratioAfterAdd(11), 0.0);
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
  r.frame({del(3, MutationCause::Cancel)});   // add #3 is in the ring #2..#501
  r.frame({add(502, Side::Bid, 9.00, 10)});   // ring #3..#502
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
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kFleet, 1}, {kRatio, 0.5}}))
      << "the pull is charged to the displaced chain's own add";
  r.frame({add(1, Side::Bid, 10.00, 40, kTs + 70 * kMs)});
  EXPECT_EQ(p4(r), (M{{kAdds, 1}, {kRatio, 1.0 / 3}})) << "the displaced #1 was 60 ms old";
}

// Property: over a long randomised run of adds, cancels, replaces, MBO
// modifies, duplicate-id adds and executions, every published ratio is in
// [0, 1] and fleeting chains never outnumber adds. Small window so it rolls
// many times; ages straddle maxAgeNs.
TEST(FlowFleetingTest, RatioStaysInUnitIntervalOverARandomRun) {
  for (const char* window : {"7", "500"}) {
    Rig r;
    r.flow->configSource()->apply("fleeting.window", window);
    std::mt19937_64 rng(0xF1EE7u);
    std::vector<uint64_t> live;
    uint64_t next = 1, ts = kTs, adds = 0, fleets = 0, ratios = 0;
    auto pick = [&]() -> std::size_t { return std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng); };
    auto px = [&] { return 9.00 + 0.01 * static_cast<double>(rng() % 90); };
    for (int i = 0; i < 20000; ++i) {
      ts += std::uniform_int_distribution<uint64_t>(0, 15 * kMs)(rng);
      const unsigned op = live.empty() ? 0 : static_cast<unsigned>(rng() % 10);
      if (op <= 2) {
        r.frame({add(next, Side::Bid, px(), 1 + rng() % 500, ts)});
        live.push_back(next++);
      } else if (op <= 4) {                       // cancel
        const std::size_t k = pick();
        r.frame({del(live[k], MutationCause::Cancel, ts)});
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      } else if (op <= 6) {                       // ITCH U
        const std::size_t k = pick();
        r.frame({del(live[k], MutationCause::Replace, ts),
                 add(next, Side::Bid, px(), 1 + rng() % 500, ts, live[k])});
        live[k] = next++;
      } else if (op == 7) {                       // MBO modify
        r.frame({upd(live[pick()], 1 + rng() % 500, MutationCause::Replace, ts, px())});
      } else if (op == 8) {                       // duplicate-id add
        r.frame({add(live[pick()], Side::Bid, px(), 1 + rng() % 500, ts)});
      } else {                                    // execution
        const std::size_t k = pick();
        r.frame({del(live[k], MutationCause::Execution, ts)});
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      }
      for (const auto& [k, v] : r.log) {
        if (k == kRatio) {
          ++ratios;
          ASSERT_GE(v, 0.0) << "window " << window << ", event " << i;
          ASSERT_LE(v, 1.0) << "window " << window << ", event " << i;
        } else if (k == kAdds) {
          ++adds;
        } else if (k == kFleet) {
          ++fleets;
        }
      }
      r.log.clear();
    }
    EXPECT_EQ(ratios, adds) << "one ratio per add";
    EXPECT_LE(fleets, adds);
    EXPECT_GT(fleets, 100u) << "the run exercised fleeting pulls (window " << window << ")";
    EXPECT_GT(adds, 3000u);
  }
}
