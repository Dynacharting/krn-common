#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
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
  uint64_t filled    = 0;   // MutationCause::Execution, incl. unattributed fills (FIFO, review F4)
  uint64_t cancelled = 0;   // MutationCause::Cancel (a replace's shares move, they are not cancelled here)

  // TobChange::mutationSeq of the last event applied to this order (0 when
  // the feed path stamps none). An older observation never rolls it back.
  uint64_t mutationSeq = 0;
};

// The delete half of a replace, held until the add half arrives (the two are
// adjacent in one frame for ITCH U). `tob` is the delete's own TobChange, so
// an operator can ask "was the replaced order at the touch?" even when the
// delete emptied that level.
//
// Review F3: the delete half of a replace of an order the shadow never saw is
// held too. `orderKnown` is then false and `order` carries only orderId and,
// when the delete's TobChange shows it left a touch (`levelKnown`), that
// touch's side and price.
struct ReplacedOrder {
  ShadowOrder order;
  TobChange   tob;
  bool        orderKnown = true;
  bool        levelKnown = true;   // order.side / order.price are meaningful
};

// Per-symbol shadow of the resting orders, keyed like OrderKey under the
// IngressApplier's default FeedScope (i.e. by order id within the symbol).
// Operators get a const view; only FlowComponent mutates it.
//
// Per-level FIFO index (review F4/F7, SPEC D5): for every (side, price) the
// shadow keeps its order ids in the book's queue order, mirroring OrderBook's
// rules exactly — add = push_back; a same-price size change (up or down)
// keeps the order's place; a price change moves it to the back of the new
// level; a duplicate-id add is delete-then-add (back of its level). A
// priority change also moves to the back in OrderBook, but no FeedEvent
// carries one, so the shadow never sees it. An unattributed trade (D3)
// consumes that queue by quantity exactly as OrderBook::consumeAtLevel does:
// front orders fully filled are erased (terminal), the one partially filled
// is decremented, the rest of the level is untouched. Shares at the level
// that the shadow does not know (orders resting before it started) are
// assumed to be AHEAD of every shadow order — they were added earlier — and
// are consumed first (see FlowComponent).
//
// Ghosts: an order the shadow never saw added that is then repriced (an
// update with a new price) goes to the BACK of its new level in the book, so
// it is no longer "ahead". The shadow keeps its place as a ghost — an id in
// the level's queue that is not in orders() (find() returns null,
// isGhost() true), with its size when the update carried one. A ghost is
// consumed in FIFO order like any order but reported in no fill, and is
// dropped on that id's delete, update to zero, duplicate add or reset. The
// side of a repriced unknown order is read off the TobChange; when it cannot
// be told, no ghost is kept.
//
// Bounded by the resting-order count: an entry is erased on every terminal
// event (delete of any cause, update to zero, fully filled by an unattributed
// trade), and the whole map is cleared on onBookReset. Orders resting before
// the shadow started (connect, snapshot) are unknown to it; events on them
// are still shown to operators with FlowEvent::orderUnknown set and counted
// in flow.meta.unknown_order. Every mutation is O(1) average (hash lookups
// plus list splice); an unattributed trade is O(orders it consumes).
class ShadowOrderBook {
public:
  using Map   = std::unordered_map<uint64_t, ShadowOrder>;
  using Queue = std::list<uint64_t>;   // order ids, front = next to fill

  const ShadowOrder* find(uint64_t orderId) const {
    auto it = orders_.find(orderId);
    return it == orders_.end() ? nullptr : &it->second;
  }
  std::size_t size() const noexcept { return orders_.size(); }
  const Map&  orders() const noexcept { return orders_; }

  // The shadow's queue at (side, price) in book order; null when the shadow
  // holds no order there. Ids index orders(), except ghosts (find() == null).
  const Queue* level(Side side, Price price) const {
    auto it = levels_.find(LevelKey{side, price.ticks});
    return it == levels_.end() ? nullptr : &it->second.ids;
  }
  // Sum of the shadow orders' remaining shares at (side, price). Equal to the
  // book's level size only when the shadow knows every order there.
  uint64_t levelShares(Side side, Price price) const {
    auto it = levels_.find(LevelKey{side, price.ticks});
    return it == levels_.end() ? 0 : it->second.shares;
  }
  // Shares of ghosts with a known size at (side, price).
  uint64_t ghostShares(Side side, Price price) const {
    auto it = levels_.find(LevelKey{side, price.ticks});
    return it == levels_.end() ? 0 : it->second.ghostShares;
  }
  // Shadow and (sized) ghost shares queued ahead of `orderId` at its level
  // (O(position)); 0 for an id the shadow does not hold.
  uint64_t sharesAhead(uint64_t orderId) const;
  std::size_t levelCount() const noexcept { return levels_.size(); }
  bool        isGhost(uint64_t orderId) const { return ghosts_.count(orderId) != 0; }
  std::size_t ghostCount() const noexcept { return ghosts_.size(); }

  // The most recent replace's delete half, until its add half consumed it.
  const std::optional<ReplacedOrder>& pendingReplace() const noexcept { return pendingReplace_; }

private:
  friend class FlowComponent;

  struct LevelKey {
    Side    side;
    int64_t ticks;
    bool operator==(const LevelKey& o) const noexcept { return side == o.side && ticks == o.ticks; }
  };
  struct LevelKeyHash {
    std::size_t operator()(const LevelKey& k) const noexcept {
      const uint64_t t = static_cast<uint64_t>(k.ticks);
      return std::hash<uint64_t>{}((t << 1) ^ (k.side == Side::Bid ? 0u : 1u));
    }
  };
  struct Level {
    Queue    ids;
    uint64_t shares      = 0;   // orders_ entries
    uint64_t ghostShares = 0;   // sized ghosts
  };
  struct Ghost {
    Side     side = Side::Bid;
    Price    price{};
    uint64_t size  = 0;
    bool     sized = false;
  };

  // Mutators (FlowComponent only). They keep orders_, levels_ and pos_ in
  // step; every one is O(1) average.
  ShadowOrder& insert_(const ShadowOrder& o);              // push_back at o's level; id must be absent
  void         erase_(uint64_t orderId);                   // no-op for an unknown id
  void         resize_(ShadowOrder& o, uint64_t newSize);  // keeps queue position
  void         reprice_(ShadowOrder& o, Price p, double pd);   // back of the new level (no-op if same price)
  void         clear_();
  void         link_(const ShadowOrder& o);     // back of o's level
  void         unlink_(const ShadowOrder& o);   // out of o's level
  void         linkId_(uint64_t id, Side s, Price p);     // back of (s, p); no share accounting
  void         unlinkId_(uint64_t id, Side s, Price p);   // out of (s, p); drops an empty level
  // Ghosts (unknown orders whose queue place the shadow does know).
  void         ghostPlace_(uint64_t id, Side s, Price p, std::optional<uint64_t> size);   // back of (s, p)
  void         ghostResize_(uint64_t id, uint64_t size);
  void         ghostErase_(uint64_t id);

  Map                                                        orders_;
  std::unordered_map<LevelKey, Level, LevelKeyHash>          levels_;
  std::unordered_map<uint64_t, Queue::iterator>              pos_;
  std::unordered_map<uint64_t, Ghost>                        ghosts_;
  std::optional<ReplacedOrder>                               pendingReplace_;
};

} // namespace krn::flow
