#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// The rolling median of the last `capacity` values, as a sorted buffer plus
// an insertion-order ring: push() is two binary searches and two bounded
// moves (<= capacity elements), median() is O(1). P5 uses capacity 1000.
class RollingMedian {
public:
  explicit RollingMedian(std::size_t capacity) : cap_(capacity), ring_(capacity, 0) {
    sorted_.reserve(capacity);
  }
  void push(uint64_t v);
  // The median of the values held; for an even count the mean of the two
  // middle values. 0 when empty.
  double median() const;
  std::size_t size() const noexcept { return sorted_.size(); }
  std::size_t capacity() const noexcept { return cap_; }
  bool full() const noexcept { return sorted_.size() == cap_; }
  void clear() { sorted_.clear(); head_ = 0; }

private:
  std::size_t           cap_;
  std::vector<uint64_t> ring_;     // insertion order; ring_[head_] is the oldest once full
  std::size_t           head_ = 0;
  std::vector<uint64_t> sorted_;
};

// P5 — large-order lifecycle (SPEC 2026-11-17-order-flow-pipelines D5).
//
//   flow.large.filled       Pushed  shares of the chain executed, at its end (non-zero only)
//   flow.large.cancelled    Pushed  shares of the chain pulled, at its end (non-zero only)
//   flow.large.lifetime_ms  Pushed  end tsNs - chain originTsNs, in ms (a level: 0 is notified)
//   flow.large.end.fill     Pushed  1 when the final removal was an execution
//   flow.large.end.cancel   Pushed  1 when the final removal was a pull
//   flow.large.active       Stored  live large chains for the symbol
//
// "Large": an add of size >= max(flow.large.minShares, flow.large.mult x the
// rolling median of the last 1000 NEW-order add sizes). Until 1000 adds have
// been seen only minShares applies. The add being judged is compared against
// the median of the adds BEFORE it, then enters the sample. A replace's add
// half does not enter the sample (it is not a new order — the same rule as P4).
//
// Chains: a large order is tracked through replaces (ITCH U: delete{Replace}
// + add{replacesOrderId}); the new id inherits largeness. A replace's add half
// is also judged on its own size, so an order replaced UP past the threshold
// becomes large then; once large a chain stays large until it ends. An MBO
// modify (update{Replace}, same id) keeps the id and so the chain.
//
// End (one publication per large chain): the chain's final removal —
//   delete{Execution} (or update{Execution} to 0 — not reachable today:
//     OrderBookManager rejects newSize 0, so a removal is always a delete) -> end.fill
//   delete{Cancel}, update{Cancel|Replace} to 0         -> end.cancel
//   a delete{Replace} whose add half never arrives (the next event of the
//     symbol is not it)                                  -> end.cancel
//   delete/update-to-0 with cause Unknown               -> neither end flag;
//     filled/cancelled/lifetime still published
// with filled/cancelled the chain's cumulative shares by cause (ShadowOrder),
// including partial fills and cancels along the way and the removed
// remainder. lifetime_ms runs from the chain's origin; when the chain's origin
// was never seen (originKnown = false) it runs from the first add the shadow
// saw, a lower bound.
//
// Also ended:
//   an unattributed trade (D3) that the shadow's FIFO consumption (review F4)
//     fills it completely                               -> end.fill (a partial
//     fill leaves it live, its shares in the chain's `filled`)
//   a duplicate-id add displacing it (review F2: delete-then-add)
//                                                       -> end.cancel; the add
//     is judged afresh as a new order
//
// Lost, not ended (active decremented, nothing published): every large order
// at a book reset, and any live chain the shadow no longer holds after an
// unattributed trade other than through a reported fill (a safety net). A
// late add whose terminal event was observed first (review F1) starts no chain.
// Orders resting before connect are unknown and never large. The size median
// is kept across book resets (a statistic of the symbol, not of one epoch).
class LargeOrderOperator final : public IFlowOperator {
public:
  static const std::string kFilled;
  static const std::string kCancelled;
  static const std::string kLifetimeMs;
  static const std::string kEndFill;
  static const std::string kEndCancel;
  static const std::string kActive;
  static constexpr std::size_t kMedianAdds = 1000;

  // The threshold given the sample so far (exposed for tests).
  static double threshold(const RollingMedian& m, uint64_t minShares, double mult);

  std::string_view name() const override { return "large"; }
  std::vector<FlowKey> keys() const override;
  void configure(const FlowConfig& cfg, FlowComponent& component) override;
  std::unique_ptr<OperatorState> makeState() const override;
  void onEvent(FlowContext& ctx) override;
  void onReset(FlowContext& ctx) override;

private:
  uint64_t minShares_ = FlowConfig{}.largeMinShares;
  double   mult_      = FlowConfig{}.largeMult;
};

} // namespace krn::flow
