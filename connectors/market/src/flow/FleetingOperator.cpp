#include "krn/flow/FleetingOperator.hpp"

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <variant>

namespace krn::flow {

const std::string FleetingOperator::kAdds      = "flow.fleeting.adds";
const std::string FleetingOperator::kCancelled = "flow.fleeting.cancelled";
const std::string FleetingOperator::kRatio     = "flow.fleeting.ratio";

namespace {

// The last `window` adds, as a ring of 0/1 slots: slot of add #seq is
// seq % window and holds 1 once the chain that add started is marked
// fleeting. One chain marks at most one slot, so sum <= count and the ratio
// stays in [0, 1].
struct FleetingState final : OperatorState {
  explicit FleetingState(std::size_t window) : slots(window, 0) {}

  struct Chain {
    uint64_t seq;        // the chain's originating add
    uint64_t originTs;   // guards against a reused origin id
    bool     counted;    // already marked fleeting
  };

  std::vector<uint8_t> slots;
  uint64_t adds = 0;   // new-order adds seen; add #adds is the newest
  uint64_t sum  = 0;   // fleeting chains among the adds in the ring
  std::unordered_map<uint64_t, Chain> chains;   // by originOrderId, live chains only

  uint64_t pushAdd() {
    const uint64_t seq = ++adds;
    uint8_t& slot = slots[seq % slots.size()];
    if (adds > slots.size()) sum -= slot;   // the oldest add leaves the ring
    slot = 0;
    return seq;
  }
  bool inRing(uint64_t seq) const { return seq != 0 && seq + slots.size() > adds; }
  void charge(uint64_t seq) {
    if (!inRing(seq)) return;   // its add already left the window
    uint8_t& slot = slots[seq % slots.size()];
    if (slot) return;
    slot = 1;
    ++sum;
  }
  double ratio() const {
    const uint64_t n = std::min<uint64_t>(adds, slots.size());
    return n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
  }
  Chain* chainOf(const ShadowOrder& o) {
    auto it = chains.find(o.originOrderId);
    return it != chains.end() && it->second.originTs == o.originTsNs ? &it->second : nullptr;
  }
  void endChain(const ShadowOrder& o) {
    auto it = chains.find(o.originOrderId);
    if (it != chains.end() && it->second.originTs == o.originTsNs) chains.erase(it);
  }
  // A chain older than maxAgeNs can never become fleeting: forget it, so the
  // map is bounded by the orders added within maxAgeNs (plus live ones until
  // the next sweep). Run every kSweepEvery adds: amortised O(1).
  void sweep(uint64_t nowNs, uint64_t maxAgeNs) {
    for (auto it = chains.begin(); it != chains.end();) {
      const bool old = nowNs > it->second.originTs && nowNs - it->second.originTs >= maxAgeNs;
      it = old ? chains.erase(it) : std::next(it);
    }
  }
};

constexpr uint64_t kSweepEvery = 1024;

} // namespace

bool FleetingOperator::isNewOrder(const FlowEvent& ev) {
  if (ev.kind != FlowEventKind::Add) return false;
  const auto* a = std::get_if<feed::ObAddEvent>(&ev.obs.event);
  return a && !a->replacesOrderId;
}

bool FleetingOperator::isFleetingCancel(const FlowEvent& ev, uint64_t maxAgeNs) {
  if (ev.orderUnknown || !ev.before) return false;
  const ShadowOrder& b = *ev.before;
  if (!b.originKnown) return false;
  switch (ev.kind) {
    case FlowEventKind::Delete:
      if (ev.cause != feed::MutationCause::Cancel && ev.cause != feed::MutationCause::Replace)
        return false;
      break;
    case FlowEventKind::Update:
      if (ev.cause == feed::MutationCause::Replace) break;
      if (ev.cause == feed::MutationCause::Cancel && !ev.after) break;   // cancelled to zero
      return false;
    case FlowEventKind::Add:
      // Review F2: an add of a resting id is delete-then-add; the displaced
      // order (`before`) was pulled.
      if (!ev.duplicateAdd) return false;
      break;
    default:
      return false;
  }
  const uint64_t age = ev.tsNs > b.originTsNs ? ev.tsNs - b.originTsNs : 0;
  return age < maxAgeNs;
}

std::vector<FlowKey> FleetingOperator::keys() const {
  return {
    {kAdds, Publication::Pushed},
    {kCancelled, Publication::Pushed},
    {kRatio, Publication::Pushed},
  };
}

void FleetingOperator::configure(const FlowConfig& cfg, FlowComponent&) {
  maxAgeNs_ = cfg.fleetingMaxAgeNs;
  window_   = cfg.fleetingWindow ? cfg.fleetingWindow : 1;
}

std::unique_ptr<OperatorState> FleetingOperator::makeState() const {
  return std::make_unique<FleetingState>(window_);
}

void FleetingOperator::onEvent(FlowContext& ctx) {
  const FlowEvent& ev = *ctx.event;
  auto& st = ctx.stateAs<FleetingState>();

  // A pull first (a duplicate-id add pulls the displaced chain before its own
  // add starts a new one under the same id).
  if (isFleetingCancel(ev, maxAgeNs_)) {
    // Once per chain. No record (a chain this operator never saw start,
    // e.g. a reused origin id) cannot be deduplicated, so it is not counted.
    FleetingState::Chain* c = st.chainOf(*ev.before);
    if (c && !c->counted) {
      c->counted = true;
      st.charge(c->seq);
      ctx.pub.increment(ctx.symbol, kCancelled, 1.0);
    }
  }

  // The chain ends: forget it (a replace's chain continues in its add half).
  if (ev.before && !ev.orderUnknown) {
    const bool ends = (ev.kind == FlowEventKind::Delete && ev.cause != feed::MutationCause::Replace) ||
                      (ev.kind == FlowEventKind::Update && !ev.after) ||
                      (ev.kind == FlowEventKind::Add && ev.duplicateAdd);
    if (ends) st.endChain(*ev.before);
  }
  for (const UnattributedFill& f : ev.fills)
    if (f.terminal) st.endChain(f.before);

  if (isNewOrder(ev)) {
    const uint64_t seq = st.pushAdd();
    if (!ev.shadowSkipped) st.chains[ev.orderId] = {seq, ev.tsNs, false};
    if (seq % kSweepEvery == 0) st.sweep(ev.tsNs, maxAgeNs_);
    ctx.pub.increment(ctx.symbol, kAdds, 1.0);
    ctx.pub.level(ctx.symbol, kRatio, st.ratio());
  }
}

void FleetingOperator::onReset(FlowContext& ctx) {
  // The shadow is cleared, so no chain can be pulled any more; the ring (the
  // symbol's rate) is kept.
  ctx.stateAs<FleetingState>().chains.clear();
}

} // namespace krn::flow
