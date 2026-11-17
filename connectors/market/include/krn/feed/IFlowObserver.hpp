#pragma once

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
// (which mutates nothing) `pre == post` = the book as it stood.
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
// observed for a seq-dropped frame, a stale-gated or malformed event, or an
// update/delete of an unknown order. Called after the mutator has returned
// and released the manager's locks — never from inside a subscribeDeltas
// handler — so an observer may read the manager, but must not block.
//
// Two ingress sources (two FeedServer sessions, or a WsFeedClient and a
// FeedServer) may call the same observer concurrently from different strands.
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
};

} // namespace krn::feed
