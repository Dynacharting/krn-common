// ENC-1646 (SPEC 2026-11-17-order-flow-pipelines D5 P0, P1; D3; D6): exact
// OFI and touch-depletion values on hand-built frames driven through the real
// IngressApplier into a real OrderBookManager, observed by the real
// FlowComponent with its default operator set.
#include "FlowTestRig.hpp"

#include "krn/flow/OfiOperator.hpp"
#include "krn/flow/TouchDepletionOperator.hpp"

#include <gtest/gtest.h>

using namespace krn;
using krn::flowtest::add;
using krn::flowtest::del;
using krn::flowtest::FeedEvent;
using krn::flowtest::kSym;
using krn::flowtest::kTs;
using krn::flowtest::MutationCause;
using krn::flowtest::seed;
using krn::flowtest::trade;
using krn::flowtest::upd;
using M = std::map<std::string, double>;

namespace {

// This file pins P0 and P1 exactly. Later operators (P2..P5) publish on the
// same frames (e.g. P4's flow.fleeting.adds on every add), so take() here
// keeps only flow.ofi and flow.touch.* — each operator's own test file pins
// its own keys.
struct Rig : krn::flowtest::Rig {
  using krn::flowtest::Rig::Rig;
  M take() {
    M out = krn::flowtest::Rig::take();
    for (auto it = out.begin(); it != out.end();) {
      const bool p01 = it->first == "flow.ofi" || it->first.rfind("flow.touch.", 0) == 0;
      it = p01 ? std::next(it) : out.erase(it);
    }
    return out;
  }
};

} // namespace

namespace {

const std::string& kOfi = flow::OfiOperator::kOfi;
const std::string& kBF  = flow::TouchDepletionOperator::kBidFilled;
const std::string& kBC  = flow::TouchDepletionOperator::kBidCancelled;
const std::string& kBA  = flow::TouchDepletionOperator::kBidAdded;
const std::string& kAF  = flow::TouchDepletionOperator::kAskFilled;
const std::string& kAC  = flow::TouchDepletionOperator::kAskCancelled;
const std::string& kAA  = flow::TouchDepletionOperator::kAskAdded;

} // namespace

// Seeding: the first add into an empty side has no touch, so no `added`, but
// its OFI term is the CKS limiting case (an absent bid is -inf: +qb'); a
// same-price add is `added` and +size OFI; adds behind the touch are neither.
TEST(FlowOperatorTest, SeedAddsAtAndBehindTheTouch) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100)});
  EXPECT_EQ(r.take(), (M{{kOfi, 100}})) << "an add into an empty bid: no touch, OFI +qb'";
  r.frame({add(2, Side::Bid, 10.00, 50)});
  EXPECT_EQ(r.take(), (M{{kBA, 50}, {kOfi, 50}}));
  r.frame({add(3, Side::Bid, 9.99, 200)});
  EXPECT_EQ(r.take(), M{}) << "behind the touch: post == pre, e = qb - qb = 0";
  r.frame({add(4, Side::Ask, 10.01, 80)});
  EXPECT_EQ(r.take(), (M{{kOfi, -80}})) << "an add into an empty ask (+inf): -qa'";
  r.frame({add(5, Side::Ask, 10.02, 300)});
  EXPECT_EQ(r.take(), M{});
  EXPECT_EQ(r.flow->shadowSize(kSym), 5u);
  EXPECT_EQ(r.stored("flow.meta.shadow_orders"), 5);
  EXPECT_EQ(r.stored(flow::OfiOperator::kOfiCum), 100 + 50 - 80);
  // Every meta counter is in the store from the symbol's first event, so an
  // AtomicAccessor reads 0 rather than nothing.
  EXPECT_EQ(r.stored("flow.meta.unattributed"), 0);
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 0);
  EXPECT_EQ(r.stored("flow.meta.unknown_cause"), 0);
  EXPECT_EQ(r.stored("flow.meta.reordered"), 0);
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
  EXPECT_EQ(r.stored(flow::OfiOperator::kOfiCum), 100 + 50 - 80 - 40 - 30 - 60 - 20);
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
  // The shadow consumes its per-level FIFO exactly as the book did: #1 whole
  // (terminal), then 20 of #2.
  EXPECT_EQ(r.flow->shadowSize(kSym), 4u);
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    EXPECT_EQ(ctx.shadow.find(1), nullptr);
    const flow::ShadowOrder* o2 = ctx.shadow.find(2);
    ASSERT_NE(o2, nullptr);
    EXPECT_EQ(o2->size, 30u);
    EXPECT_EQ(o2->filled, 20u);
  });
  // #2's remaining 30 is then deleted: a known order.
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 30}, {kOfi, -30}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 0);
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
  // CKS limiting case: an absent bid is -inf, an absent ask +inf
  EXPECT_EQ(OfiOperator::term({{none, none}, {lv(100, 10), none}}), 10);    // bid appears: +qb'
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), none}, {none, none}}), -10);   // bid empties: -qb
  EXPECT_EQ(OfiOperator::term({{none, none}, {none, lv(200, 4)}}), -4);     // ask appears: -qa'
  EXPECT_EQ(OfiOperator::term({{none, lv(200, 4)}, {none, none}}), 4);      // ask empties: +qa
  EXPECT_EQ(OfiOperator::term({{none, none}, {none, none}}), 0);
  // both sides add up
  EXPECT_EQ(OfiOperator::term({{lv(100, 10), lv(200, 10)}, {lv(100, 12), lv(201, 3)}}), 2 + 10);
}

// ---------------------------------------------------------------------------
// ENC-1646 review fixes (F1-F7). Each was a probe that failed on bc94244..e3d3294.
// ---------------------------------------------------------------------------

// F6: deleting the last bid is -qb (CKS limiting case), consistent with P1.
TEST(FlowOperatorTest, OfiDeletingTheLastBidAgreesWithTouchDepletion) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(4, Side::Ask, 10.01, 80)});
  r.take();
  r.frame({del(1, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 100}, {kOfi, -100}}));
  r.frame({add(2, Side::Bid, 9.50, 500)});   // the side refills: +qb'
  EXPECT_EQ(r.take(), (M{{kOfi, 500}}));
  r.frame({del(4, MutationCause::Execution)});   // the ask empties: +qa
  EXPECT_EQ(r.take(), (M{{kAF, 80}, {kOfi, 80}}));
}

// F2: a duplicate-id add is delete-then-add in OrderBook::addImpl; the old
// shares leave the touch (cancelled), the new ones arrive (added).
TEST(FlowOperatorTest, DuplicateIdAddAtTheTouchIsCancelledPlusAdded) {
  Rig r;
  seed(r);
  r.frame({upd(1, 90, MutationCause::Execution)});
  r.take();
  r.frame({add(1, Side::Bid, 10.00, 30)});   // #1 90 -> book: #2 50 then #1 30 at 10.00
  EXPECT_EQ(r.take(), (M{{kBC, 90}, {kBA, 30}, {kOfi, 80 - 140}}));
  EXPECT_EQ(r.flow->shadowSize(kSym), 5u);
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    const flow::ShadowOrder* o = ctx.shadow.find(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->size, 30u);
    EXPECT_EQ(o->filled, 0u) << "the re-added id starts a fresh history";
    EXPECT_EQ(o->replaces, 0u);
    const auto* q = ctx.shadow.level(Side::Bid, Price{1000});
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(*q, (flow::ShadowOrderBook::Queue{2, 1})) << "delete-then-add: #1 to the back";
  });
}

// F3: ITCH U on an order the shadow never saw, the level's only order.
TEST(FlowOperatorTest, SamePriceReplaceOfAnUnknownOnlyOrderAtTheTouch) {
  Rig r(/*attach=*/false);
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(3, Side::Bid, 9.99, 200)});
  r.frame({add(4, Side::Ask, 10.01, 80)});
  r.attach();
  r.frame({del(1, MutationCause::Replace), add(6, Side::Bid, 10.00, 120, kTs, 1)});
  EXPECT_EQ(r.take(), (M{{kBC, 100}, {kBA, 120}, {kOfi, -100 + 120}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 1);
}

// The reviewer's original F3 probe: an order the old shadow forgot after an
// unattributed trade. With the FIFO shadow (F4) #1 is still known, at 90.
TEST(FlowOperatorTest, SamePriceReplaceAfterAnUnattributedTrade) {
  Rig r;
  seed(r);
  r.frame({del(2, MutationCause::Cancel)});
  r.frame({trade(10.00, 10, Aggressor::Sell, false)});   // #1 100 -> 90
  r.take();
  r.frame({del(1, MutationCause::Replace), add(6, Side::Bid, 10.00, 120, kTs, 1)});
  EXPECT_EQ(r.take(), (M{{kBC, 90}, {kBA, 120}, {kOfi, -90 + 120}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 0);
}

// F4: a partial unattributed fill decrements the front order only; the rest
// of the level stays known.
TEST(FlowOperatorTest, PartialUnattributedTradeConsumesTheFifoFront) {
  Rig r;
  seed(r);
  r.take();
  r.frame({trade(10.00, 30, Aggressor::Sell, false)});   // #1 100 -> 70, #2 untouched
  EXPECT_EQ(r.take(), (M{{kBF, 30}, {kOfi, -30}}));
  EXPECT_EQ(r.flow->shadowSize(kSym), 5u);
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    ASSERT_NE(ctx.shadow.find(1), nullptr);
    ASSERT_NE(ctx.shadow.find(2), nullptr);
    EXPECT_EQ(ctx.shadow.find(1)->size, 70u);
    EXPECT_EQ(ctx.shadow.find(1)->filled, 30u);
    EXPECT_EQ(ctx.shadow.find(2)->size, 50u);
    EXPECT_EQ(ctx.shadow.levelShares(Side::Bid, Price{1000}), 120u);
  });
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 50}, {kOfi, -50}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 0);
}

// F4/F7: the shadow's queue follows OrderBook's rules — a same-price size
// increase keeps its place, a price change goes to the back of the new level
// — so an unattributed trade fills the same orders the book did.
TEST(FlowOperatorTest, FifoIndexMirrorsTheBookQueueRules) {
  Rig r;
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(2, Side::Bid, 10.00, 50)});
  r.frame({add(3, Side::Bid, 9.99, 40)});
  r.frame({add(4, Side::Ask, 10.05, 10)});
  r.frame({upd(1, 150, MutationCause::Replace, kTs, 10.00)});   // same price, size up: keeps the front
  r.frame({upd(3, 40, MutationCause::Replace, kTs, 10.00)});    // 9.99 -> 10.00: back of 10.00
  r.take();
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    const auto* q = ctx.shadow.level(Side::Bid, Price{1000});
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(*q, (flow::ShadowOrderBook::Queue{1, 2, 3}));
    EXPECT_EQ(ctx.shadow.level(Side::Bid, Price{999}), nullptr);
  });
  // 180 = #1 150 whole + 30 of #2; #3 untouched.
  r.frame({trade(10.00, 180, Aggressor::Sell, false)});
  EXPECT_EQ(r.take(), (M{{kBF, 180}, {kOfi, -180}}));
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    EXPECT_EQ(ctx.shadow.find(1), nullptr);
    ASSERT_NE(ctx.shadow.find(2), nullptr);
    ASSERT_NE(ctx.shadow.find(3), nullptr);
    EXPECT_EQ(ctx.shadow.find(2)->size, 20u);
    EXPECT_EQ(ctx.shadow.find(3)->size, 40u);
  });
  // The book agrees: deleting #2 then #3 removes exactly 20 and 40.
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 20}, {kOfi, -20}}));
  r.frame({del(3, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 40}, {kOfi, -40}}));
  EXPECT_EQ(r.stored("flow.meta.unknown_order"), 0);
  EXPECT_EQ(r.flow->shadowSize(kSym), 1u);
}

// F4: orders resting before attach are ahead of every order the shadow saw
// added; an unattributed fill consumes them first.
TEST(FlowOperatorTest, UnattributedTradeSkipsUnknownSharesAheadOfTheShadow) {
  Rig r(/*attach=*/false);
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(4, Side::Ask, 10.01, 80)});
  r.attach();
  r.frame({add(2, Side::Bid, 10.00, 50)});
  r.take();
  r.frame({trade(10.00, 120, Aggressor::Sell, false)});   // book: #1 whole, 20 of #2
  EXPECT_EQ(r.take(), (M{{kBF, 120}, {kOfi, -120}}));
  r.flow->withSymbol(kSym, *r.flow->operators().front(), [&](flow::FlowContext& ctx) {
    ASSERT_NE(ctx.shadow.find(2), nullptr);
    EXPECT_EQ(ctx.shadow.find(2)->size, 30u);
  });
  r.frame({del(2, MutationCause::Cancel)});
  EXPECT_EQ(r.take(), (M{{kBC, 30}, {kOfi, -30}}));
}

// F4: an unattributed trade reports which shadow orders it filled, in FIFO
// order (FlowEvent::fills), for P3/P5.
TEST(FlowOperatorTest, UnattributedTradeReportsItsFills) {
  struct Recorder final : flow::IFlowOperator {
    std::vector<flow::UnattributedFill>* out;
    explicit Recorder(std::vector<flow::UnattributedFill>* o) : out(o) {}
    std::string_view name() const override { return "rec"; }
    std::vector<flow::FlowKey> keys() const override { return {}; }
    void onEvent(flow::FlowContext& ctx) override {
      if (ctx.event->kind == flow::FlowEventKind::Trade) *out = ctx.event->fills;
    }
  };
  std::vector<flow::UnattributedFill> fills;
  Rig r(/*attach=*/false);
  std::vector<std::unique_ptr<flow::IFlowOperator>> ops;
  ops.push_back(std::make_unique<Recorder>(&fills));
  r.flow = std::make_shared<flow::FlowComponent>(
      flow::FlowComponent::Env{&r.dispatcher, &r.store, &r.mgr, nullptr}, std::move(ops));
  r.attach();
  seed(r);
  r.frame({trade(10.00, 120, Aggressor::Sell, false)});
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].before.orderId, 1u);
  EXPECT_EQ(fills[0].qty, 100u);
  EXPECT_TRUE(fills[0].terminal);
  EXPECT_EQ(fills[1].before.orderId, 2u);
  EXPECT_EQ(fills[1].qty, 20u);
  EXPECT_FALSE(fills[1].terminal);
}

// F1: the manager stamps a per-symbol mutation sequence under its lock.
TEST(FlowOperatorTest, ManagerStampsAPerSymbolMutationSeq) {
  OrderBookManager mgr;
  mgr.setTickSize(kSym, 0.01);
  TobChange a, b, c, other, rejected;
  ASSERT_TRUE(mgr.onAdd(kSym, 1, Side::Bid, 10.00, 100, 0, FeedScope{}, false, &a));
  ASSERT_TRUE(mgr.onAdd("OTHER", 1, Side::Bid, 10.00, 100, 0, FeedScope{}, false, &other));
  ASSERT_TRUE(mgr.onUpdate(kSym, 1, FeedScope{}, std::nullopt, 60, false, &b));
  EXPECT_FALSE(mgr.onDelete(kSym, 99, FeedScope{}, false, &rejected));
  ASSERT_TRUE(mgr.onDelete(kSym, 1, FeedScope{}, false, &c));
  EXPECT_EQ(a.mutationSeq, 1u);
  EXPECT_EQ(other.mutationSeq, 1u) << "per symbol";
  EXPECT_EQ(b.mutationSeq, 2u);
  EXPECT_EQ(c.mutationSeq, 3u) << "a rejected mutation takes no sequence";
  EXPECT_EQ(mgr.tobNow(kSym).mutationSeq, 3u);
}

// F1: two strands feeding one symbol can be observed out of mutation order. A
// delete observed before its add must not leave a permanent shadow entry.
TEST(FlowOperatorTest, ReorderedDeleteBeforeItsAddLeavesNoShadowEntry) {
  OrderBookManager mgr;
  mgr.setTickSize(kSym, 0.01);
  AtomicStore store;
  Dispatcher dispatcher(nullptr, &store);
  flow::FlowComponent fc(flow::FlowComponent::Env{&dispatcher, &store, &mgr, nullptr});
  const FeedEvent a = add(42, Side::Bid, 10.00, 100);
  const FeedEvent d = del(42, MutationCause::Cancel);
  TobChange ta{}, td{};
  ta.mutationSeq = 7;
  td.mutationSeq = 8;
  fc.onFlowEvent(feed::FlowObservation{d, td, false});
  fc.onFlowEvent(feed::FlowObservation{a, ta, false});
  EXPECT_EQ(fc.shadowSize(kSym), 0u) << "order 42 no longer rests in the book";
  auto v = store.get(kSym, "flow.meta.reordered");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(std::get<double>(*v), 1.0);
  // A later, in-order reuse of the id is a real add.
  ta.mutationSeq = 9;
  fc.onFlowEvent(feed::FlowObservation{a, ta, false});
  EXPECT_EQ(fc.shadowSize(kSym), 1u);
}

// F1: a late update never rolls an order's shadow state back.
TEST(FlowOperatorTest, ReorderedUpdateDoesNotRegressTheShadow) {
  flow::FlowComponent fc(flow::FlowComponent::Env{});
  TobChange t{};
  const FeedEvent a  = add(5, Side::Bid, 10.00, 100);
  const FeedEvent u1 = upd(5, 80, MutationCause::Cancel);
  const FeedEvent u2 = upd(5, 60, MutationCause::Cancel);
  t.mutationSeq = 1; fc.onFlowEvent(feed::FlowObservation{a, t, false});
  t.mutationSeq = 3; fc.onFlowEvent(feed::FlowObservation{u2, t, false});
  t.mutationSeq = 2; fc.onFlowEvent(feed::FlowObservation{u1, t, false});
  bool seen = false;
  fc.withSymbol(kSym, *fc.operators().front(), [&](flow::FlowContext& ctx) {
    ASSERT_NE(ctx.shadow.find(5), nullptr);
    EXPECT_EQ(ctx.shadow.find(5)->size, 60u);
    EXPECT_EQ(ctx.shadow.find(5)->cancelled, 40u);
    seen = true;
  });
  EXPECT_TRUE(seen);
}

// F1: the ingress stamps every observation — a print carries the sequence of
// the last mutation (it mutates nothing), so a print after its fill is never
// "reordered".
TEST(FlowOperatorTest, IngressStampsEveryObservationIncludingPrints) {
  struct SeqRecorder final : feed::IFlowObserver {
    std::vector<uint64_t> seqs;
    void onFlowEvent(const feed::FlowObservation& o) override { seqs.push_back(o.tob.mutationSeq); }
  };
  Rig r;
  auto rec = std::make_shared<SeqRecorder>();
  r.reg->add(rec);
  seed(r);
  r.frame({upd(1, 60, MutationCause::Execution),
           trade(10.00, 40, Aggressor::Sell, /*bookNeutral=*/true, kTs, 1)});
  EXPECT_EQ(rec->seqs, (std::vector<uint64_t>{1, 2, 3, 4, 5, 6, 6}));
  EXPECT_EQ(r.stored("flow.meta.reordered"), 0);
}
