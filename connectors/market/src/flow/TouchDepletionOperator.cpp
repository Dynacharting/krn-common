#include "krn/flow/TouchDepletionOperator.hpp"

#include <cstdint>

namespace krn::flow {

const std::string TouchDepletionOperator::kBidFilled    = "flow.touch.bid.filled";
const std::string TouchDepletionOperator::kBidCancelled = "flow.touch.bid.cancelled";
const std::string TouchDepletionOperator::kBidAdded     = "flow.touch.bid.added";
const std::string TouchDepletionOperator::kAskFilled    = "flow.touch.ask.filled";
const std::string TouchDepletionOperator::kAskCancelled = "flow.touch.ask.cancelled";
const std::string TouchDepletionOperator::kAskAdded     = "flow.touch.ask.added";

namespace {

const TobLevel& level(const TopOfBook& t, Side s) { return s == Side::Bid ? t.bid : t.ask; }

// The touch level's signed size change as TobChange shows it. Zero when there
// was no touch, or when a better level opened in front of an untouched one.
int64_t tobDelta(const TobChange& tc, Side s) {
  const TobLevel& a = level(tc.pre, s);
  const TobLevel& b = level(tc.post, s);
  if (!a.present) return 0;
  if (b.present && b.price == a.price)
    return static_cast<int64_t>(b.size) - static_cast<int64_t>(a.size);
  const bool worse = !b.present ||
                     (s == Side::Bid ? b.price.ticks < a.price.ticks : b.price.ticks > a.price.ticks);
  return worse ? -static_cast<int64_t>(a.size) : 0;
}

bool atTouch(const TobLevel& touch, Price p) { return touch.present && touch.price == p; }

} // namespace

TouchDepletionOperator::Result TouchDepletionOperator::evaluate(const FlowEvent& ev) {
  Result r;
  auto amounts = [&](Side s) -> Amounts& { return s == Side::Bid ? r.bid : r.ask; };
  auto removed = [&](Side s, uint64_t shares, feed::MutationCause cause) {
    switch (cause) {
      case feed::MutationCause::Execution: amounts(s).filled    += shares; break;
      case feed::MutationCause::Cancel:
      case feed::MutationCause::Replace:   amounts(s).cancelled += shares; break;
      case feed::MutationCause::Unknown:   break;   // flow.meta.unknown_cause
    }
  };
  const TobChange& tc = ev.tob();

  switch (ev.kind) {
    case FlowEventKind::Trade: {
      if (!ev.unattributed) return r;   // a print: pre == post
      for (Side s : {Side::Bid, Side::Ask}) {
        const int64_t d = tobDelta(tc, s);
        if (d < 0) amounts(s).filled += static_cast<uint64_t>(-d);
      }
      return r;
    }
    case FlowEventKind::Add: {
      if (!ev.after) return r;
      const ShadowOrder& o = *ev.after;
      // ITCH U add half: judged against the touch as it stood before the
      // delete half, so a same-price replace of the level's only order still
      // reads cancelled + added.
      if (ev.before && ev.replacedTob) {
        const ShadowOrder& old = *ev.before;
        if (old.side == o.side && old.price == o.price &&
            atTouch(level(ev.replacedTob->pre, old.side), old.price)) {
          amounts(o.side).added += o.size;
          return r;
        }
      }
      if (atTouch(level(tc.pre, o.side), o.price)) amounts(o.side).added += o.size;
      return r;
    }
    case FlowEventKind::Update:
    case FlowEventKind::Delete: {
      if (ev.orderUnknown || !ev.before) {
        for (Side s : {Side::Bid, Side::Ask}) {
          const int64_t d = tobDelta(tc, s);
          if (d < 0)      removed(s, static_cast<uint64_t>(-d), ev.cause);
          else if (d > 0) amounts(s).added += static_cast<uint64_t>(d);
        }
        return r;
      }
      const ShadowOrder& b = *ev.before;
      const TobLevel& touch = level(tc.pre, b.side);
      const bool was = atTouch(touch, b.price);
      if (ev.kind == FlowEventKind::Delete) {
        if (was) removed(b.side, b.size, ev.cause);
        return r;
      }
      const ShadowOrder* a = ev.after ? &*ev.after : nullptr;   // null: updated to zero
      const bool now = a && atTouch(touch, a->price);
      if (ev.cause == feed::MutationCause::Replace) {
        // MBO modify: the old shares leave, the new ones arrive — never the net.
        if (was) removed(b.side, b.size, ev.cause);
        if (now) amounts(b.side).added += a->size;
        return r;
      }
      if (was && now) {
        if (a->size < b.size)      removed(b.side, b.size - a->size, ev.cause);
        else if (a->size > b.size) amounts(b.side).added += a->size - b.size;
      } else {
        if (was) removed(b.side, b.size, ev.cause);
        if (now) amounts(b.side).added += a->size;
      }
      return r;
    }
  }
  return r;
}

std::vector<FlowKey> TouchDepletionOperator::keys() const {
  return {
    {kBidFilled, Publication::Pushed}, {kBidCancelled, Publication::Pushed},
    {kBidAdded, Publication::Pushed},  {kAskFilled, Publication::Pushed},
    {kAskCancelled, Publication::Pushed}, {kAskAdded, Publication::Pushed},
  };
}

void TouchDepletionOperator::onEvent(FlowContext& ctx) {
  const Result r = evaluate(*ctx.event);
  const auto& p = ctx.pub;
  p.increment(ctx.symbol, kBidFilled,    static_cast<double>(r.bid.filled));
  p.increment(ctx.symbol, kBidCancelled, static_cast<double>(r.bid.cancelled));
  p.increment(ctx.symbol, kBidAdded,     static_cast<double>(r.bid.added));
  p.increment(ctx.symbol, kAskFilled,    static_cast<double>(r.ask.filled));

  p.increment(ctx.symbol, kAskAdded,     static_cast<double>(r.ask.added));
}

} // namespace krn::flow
