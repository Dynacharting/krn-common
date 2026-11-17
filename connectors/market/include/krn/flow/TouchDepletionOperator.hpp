#pragma once

#include <string>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// P1 — touch depletion: filled vs pulled (SPEC 2026-11-17-order-flow-pipelines
// D5). Shares removed from / added to the touch level by one event, split by
// cause. "Touch" is the best price on that side BEFORE the event
// (TobChange.pre); a side with no pre touch has nothing to deplete.
//
//   flow.touch.{bid,ask}.filled     Pushed  shares executed off the touch
//   flow.touch.{bid,ask}.cancelled  Pushed  shares pulled off the touch
//   flow.touch.{bid,ask}.added      Pushed  shares added at the touch price
//
// Rules, per event:
//   - removal at the touch, classified by MutationCause: Execution -> filled;
//     Cancel -> cancelled; Replace -> cancelled (a replace away from the touch
//     pulls the touch shares). Unknown -> neither (counted by the core in
//     flow.meta.unknown_cause).
//   - an unattributed execution (D3: a non-bookNeutral trade that consumed
//     the book with no per-order event) -> filled, by the touch's size change.
//   - add at the touch price -> added. An add at a better price opens a new
//     level and is not "at the touch"; an add into an empty side is not either.
//   - same-price replace -> cancelled (the old size) + added (the new size),
//     never the net. For ITCH U (delete{Replace} + add{replacesOrderId}) the
//     add half counts as added when the replaced order was at the touch
//     before its delete and the add is at that same price — even when the
//     delete emptied the level, so the add's own pre touch moved. For an MBO
//     modify (update{Replace}, same id) the same split comes from the shadow.
//   - an update moving an order onto the touch price -> added (its new size).
//   - bookNeutral prints change nothing (pre == post) and count nowhere.
//   - an update/delete of an order the shadow never saw: fall back on the
//     TobChange — the touch's size change on each side, classified by cause
//     (a same-price replace on such an order is then the net, not the split).
class TouchDepletionOperator final : public IFlowOperator {
public:
  static const std::string kBidFilled, kBidCancelled, kBidAdded;
  static const std::string kAskFilled, kAskCancelled, kAskAdded;

  struct Amounts {
    uint64_t filled = 0, cancelled = 0, added = 0;
  };
  struct Result {
    Amounts bid, ask;
  };
  // The whole P1 rule for one resolved event (exposed for tests).
  static Result evaluate(const FlowEvent& ev);

  std::string_view name() const override { return "touch"; }
  std::vector<FlowKey> keys() const override;
  void onEvent(FlowContext& ctx) override;
};

} // namespace krn::flow
