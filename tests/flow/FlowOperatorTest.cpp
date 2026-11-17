// ENC-1646 (SPEC 2026-11-17-order-flow-pipelines D5 P0, P1; D3; D6): exact
// OFI and touch-depletion values on hand-built frames driven through the real
// IngressApplier into a real OrderBookManager, observed by the real
// FlowComponent with its default operator set.
#include "FlowTestRig.hpp"

#include "krn/flow/OfiOperator.hpp"
#include "krn/flow/TouchDepletionOperator.hpp"

#include <gtest/gtest.h>

using namespace krn;
using namespace krn::flowtest;
using M = std::map<std::string, double>;

namespace {

const std::string& kOfi = flow::OfiOperator::kOfi;
const std::string& kBF  = flow::TouchDepletionOperator::kBidFilled;
const std::string& kBC  = flow::TouchDepletionOperator::kBidCancelled;
const std::string& kBA  = flow::TouchDepletionOperator::kBidAdded;
const std::string& kAF  = flow::TouchDepletionOperator::kAskFilled;
const std::string& kAC  = flow::TouchDepletionOperator::kAskCancelled;
const std::string& kAA  = flow::TouchDepletionOperator::kAskAdded;

} // namespace

// Seeding: the first add into an empty side has no touch and no OFI term; a
// same-price add is `added` and +size OFI; adds behind the touch are neither.
TEST(FlowOperatorTest, SeedAddsAtAndBehindTheTouch) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100)});
  EXPECT_EQ(r.take(), M{}) << "an add into an empty bid has no touch and no OFI term";
  r.frame({add(2, Side::Bid, 10.00, 50)});
  EXPECT_EQ(r.take(), (M{{kBA, 50}, {kOfi, 50}}));
  r.frame({add(3, Side::Bid, 9.99, 200)});
  EXPECT_EQ(r.take(), M{}) << "behind the touch: post == pre, e = qb - qb = 0";
  r.frame({add(4, Side::Ask, 10.01, 80)});
  EXPECT_EQ(r.take(), M{});
  r.frame({add(5, Side::Ask, 10.02, 300)});
  EXPECT_EQ(r.take(), M{});
  EXPECT_EQ(r.flow->shadowSize(kSym), 5u);
  EXPECT_EQ(r.stored("flow.meta.shadow_orders"), 5);
  EXPECT_EQ(r.stored(flow::OfiOperator::kOfiCum), 50);
  // Every meta counter is in the store from the symbol's first event, so an
  // AtomicAccessor reads 0 rather than nothing.
  EXPECT_EQ(r.stored("flow.meta.unattributed"), 0);
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 0);
  EXPECT_EQ(r.stored("flow.meta.unknown_cause"), 0);
}

TEST(FlowOperatorTest, BetterPricedAddIsNotAtTheTouch) {
  Rig q;
  seed(q);
  q.take();
  q.frame({del(4, MutationCause::Cancel)});   // ask touch -> 10.02
  EXPECT_EQ(q.take(), (M{{kAC, 80}, {kOfi, 80}}));
  q.frame({add(9, Side::Bid, 10.01, 10)});    // new best bid, in front of 10.00
  EXPECT_EQ(q.take(), (M{{kOfi, 10}})) << "Pb' > Pb: e = +qb' only; no `added` at the old touch";
}

TEST(FlowOperatorTest, ExecutionsAndCancelsAtTheBidTouch) {
  Rig r;
  seed(r);
  r.take();
  // E on #1: 100 -> 60, then its bookNeutral print (pre == post): nothing more.
  r.frame({upd(1, 60, MutationCause::Execution),
           trade(10.00, 40, Aggressor::Sell, /*bookNeutral=*/true, kTs, 1)});
  EXPECT_EQ(r.take(), (M{{kBF, 40}, {kOfi, -40}}));
  // X on #2: 50 -> 20.
  r.frame({upd(2, 20, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 30}, {kOfi, -30}}));
  // D #1 (level keeps #2's 20).
  r.frame({del(1, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 60}, {kOfi, -60}}));
  // D #2 empties 10.00; touch moves to 9.99: e = -qb (Pb' < Pb).
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 20}, {kOfi, -20}}));
  // A cancel behind the touch is not touch depletion and moves no OFI.
  r.frame({add(7, Side::Bid, 9.98, 40)});
  r.frame({del(7, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), M{});
  EXPECT_EQ(r.flow->shadowSize(kSym), 3u);
  EXPECT_EQ(r.stored(flow::OfiOperator::kOfiCum), 50 - 40 - 30 - 60 - 20);
}

TEST(FlowOperatorTest, ExecutionThatEmptiesTheAskTouch) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(4, MutationCause::Execution),
           trade(10.01, 80, Aggressor::Buy, true, kTs, 4)});
  // Pa' > Pa: e = +qa = +80 (ask depletion is buy pressure).
  EXPECT_EQ(r.take(), (M{{kAF, 80}, {kOfi, 80}}));
  r.frame({upd(5, 100, MutationCause::Execution)});
  EXPECT_EQ(r.take(), (M{{kAF, 200}, {kOfi, 200}}));
}

// ITCH U, the level holds another order: cancelled(old) + added(new), never the net.
TEST(FlowOperatorTest, SamePriceReplaceIsCancelledPlusAdded) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(2, MutationCause::Replace), add(6, Side::Bid, 10.00, 70, kTs, 2)});
  EXPECT_EQ(r.take(), (M{{kBC, 50}, {kBA, 70}, {kOfi, 20}}));
}

// ITCH U on the level's only order: the delete half empties the level, so the
// add half's own pre touch is 9.99 — it still reads as a same-price replace.
TEST(FlowOperatorTest, SamePriceReplaceOfTheOnlyOrderAtTheTouch) {
  Rig r;
  seed(r);
  r.frame({del(2, MutationCause::Cancel)});
  r.take();
  r.frame({del(1, MutationCause::Replace), add(6, Side::Bid, 10.00, 120, kTs, 1)});
  // OFI: delete -100 (touch -> 9.99); add re-opens 10.00 in front: +120.
  EXPECT_EQ(r.take(), (M{{kBC, 100}, {kBA, 120}, {kOfi, 20}}));
}

TEST(FlowOperatorTest, ReplaceAwayFromTheTouchIsCancelled) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(1, MutationCause::Replace), add(6, Side::Bid, 9.98, 100, kTs, 1)});
  EXPECT_EQ(r.take(), (M{{kBC, 100}, {kOfi, -100}}));
  // Replace from behind the touch onto it: added only.
  r.frame({del(3, MutationCause::Replace), add(8, Side::Bid, 10.00, 25, kTs, 3)});
  EXPECT_EQ(r.take(), (M{{kBA, 25}, {kOfi, 25}}));
}

// The replace chain keeps the order's identity in the shadow.
TEST(FlowOperatorTest, ReplaceKeepsOriginInTheShadow) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  r.frame({upd(1, 90, MutationCause::Execution, kTs + 5)});
  r.frame({del(1, MutationCause::Replace, kTs + 10), add(6, Side::Bid, 10.00, 70, kTs + 10, 1)});
  bool seen = false;
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    const flow::ShadowOrder* o = ctx.shadow.find(6);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->originOrderId, 1u);
    EXPECT_EQ(o->originTsNs, kTs);
    EXPECT_EQ(o->originSize, 100u);
    EXPECT_TRUE(o->originKnown);
    EXPECT_EQ(o->replaces, 1u);
    EXPECT_EQ(o->filled, 10u);
    EXPECT_EQ(o->addTsNs, kTs + 10);
    EXPECT_EQ(ctx.shadow.find(1), nullptr) << "the replaced id is pruned";
    seen = true;
  });
  EXPECT_TRUE(seen);
}

// MBO modify (update{Replace}, same id) at the touch price: split, not net.
TEST(FlowOperatorTest, ModifyReplaceSamePriceIsCancelledPlusAdded) {
  Rig r;
  seed(r);
  r.take();
  r.frame({upd(1, 130, MutationCause::Replace, kTs, 10.00)});
  EXPECT_EQ(r.take(), (M{{kBC, 100}, {kBA, 130}, {kOfi, 30}}));
}

// FeedServer's `ob trade`: consumes the book with no per-order event.
TEST(FlowOperatorTest, UnattributedExecutionCountsAsFilled) {
  Rig r;
  seed(r);
  r.take();
  r.frame({trade(10.00, 120, Aggressor::Sell, /*bookNeutral=*/false)});
  EXPECT_EQ(r.take(), (M{{kBF, 120}, {kOfi, -120}}));
  EXPECT_EQ(r.stored("flow.meta.unattributed"), 1);
  // Both shadow orders at 10.00 are forgotten (the book took #1 whole and 20
  // of #2, FIFO, and said neither).
  EXPECT_EQ(r.flow->shadowSize(kSym), 3u);
  // #2's remaining 30 is then deleted: the book knows it, the shadow does
  // not — counted, and P1 falls back on the TobChange.
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 30}, {kOfi, -30}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 1);
}

// Orders resting before the component saw them (connect / snapshot).
TEST(FlowOperatorTest, UnknownOrderUpdatesFallBackOnTheTopOfBook) {
  Rig r(/*attach=*/false);
  seed(r);
  r.attach();
  r.take();
  EXPECT_EQ(r.flow->shadowSize(kSym), 0u);
  r.frame({upd(1, 60, MutationCause::Execution)});
  EXPECT_EQ(r.take(), (M{{kBF, 40}, {kOfi, -40}}));
  r.frame({upd(3, 150, MutationCause::Cancel)});   // behind the touch
  EXPECT_EQ(r.take(), M{});
  r.frame({del(4, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kAC, 80}, {kOfi, 80}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 3);
  EXPECT_EQ(r.flow->shadowSize(kSym), 0u) << "updates never create shadow entries";
}

TEST(FlowOperatorTest, UnknownCauseIsNeitherFilledNorCancelled) {
  Rig r;
  seed(r);
  r.take();
  r.frame({del(1, MutationCause::Unknown)});
  EXPECT_EQ(r.take(), (M{{kOfi, -100}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_cause"), 1);
}

// D6: exchange time when present, else the receive clock, flagged.
TEST(FlowOperatorTest, TimeSourceFallsBackToTheReceiveClock) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100, kTs)});
  EXPECT_EQ(r.stored("flow.meta.ts_source"), 0);
  r.frame({add(2, Side::Bid, 10.00, 100, /*ts=*/0)});
  EXPECT_EQ(r.stored("flow.meta.ts_source"), 1);
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    const auto* o = ctx.shadow.find(2);
    ASSERT_NE(o, nullptr);
    EXPECT_GT(o->addTsNs, 0u) << "the fallback stamps a monotonic receive time";
  });
  r.frame({add(3, Side::Bid, 10.00, 100, kTs)});
  EXPECT_EQ(r.stored("flow.meta.ts_source"), 0);
}

TEST(FlowOperatorTest, ResetClearsTheShadowButKeepsCumulativeOfi) {
  Rig r;
  seed(r);
  EXPECT_EQ(r.flow->shadowSize(kSym), 5u);
  const double cum = r.stored(flow::OfiOperator::kOfiCum);
  std::vector<FeedEvent> f{feed::ObResetEvent{kSym, 2}};
  r.applier.apply(f);
  EXPECT_EQ(r.flow->shadowSize(kSym), 0u);
  EXPECT_EQ(r.stored("flow.meta.shadow_orders"), 0);
  EXPECT_EQ(r.stored(flow::OfiOperator::kOfiCum), cum);
}

TEST(FlowOperatorTest, DisabledComponentPublishesNothing) {
  Rig r;
  r.flow->configSource()->apply("enabled", "false");
  r.flow->start();
  EXPECT_FALSE(r.flow->enabled());
  seed(r);
  EXPECT_EQ(r.take(), M{});
  EXPECT_EQ(r.flow->shadowSize(kSym), 0u);
}

// The OFI term on its own, every price relation per side.
TEST(FlowOperatorTest, OfiTermCases) {
  auto lv = [](int64_t t, uint64_t q) { return TobLevel{true, Price{t}, t * 0.01, q}; };
  const TobLevel none{};
  using flow::OfiOperator;
  // bid same price: qb' - qb
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), none}, {lv(100, 25), none}}), 15);
  // bid up: +qb'
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), none}, {lv(101, 7), none}}), 7);
  // bid down: -qb
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), none}, {lv(99, 7), none}}), -10);
  // ask same price: qa - qa'
  EXPECT_EQ(OfiOperator::term({{none, lv(200, 10)}, {none, lv(200, 4)}}), 6);
  // ask down: -qa'
  EXPECT_EQ(OfiOperator::term({{none, lv(200, 10)}, {none, lv(199, 4)}}), -4);
  // ask up: +qa
  EXPECT_EQ(OfiOperator::term({{none, lv(200, 10)}, {none, lv(201, 4)}}), 10);
  // absent before or after: no term for that side
  EXPECT_EQ(OfiOperator::term({{none, none}, {lv(100, 10), none}}), 0);
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), none}, {none, none}}), 0);
  // both sides add up
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), lv(200, 10)}, {lv(100, 12), lv(201, 3)}}), 2 + 10);
}
