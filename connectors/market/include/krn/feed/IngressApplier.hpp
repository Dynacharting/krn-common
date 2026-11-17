#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "krn/feed/FeedEvent.hpp"
#include "krn/feed/IFlowObserver.hpp"

namespace krn {
class Dispatcher;
class OrderBookManager;
} // namespace krn

namespace krn::feed {

// The one place a translated feed frame meets the book (ENC-1645, SPEC
// 2026-11-17-order-flow-pipelines D3). WsFeedClient (after its IFeedAdapter)
// and FeedServer (after its JSON translation) both route through it.
//
// Routing, per event, in frame order:
//   TickEvent       -> Dispatcher::onTick (payload moved out of the event)
//   ObAddEvent      -> OrderBookManager::onAdd
//   ObUpdateEvent   -> OrderBookManager::onUpdate
//   ObDeleteEvent   -> OrderBookManager::onDelete
//   ObTradeEvent    -> OrderBookManager::onTrade, unless bookNeutral
//   ObTickSizeEvent -> OrderBookManager::setTickSize
//   ObResetEvent    -> OrderBookManager::onReset (+ IFlowObserver::onBookReset)
// A frame whose seq gate says "drop" skips every Ob{Add,Update,Delete,Trade};
// ticks, tick sizes and resets still apply (unchanged from WsFeedClient M7).
//
// Observers: see IFlowObserver. With no observer registered the apply path
// is the pre-ENC-1645 one (no TobChange is computed).
//
// Not thread-safe per instance: one IngressApplier per ingress strand. The
// Dispatcher, manager and registry it points at may be shared.
class IngressApplier {
public:
    struct FrameResult {
        bool        dropBook = false;  // the frame's book events were skipped (seq gap / stale)
        std::size_t applied  = 0;      // book events the manager accepted (mutator returned true)
        std::size_t observed = 0;      // events observed (each shown to every registered observer)
    };

    // Any pointer may be null: a null dispatcher drops ticks, a null manager
    // drops every Ob* event, a null registry observes nothing.
    IngressApplier(Dispatcher* dispatcher,
                   OrderBookManager* obManager,
                   std::shared_ptr<FlowObserverRegistry> observers = nullptr);

    // Apply one frame. M7 seq gate: the frame's sequence is evaluated exactly
    // once, on the first Ob{Add,Update,Delete,Trade} event carrying a `seq`,
    // through OrderBookManager::onSeq; a gap or stale state drops the frame's
    // book events. A frame with no seq is never dropped here (per-event stale
    // gating inside the manager still applies).
    FrameResult apply(std::vector<FeedEvent>& frame);

    // Apply one frame whose seq gate the caller has already evaluated
    // (FeedServer gates a JSON line before validating its body). `seq` fields
    // on the events are ignored.
    FrameResult applyGated(std::vector<FeedEvent>& frame, bool dropBook);

    // The M7 gate on its own: true = drop this frame's book events. Calls
    // OrderBookManager::onSeq at most once (it has side effects).
    bool seqGate(const std::vector<FeedEvent>& frame);

    Dispatcher*       dispatcher() const noexcept { return dispatcher_; }
    OrderBookManager* orderBooks() const noexcept { return obManager_; }

private:
    Dispatcher*                           dispatcher_;
    OrderBookManager*                     obManager_;
    std::shared_ptr<FlowObserverRegistry> observers_;
};

} // namespace krn::feed
