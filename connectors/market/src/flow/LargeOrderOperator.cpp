#include "krn/flow/LargeOrderOperator.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <variant>

namespace krn::flow {

const std::string LargeOrderOperator::kFilled     = "flow.large.filled";
const std::string LargeOrderOperator::kCancelled  = "flow.large.cancelled";
const std::string LargeOrderOperator::kLifetimeMs = "flow.large.lifetime_ms";
const std::string LargeOrderOperator::kEndFill    = "flow.large.end.fill";
const std::string LargeOrderOperator::kEndCancel  = "flow.large.end.cancel";
const std::string LargeOrderOperator::kActive     = "flow.large.active";

void RollingMedian::push(uint64_t v) {
  if (cap_ == 0) return;
  if (sorted_.size() == cap_) {
    const uint64_t oldest = ring_[head_];
    sorted_.erase(std::lower_bound(sorted_.begin(), sorted_.end(), oldest));
    ring_[head_] = v;
    head_ = (head_ + 1) % cap_;
  } else {
    ring_[sorted_.size()] = v;   // head_ stays 0 until full: ring_[0] is the oldest
  }
  sorted_.insert(std::upper_bound(sorted_.begin(), sorted_.end(), v), v);
}

double RollingMedian::median() const {
  const std::size_t n = sorted_.size();
  if (n == 0) return 0.0;
  if (n % 2) return static_cast<double>(sorted_[n / 2]);
  return (static_cast<double>(sorted_[n / 2 - 1]) + static_cast<double>(sorted_[n / 2])) / 2.0;
}

namespace {

enum class End { Fill, Cancel, Neither };

struct PendingReplace {
  uint64_t    orderId;
  ShadowOrder order;   // as the delete half left it
  uint64_t    tsNs;
};

struct LargeState final : OperatorState {
  RollingMedian                  median{LargeOrderOperator::kMedianAdds};
  std::unordered_set<uint64_t>   live;      // current order id of each live large chain
  std::optional<PendingReplace>  pending;    // a large chain between its delete{Replace} and add halves
  double                         activePublished = -1;

  std::size_t active() const { return live.size() + (pending ? 1 : 0); }
};

void publishEnd(const FlowContext& ctx, const ShadowOrder& o, uint64_t filled, uint64_t cancelled,
                uint64_t endTs, End why) {
  const auto& p = ctx.pub;
  p.increment(ctx.symbol, LargeOrderOperator::kFilled, static_cast<double>(filled));
  p.increment(ctx.symbol, LargeOrderOperator::kCancelled, static_cast<double>(cancelled));
  const uint64_t life = endTs > o.originTsNs ? endTs - o.originTsNs : 0;
  p.level(ctx.symbol, LargeOrderOperator::kLifetimeMs, static_cast<double>(life) / 1e6);
  if (why == End::Fill)   p.increment(ctx.symbol, LargeOrderOperator::kEndFill, 1.0);
  if (why == End::Cancel) p.increment(ctx.symbol, LargeOrderOperator::kEndCancel, 1.0);
}

void publishActive(const FlowContext& ctx, LargeState& st) {
  const double a = static_cast<double>(st.active());
  if (a == st.activePublished) return;
  st.activePublished = a;
  ctx.pub.state(ctx.symbol, LargeOrderOperator::kActive, a);
}

} // namespace

double LargeOrderOperator::threshold(const RollingMedian& m, uint64_t minShares, double mult) {
  const double floor = static_cast<double>(minShares);
  if (!m.full()) return floor;
  return std::max(floor, mult * m.median());
}

std::vector<FlowKey> LargeOrderOperator::keys() const {
  return {
    {kFilled, Publication::Pushed},     {kCancelled, Publication::Pushed},
    {kLifetimeMs, Publication::Pushed}, {kEndFill, Publication::Pushed},
    {kEndCancel, Publication::Pushed},  {kActive, Publication::Stored},
  };
}

void LargeOrderOperator::configure(const FlowConfig& cfg, FlowComponent&) {
  minShares_ = cfg.largeMinShares;
  mult_      = cfg.largeMult;
}

std::unique_ptr<OperatorState> LargeOrderOperator::makeState() const {
  return std::make_unique<LargeState>();
}

void LargeOrderOperator::onEvent(FlowContext& ctx) {
  const FlowEvent& ev = *ctx.event;
  auto& st = ctx.stateAs<LargeState>();
  const auto* addEv = ev.kind == FlowEventKind::Add ? std::get_if<feed::ObAddEvent>(&ev.obs.event)
                                                    : nullptr;

  // A pending replace lives exactly until the next event (as in the shadow):
  // anything but its add half means the order was pulled for good.
  if (st.pending) {
    const bool isItsAdd = addEv && addEv->replacesOrderId &&
                          *addEv->replacesOrderId == st.pending->orderId;
    if (!isItsAdd) {
      const ShadowOrder& o = st.pending->order;
      publishEnd(ctx, o, o.filled, o.cancelled + o.size, st.pending->tsNs, End::Cancel);
      st.pending.reset();
    }
  }

  switch (ev.kind) {
    case FlowEventKind::Add: {
      if (!addEv) break;
      // Review F2: an add of a resting id displaces it (delete-then-add): a
      // large order there ends as a pull, and the add starts afresh.
      if (ev.duplicateAdd && ev.before && st.live.erase(ev.before->orderId)) {
        const ShadowOrder& b = *ev.before;
        publishEnd(ctx, b, b.filled, b.cancelled + b.size, ev.tsNs, End::Cancel);
      }
      const bool qualifies =
          static_cast<double>(addEv->size) >= threshold(st.median, minShares_, mult_);
      // Review F1: a late add whose terminal event was already observed does
      // not rest, so it starts no chain (it still is a size sample).
      const bool rests = ev.after.has_value() && !ev.shadowSkipped;
      if (!addEv->replacesOrderId) {
        st.median.push(addEv->size);
        if (qualifies && rests) st.live.insert(addEv->orderId);
        break;
      }
      const bool inherited = st.pending.has_value();   // only its own add half reaches here
      st.pending.reset();
      if ((inherited || qualifies) && rests) st.live.insert(addEv->orderId);
      break;
    }
    case FlowEventKind::Delete:
    case FlowEventKind::Update: {
      auto it = st.live.find(ev.orderId);
      if (it == st.live.end() || !ev.before) break;
      const ShadowOrder& b = *ev.before;
      if (ev.kind == FlowEventKind::Delete) {
        st.live.erase(it);
        switch (ev.cause) {
          case feed::MutationCause::Replace:
            st.pending = PendingReplace{ev.orderId, b, ev.tsNs};
            break;
          case feed::MutationCause::Execution:
            publishEnd(ctx, b, b.filled, b.cancelled, ev.tsNs, End::Fill);
            break;
          case feed::MutationCause::Cancel:
            publishEnd(ctx, b, b.filled, b.cancelled, ev.tsNs, End::Cancel);
            break;
          case feed::MutationCause::Unknown:
            publishEnd(ctx, b, b.filled, b.cancelled, ev.tsNs, End::Neither);
            break;
        }
        break;
      }
      if (ev.after) break;   // still resting
      st.live.erase(it);     // updated to zero: terminal
      switch (ev.cause) {
        case feed::MutationCause::Execution:
          publishEnd(ctx, b, b.filled + b.size, b.cancelled, ev.tsNs, End::Fill);
          break;
        case feed::MutationCause::Cancel:
        case feed::MutationCause::Replace:
          publishEnd(ctx, b, b.filled, b.cancelled + b.size, ev.tsNs, End::Cancel);
          break;
        case feed::MutationCause::Unknown:
          publishEnd(ctx, b, b.filled, b.cancelled, ev.tsNs, End::Neither);
          break;
      }
      break;
    }
    case FlowEventKind::Trade: {
      if (!ev.unattributed) break;
      // Review F4: the shadow consumes the level FIFO by quantity. A large
      // order it fills completely ends as a fill; a partial fill leaves it
      // live (the shadow's `filled` carries the shares to its end).
      for (const UnattributedFill& f : ev.fills) {
        if (!st.live.erase(f.before.orderId)) continue;
        publishEnd(ctx, f.before, f.before.filled + f.qty, f.before.cancelled, ev.tsNs, End::Fill);
      }
      // Safety net: any other live chain the shadow no longer holds is lost.
      for (auto it = st.live.begin(); it != st.live.end();) {
        if (!ctx.shadow.find(*it)) it = st.live.erase(it);
        else ++it;
      }
      break;
    }
  }
  publishActive(ctx, st);
}

void LargeOrderOperator::onReset(FlowContext& ctx) {
  auto& st = ctx.stateAs<LargeState>();
  st.live.clear();
  st.pending.reset();
  publishActive(ctx, st);
}

} // namespace krn::flow
