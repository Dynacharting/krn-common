#pragma once

#include <cstdint>
#include <string>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// P3 — absorption and refill (SPEC 2026-11-17-order-flow-pipelines D5).
//
//   flow.absorb.{bid,ask}.volume  Pushed  E of the tenure, once, when it fires
//   flow.absorb.{bid,ask}.ratio   Pushed  E / D0, same event
//   flow.absorb.{bid,ask}.px      Pushed  the level's price, same event
//   flow.refill.{bid,ask}         Pushed  shares of one refill add
//
// Tenure, per side: a level's tenure runs while it is the touch, the best
// price on that side BEFORE the event (TobChange.pre). It starts on the first
// event whose pre touch is at a new price, with D0 = that pre touch's
// displayed size (= the post of the event that made it the touch), and ends
// when the touch price on that side changes (the level traded through,
// cancelled away, or a better level opened in front of it) or the side
// empties. A level that empties and is re-established at the same price is a
// NEW tenure: the intermediate event's touch was a different price.
//
// Executed volume E, per tenure:
//   - Execution updates/deletes of an order at the touch price: the shares
//     the shadow saw leave it. An Execution on an order the shadow never saw
//     falls back on the touch's size decrease (TobChange).
//   - unattributed executions (D3, a non-bookNeutral trade that consumed the
//     book): the touch's size decrease on each side.
//   - hidden prints: a bookNeutral trade with no restingOrderId (ITCH P) at
//     the touch price. Its side is decided by PRICE, not by the aggressor
//     field: a print at the best-bid price is absorbed by the bid, at the
//     best-ask price by the ask (the book is never crossed, so at most one
//     side matches). Only a locked book (bid == ask) falls back on the
//     aggressor: Sell -> bid, Buy -> ask, Unknown -> not counted. This keeps
//     P3 independent of ENC-1665 (feedsim's P side is the aggressor, real
//     ITCH's is the resting side) everywhere but a locked book.
//   - NOT the print of a displayed execution (a bookNeutral trade WITH a
//     restingOrderId, ITCH E): its shares are already counted from the
//     Execution mutation on that order. Counting both would double E.
//
// Absorption fires at most once per tenure, on the event that brings
// E >= flow.absorb.minRatio x D0, provided the level still holds after that
// event (the post touch on that side is still at the tenure's price). An
// event that crosses the threshold AND takes the level does not fire, and
// the tenure ends with it.
//
// Refill: a plain add (not a replace's add half — those shares moved, they
// were not reloaded) on the touch's side at the touch price, within
// flow.refill.windowNs of the tenure's latest qualifying execution at that
// price (0 <= add tsNs - exec tsNs <= windowNs). Qualifying executions are
// the displayed per-order ones; unattributed executions (D3) and hidden
// prints do not open a refill window (nothing displayed was taken, or no
// order is known to have been reloaded). Each refill add publishes its
// shares. Refill does not change D0 or E.
class AbsorptionOperator final : public IFlowOperator {
public:
  static const std::string kBidVolume, kBidRatio, kBidPx;
  static const std::string kAskVolume, kAskRatio, kAskPx;
  static const std::string kRefillBid, kRefillAsk;

  std::string_view name() const override { return "absorb"; }
  std::vector<FlowKey> keys() const override;
  void configure(const FlowConfig& cfg, FlowComponent& component) override;
  std::unique_ptr<OperatorState> makeState() const override;
  void onEvent(FlowContext& ctx) override;
  void onReset(FlowContext& ctx) override;

private:
  uint64_t windowNs_ = 5'000'000;
  double   minRatio_ = 2.0;
};

} // namespace krn::flow
