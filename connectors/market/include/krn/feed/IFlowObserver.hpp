#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "krn/book/DepthTypes.hpp"   // TobChange
#include "krn/feed/FeedEvent.hpp"

namespace krn::feed {

// What an IFlowObserver is shown for one applied book event (ENC-1645, SPEC
// 2026-11-17-order-flow-pipelines D3).
//
// `event` is one of ObAddEvent, ObUpdateEvent, ObDeleteEvent or ObTradeEvent
// — never a TickEvent, ObTickSizeEvent or ObResetEvent. It refers to the
// frame's own event and is valid only for the duration of the call.
//
// `tob` is the top of book immediately before and after this event's book
// mutation, read under the manager's per-symbol lock. For a bookNeutral trade
// (which mutates nothing) `pre == post` = the book as it stood when the print
// was applied. For an ITCH E print that is the POST-fill book, because the
// Execution mutation precedes the print in the frame: a consumer wanting the
// touch the fill hit reads the preceding Execution's `tob.pre`, not the
// print's.
//
// A bookNeutral ObTradeEvent's `restingOrderId`:
//   - set   => the print of a displayed execution against that resting order
//              (ITCH E). Its book change is the ObUpdate/ObDelete with
//              cause=Execution on that id. When that mutation appears
//              earlier in the same frame and the manager rejected it (an
//              unknown order, a stale-gated symbol), the print is an orphan
//              and is NOT observed; it counts in feed.flow_orphan_exec. A
//              print whose frame carries no Execution for its id (an
//              adapter that emits the book change after the print, as the
//              Databento MBO contract does) is observed.
//   - unset => a hidden / non-displayed print (ITCH P): no displayed order
//              traded and the book does not change.
//
// `tob.mutationSeq` is the symbol's mutation sequence of this event's
// mutation; for a bookNeutral trade, the last mutation's (it mutates
// nothing).
//
// `unattributed` is true for a trade that consumed resting liquidity through
// OrderBook::applyTrade (a non-bookNeutral ObTradeEvent, e.g. FeedServer's
// `ob trade`): shares left the book with no per-order event saying whose.
struct FlowObservation {
    const FeedEvent& event;
    TobChange        tob;
    bool             unattributed = false;
};

// Receives every book event an IngressApplier actually applied, in frame
// order, on the ingress source's strand. Called only when the
// OrderBookManager mutator returned true (or, for a bookNeutral trade, when
// the frame was not seq-dropped and the symbol was not stale): nothing is
// observed for a seq-dropped frame, a stale-gated or malformed event, an
// update/delete of an unknown order, or an orphaned execution print (above). Called after the mutator has returned
// and released the manager's locks — never from inside a subscribeDeltas
// handler — so an observer may read the manager, but must not block.
//
// Two ingress sources (two FeedServer sessions, or a WsFeedClient and a
// FeedServer) may call the same observer concurrently from different strands.
// Because the call comes after the book lock is released, two strands feeding
// ONE symbol can then be observed out of mutation order: strand A's add is
// applied, strand B's delete of that order is applied and observed, and only
// then is A's add observed. `tob.mutationSeq` (stamped under the lock, 1, 2,
// 3 … per symbol) is how an observer tells; exact per-order flow requires one
// ingress strand per symbol (ENC-1646 review F1).
class IFlowObserver {
public:
    virtual ~IFlowObserver() = default;

    virtual void onFlowEvent(const FlowObservation& obs) = 0;

    // A symbol's book epoch was reset (ObResetEvent / control reset). The
    // book is now stale until a snapshot; per-order shadow state keyed on the
    // old epoch is no longer meaningful. Default: ignore.
    virtual void onBookReset(const ObResetEvent& /*reset*/) {}
};

// The set of observers an IngressApplier notifies. Shared by every ingress
// source of one MarketConnector. Thread-safe: add/remove may race with
// snapshot(); a frame in flight keeps notifying the set it started with.
class FlowObserverRegistry {
public:
    using List = std::vector<std::shared_ptr<IFlowObserver>>;

    void add(std::shared_ptr<IFlowObserver> obs);
    void remove(const IFlowObserver* obs);
    void clear();

    // The current observers (never null; empty when none).
    std::shared_ptr<const List> snapshot() const;
    bool empty() const;

private:
    mutable std::mutex mu_;
    std::shared_ptr<const List> list_ = std::make_shared<const List>();
    // Mirrors list_->empty(), so IngressApplier's no-observer path is one
    // atomic load per frame instead of a mutex and a shared_ptr copy.
    std::atomic<bool> empty_{true};
};

} // namespace krn::feed
