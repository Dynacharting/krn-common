#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "krn/book/DepthTypes.hpp"   // TobChange, Price, Side

namespace krn::flow {

class FlowComponent;

// One resting order as the flow component last saw it (SPEC
// 2026-11-17-order-flow-pipelines D5: "add tsNs, original size, side, price,
// filled"). Built only from observed events — never read out of the book.
struct ShadowOrder {
  uint64_t orderId  = 0;
  Side     side     = Side::Bid;
  Price    price{};          // tick index (compare with TobLevel::price)
  double   priceD   = 0.0;   // as the feed sent it
  uint64_t size     = 0;     // current remaining shares
  uint64_t addSize  = 0;     // shares at this id's add
  uint64_t addTsNs  = 0;     // effective time (D6) of this id's add

  // Identity across replaces (ITCH U: delete{Replace} + add{replacesOrderId}).
  // For an order with no replace history these equal orderId / addTsNs /
  // addSize. When the add named a replaced order the shadow never saw,
  // originKnown is false and origin* describe this id's own add.
  uint64_t originOrderId = 0;
  uint64_t originTsNs    = 0;
  uint64_t originSize    = 0;
  bool     originKnown   = true;
  uint32_t replaces      = 0;   // replaces in the chain so far

  // Shares that left this order chain, by cause, across replaces.
  uint64_t filled    = 0;   // MutationCause::Execution
  uint64_t cancelled = 0;   // MutationCause::Cancel (a replace's shares move, they are not cancelled here)
};

// The delete half of a replace, held until the add half arrives (the two are
// adjacent in one frame for ITCH U). `tob` is the delete's own TobChange, so
// an operator can ask "was the replaced order at the touch?" even when the
// delete emptied that level.
struct ReplacedOrder {
  ShadowOrder order;
  TobChange   tob;
};

// Per-symbol shadow of the resting orders, keyed like OrderKey under the
// IngressApplier's default FeedScope (i.e. by order id within the symbol).
// Operators get a const view; only FlowComponent mutates it.
//
// Bounded by the resting-order count: an entry is erased on every terminal
// event (delete of any cause, update to zero), on an unattributed trade at
// its level (D3 — the book consumed it FIFO with no per-order event, so the
// shadow can no longer say how much of it is left), and the whole map is
// cleared on onBookReset. Orders resting before the shadow started (connect,
// snapshot) are unknown to it; events on them are still shown to operators
// with FlowEvent::orderUnknown set and counted in flow.meta.unknown_order.
class ShadowOrderBook {
public:
  using Map = std::unordered_map<uint64_t, ShadowOrder>;

  const ShadowOrder* find(uint64_t orderId) const {
    auto it = orders_.find(orderId);
    return it == orders_.end() ? nullptr : &it->second;
  }
  std::size_t size() const noexcept { return orders_.size(); }
  const Map&  orders() const noexcept { return orders_; }

  // The most recent replace's delete half, until its add half consumed it.
  const std::optional<ReplacedOrder>& pendingReplace() const noexcept { return pendingReplace_; }

private:
  friend class FlowComponent;
  Map                          orders_;
  std::optional<ReplacedOrder> pendingReplace_;
};

} // namespace krn::flow
