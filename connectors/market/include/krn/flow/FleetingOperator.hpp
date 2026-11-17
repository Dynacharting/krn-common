#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// P4 — fleeting-order rate (SPEC 2026-11-17-order-flow-pipelines D5, Q3
// settled: the ratio is the operator's own rolling series, no two-window join).
//
//   flow.fleeting.adds       Pushed  1 per new order (an add that is not a replace's add half)
//   flow.fleeting.cancelled  Pushed  1 per fleeting order CHAIN, at its first young pull
//   flow.fleeting.ratio      Pushed  fleeting chains / adds over the last flow.fleeting.window
//                                    adds, in [0, 1], published on every add — INCLUDING 0
//                                    (a level, FlowPublisher::level), so a bucket's `last` is exact
//
// Rules:
//   - An add counts only when it is a NEW order. The add half of a replace
//     (ITCH U: add{replacesOrderId}) is not: a replace revises an existing
//     order, and its pull already counts below. So no `adds`, no ratio push,
//     no window slot for it.
//   - A fleeting cancel is a removal-by-pull of a KNOWN order whose age is
//     < flow.fleeting.maxAgeNs (default 50 ms; strictly less). Age is the
//     event's tsNs minus the order chain's originTsNs, so it survives
//     replaces (a replace does not make an order young again). Counted:
//     delete{Cancel}, delete{Replace} (an ITCH U's delete half), update{Cancel}
//     to size 0, and update{Replace} (an MBO modify — the order is pulled and
//     re-posted whatever its new size), and a duplicate-id add (review F2:
//     delete-then-add — the displaced order is pulled; the add itself also
//     counts as a new order). NOT counted: a partial cancel
//     (update{Cancel} to a size > 0 — the order still rests), executions,
//     cause Unknown, and any order whose age is unknown: resting before the
//     shadow started (FlowEvent::orderUnknown), or a chain whose origin the
//     shadow never saw (ShadowOrder::originKnown = false — its true age is
//     larger than the one measurable, so a short measured age proves nothing).
//   - ONCE PER CHAIN: the first such pull of an order chain marks it
//     fleeting and publishes cancelled=1; later replaces and the final cancel
//     of the same chain (originOrderId) never count again. The mark is
//     charged to the ring slot of the chain's ORIGINATING add, so each slot is
//     0 or 1 and the ratio stays in [0, 1].
//   - The window is a ring of the last `window` new-order adds; slot = 1 when
//     the chain that add started was marked fleeting. The ratio is (marked
//     slots) / (adds in the ring); during warm-up the denominator is the adds
//     seen so far. A chain marked after its add left the ring still counts in
//     `cancelled`, but not in the ratio.
//   - Per-chain state lives only for chains younger than maxAgeNs (swept every
//     1024 adds) and is dropped at the chain's terminal event and at a book
//     reset, so it is bounded by the adds within maxAgeNs.
//   - A book reset keeps the ring: the rate is a statistic of the symbol's
//     order flow, not of one book epoch (P5's size median is kept the same way).
class FleetingOperator final : public IFlowOperator {
public:
  static const std::string kAdds;
  static const std::string kCancelled;
  static const std::string kRatio;

  // Is this event a fleeting cancel under `maxAgeNs`? (exposed for tests)
  static bool isFleetingCancel(const FlowEvent& ev, uint64_t maxAgeNs);
  // Is this event a new order (an add that is not a replace's add half)?
  static bool isNewOrder(const FlowEvent& ev);

  std::string_view name() const override { return "fleeting"; }
  std::vector<FlowKey> keys() const override;
  void configure(const FlowConfig& cfg, FlowComponent& component) override;
  std::unique_ptr<OperatorState> makeState() const override;
  void onEvent(FlowContext& ctx) override;
  void onReset(FlowContext& ctx) override;

private:
  uint64_t    maxAgeNs_ = FlowConfig{}.fleetingMaxAgeNs;
  std::size_t window_   = FlowConfig{}.fleetingWindow;
};

} // namespace krn::flow
