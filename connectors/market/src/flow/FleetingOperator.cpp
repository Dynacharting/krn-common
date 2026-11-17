#include "krn/flow/FleetingOperator.hpp"

namespace krn::flow {

const std::string FleetingOperator::kAdds      = "flow.fleeting.adds";
const std::string FleetingOperator::kCancelled = "flow.fleeting.cancelled";
const std::string FleetingOperator::kRatio     = "flow.fleeting.ratio";

namespace {

// The last `window` adds, as a ring of per-add fleeting-cancel counts.
struct FleetingState final : OperatorState {
  explicit FleetingState(std::size_t window) : slots(window, 0) {}
  std::vector<uint32_t> slots;
  std::size_t head  = 0;   // index of the newest slot
  std::size_t count = 0;   // adds in the ring (<= slots.size())
  uint64_t    sum   = 0;   // fleeting cancels in the ring

  void clear() {
    std::fill(slots.begin(), slots.end(), 0u);
    head = count = 0;
    sum = 0;
  }
  void pushAdd() {
    head = count == 0 ? 0 : (head + 1) % slots.size();
    if (count == slots.size()) sum -= slots[head];   // the oldest add leaves
    else ++count;
    slots[head] = 0;
  }
  void cancelled() {
    if (count == 0) return;   // no add yet: no slot to charge
    ++slots[head];
    ++sum;
  }
  double ratio() const { return count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0; }
};

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
  if (isNewOrder(ev)) {
    st.pushAdd();
    ctx.pub.increment(ctx.symbol, kAdds, 1.0);
    ctx.pub.level(ctx.symbol, kRatio, st.ratio());
    return;
  }
  if (isFleetingCancel(ev, maxAgeNs_)) {
    st.cancelled();
    ctx.pub.increment(ctx.symbol, kCancelled, 1.0);
  }
}

void FleetingOperator::onReset(FlowContext& ctx) {
  ctx.stateAs<FleetingState>().clear();
}

} // namespace krn::flow
