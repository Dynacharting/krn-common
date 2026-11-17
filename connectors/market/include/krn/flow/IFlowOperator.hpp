#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "krn/book/DepthTypes.hpp"
#include "krn/feed/FeedEvent.hpp"
#include "krn/feed/IFlowObserver.hpp"
#include "krn/flow/FlowConfig.hpp"
#include "krn/flow/FlowPublisher.hpp"
#include "krn/flow/ShadowOrderBook.hpp"

namespace krn::flow {

class FlowComponent;

enum class FlowEventKind : uint8_t { Add, Update, Delete, Trade };

// One shadow order an unattributed trade consumed (review F4): the order as
// the shadow held it before the trade, the shares the trade took from it, and
// whether that left it at zero (it is then erased from the shadow).
struct UnattributedFill {
  ShadowOrder before;
  uint64_t    qty      = 0;
  bool        terminal = false;
};

// A resting level: side + tick index.
struct OrderLevel {
  Side  side = Side::Bid;
  Price price{};
};

// One observed book event, resolved by FlowComponent before any operator
// sees it: effective time (D6), the shadow order before/after, and the raw
// observation. Valid only for the duration of the onEvent call.
struct FlowEvent {
  FlowEvent(FlowEventKind k, const feed::FlowObservation& o, const std::string& sym)
    : kind(k), obs(o), symbol(sym) {}

  FlowEventKind               kind;
  const feed::FlowObservation& obs;      // the raw event + TobChange + unattributed
  const std::string&          symbol;

  // D6: the exchange time when the event carries one, else a monotonic
  // receive clock (steady_clock, ns); exchangeTs says which.
  uint64_t tsNs       = 0;
  bool     exchangeTs = true;

  // Update/Delete: the event's cause. Trade: Execution when unattributed,
  // Unknown otherwise. Add: Replace when it is a replace's add half, else
  // Unknown.
  feed::MutationCause cause = feed::MutationCause::Unknown;
  bool unattributed = false;   // a non-bookNeutral trade that consumed the book (D3)
  bool bookNeutral  = false;   // a print that mutated nothing (pre == post)

  // Add/Update/Delete: the order id. Trade: restingOrderId, or 0.
  uint64_t orderId = 0;

  // The order as the shadow held it before this event. For an Add it is the
  // replaced order (a replace's add half whose delete half the shadow saw),
  // and `replacedTob` is that delete's TobChange — or, with `duplicateAdd`,
  // the resting order of the same id that this add replaced. Empty otherwise.
  std::optional<ShadowOrder> before;
  std::optional<TobChange>   replacedTob;
  // Add, replace's add half: where the replaced order rested — from `before`
  // when the shadow knew it; for an order it never saw (review F3), the touch
  // its delete half left, when that delete changed a touch. `replacedTob` is
  // set in both cases; empty when the level cannot be told.
  std::optional<OrderLevel>  replacedLevel;
  // Add of an id already resting (review F2): OrderBook::addImpl deletes the
  // old order and adds the new one at the back of its level. `before` is the
  // old order; `after` starts a fresh history (no replace chain). When the
  // shadow never saw the old order, `before` is empty and `orderUnknown` is
  // set (detected from the TobChange, or a ghost of that id).
  bool duplicateAdd = false;
  // The order as the shadow holds it after this event; empty when the event
  // removed it (or for a trade).
  std::optional<ShadowOrder> after;

  // Update/Delete of an order the shadow never saw added (resting before the
  // shadow started). `before`/`after` are empty; operators fall back on
  // `obs.tob` or skip the event.
  bool orderUnknown = false;

  // Add: the order's price as a tick index (the shadow's `price`).
  // Update with a new price: the new tick index. Unattributed trade: the
  // trade's tick index. Otherwise zero.
  Price eventPrice{};

  // Unattributed trade (review F4): the shadow orders it consumed, in FIFO
  // order. Shares it took from orders the shadow does not know are not here.
  std::vector<UnattributedFill> fills;

  // Review F1: this observation is older (TobChange::mutationSeq) than one
  // already seen for the symbol — two ingress strands fed it. Counted in
  // flow.meta.reordered. Exact flow requires one ingress strand per symbol.
  bool reordered = false;
  // The shadow did not apply this event because it already holds a newer
  // state for the order (a late update/delete), or the order's terminal event
  // was observed first (a late add). `before`/`after` are then empty.
  bool shadowSkipped = false;

  const TobChange& tob() const noexcept { return obs.tob; }
};

// Per-symbol state an operator owns. FlowComponent creates one per symbol per
// operator (IFlowOperator::makeState) and only ever touches it under that
// symbol's mutex.
struct OperatorState {
  virtual ~OperatorState() = default;
};

// What an operator is handed for one symbol, always under that symbol's
// mutex. `event` is null outside onEvent (onReset, withSymbol).
struct FlowContext {
  const std::string&      symbol;
  const FlowEvent*        event;
  const ShadowOrderBook&  shadow;
  const FlowPublisher&    pub;
  const FlowConfig&       cfg;
  OperatorState*          state;   // this operator's own; null if makeState() returned null

  template <class T> T& stateAs() const { return static_cast<T&>(*state); }
};

// One flow operator (P0..P5). Each lives in its own class/file under
// connectors/market/{include/krn/flow,src/flow}/ and is added to the default
// set with one line in src/flow/FlowOperators.cpp.
//
// Threading: onEvent/onReset run on an ingress strand under the symbol's
// mutex (D3: two strands may feed one symbol — but then events can be
// observed out of mutation order; FlowEvent::reordered marks those, and exact
// flow requires one ingress strand per symbol). They must not block, and must
// not call back into FlowComponent::withSymbol for the SAME symbol (the
// mutex is not recursive). An exception is caught, counted
// (`flow.operator_error`) and does not stop the other operators.
class IFlowOperator {
public:
  virtual ~IFlowOperator() = default;

  // Short stable name, for logs ("ofi", "touch", "sweep", …).
  virtual std::string_view name() const = 0;

  // Every key this operator publishes, with how (D4). docs/atomic-keys.md
  // must list exactly these (FlowVocabularyTest), and every Pushed key must
  // be notified by FlowPushedKeysTest's scenario — extend it with yours.
  virtual std::vector<FlowKey> keys() const = 0;

  // Once, at FlowComponent::start() (or lazily on the first event), with the
  // flow.* config as dispatched (D7) — read your own fields here. `component`
  // stays valid for the operator's lifetime: keep it for withSymbol() and
  // io() (e.g. a sweep close timer).
  virtual void configure(const FlowConfig& /*cfg*/, FlowComponent& /*component*/) {}

  // Fresh per-symbol state (null = stateless).
  virtual std::unique_ptr<OperatorState> makeState() const { return nullptr; }

  // Before an event is taken under ITS symbol's lock, with no flow lock held:
  // every operator sees every symbol's event time here first. For operators
  // whose state closes on "a later event of any symbol" (P2 sweeps) — call
  // withSymbol() on the other symbols from here.
  virtual void onBeforeEvent(const std::string& /*symbol*/, uint64_t /*tsNs*/) {}

  // The same hook with the time source (D6): `exchangeTs` is true when
  // `tsNs` is the event's exchange time, false when it is the receive-clock
  // fallback (steady_clock ns). FlowComponent calls THIS one; the default
  // forwards to onBeforeEvent(symbol, tsNs), so an operator overrides
  // whichever it needs. (A distinct name rather than an overload, so an
  // operator overriding one does not hide the other: -Woverloaded-virtual.)
  virtual void onBeforeEventTimed(const std::string& symbol, uint64_t tsNs, bool /*exchangeTs*/) {
    onBeforeEvent(symbol, tsNs);
  }

  // One resolved event, under the symbol's mutex. The shadow has already
  // been updated (FlowEvent::before/after carry both sides).
  virtual void onEvent(FlowContext& ctx) = 0;

  // The symbol's book epoch was reset; the shadow has already been cleared.
  // State is kept unless you reset it here.
  virtual void onReset(FlowContext& /*ctx*/) {}

  // First step of ~FlowComponent, after it stopped accepting withSymbol
  // calls and before any operator is destroyed: cancel your timers here.
  // The component is still intact. Default: nothing.
  virtual void shutdown() {}
};

} // namespace krn::flow
