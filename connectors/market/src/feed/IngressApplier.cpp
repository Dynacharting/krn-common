#include "krn/feed/IngressApplier.hpp"

#include "krn/Dispatcher.hpp"
#include "krn/Event.hpp"
#include "krn/book/OrderBookManager.hpp"
#include "krn/util/Logger.hpp"
#include "krn/util/Metrics.hpp"

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace krn::feed {

// ---------------------------------------------------------------------------
// FlowObserverRegistry — copy-on-write list, so a frame in flight iterates a
// stable snapshot and add/remove never block on (or invalidate) a notify.
// ---------------------------------------------------------------------------
void FlowObserverRegistry::add(std::shared_ptr<IFlowObserver> obs) {
    if (!obs) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto next = std::make_shared<List>(*list_);
    next->push_back(std::move(obs));
    list_ = std::move(next);
}

void FlowObserverRegistry::remove(const IFlowObserver* obs) {
    std::lock_guard<std::mutex> lk(mu_);
    auto next = std::make_shared<List>(*list_);
    next->erase(std::remove_if(next->begin(), next->end(),
                               [obs](const auto& p) { return p.get() == obs; }),
                next->end());
    list_ = std::move(next);
}

void FlowObserverRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    list_ = std::make_shared<const List>();
}

std::shared_ptr<const FlowObserverRegistry::List> FlowObserverRegistry::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return list_;
}

bool FlowObserverRegistry::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return list_->empty();
}

// ---------------------------------------------------------------------------
// IngressApplier
// ---------------------------------------------------------------------------
IngressApplier::IngressApplier(Dispatcher* dispatcher,
                               OrderBookManager* obManager,
                               std::shared_ptr<FlowObserverRegistry> observers)
    : dispatcher_(dispatcher), obManager_(obManager), observers_(std::move(observers)) {}

IngressApplier::FrameResult IngressApplier::apply(std::vector<FeedEvent>& frame) {
    const bool drop = seqGate(frame);
    return applyGated(frame, drop);
}

// M7: per-message gap detection. A single wire frame may expand into several
// FeedEvents (e.g. replace -> delete+add), so evaluate the frame's sequence
// exactly once — on the first order-book event, which carries the resolved
// symbol plus the optional wire seq. If onSeq reports a gap/stale state, drop
// this frame's book mutations (subsequent frames are auto-gated by the
// manager until a snapshot resyncs). Conservative: events with no seq leave
// dropBook=false, so behavior is unchanged for sequence-less feeds (ITCH).
bool IngressApplier::seqGate(const std::vector<FeedEvent>& frame) {
    if (!obManager_) return false;
    bool dropBook = false;
    for (const auto& evt : frame) {
        const bool decided = std::visit([&](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ObAddEvent>    ||
                          std::is_same_v<T, ObUpdateEvent> ||
                          std::is_same_v<T, ObDeleteEvent> ||
                          std::is_same_v<T, ObTradeEvent>) {
                if (e.seq) { dropBook = !obManager_->onSeq(e.symbol, *e.seq); return true; }
            }
            return false;
        }, evt);
        if (decided) break;
    }
    return dropBook;
}

namespace {

using ObserverList = FlowObserverRegistry::List;

// An observer's exception must not abort the rest of the frame: the book has
// already taken this event, and skipping the frame's remaining events would
// leave the book half-applied.
void notifyAll(const ObserverList& obs, const FlowObservation& o) {
    for (const auto& ob : obs) {
        try {
            ob->onFlowEvent(o);
        } catch (const std::exception& ex) {
            KRN_METRIC_HIT("feed.flow_observer_error");
            krn::util::logger().log(krn::util::LogLevel::Warn,
                                    "feed.flow_observer.exception", {{"err", ex.what()}});
        } catch (...) {
            KRN_METRIC_HIT("feed.flow_observer_error");
            krn::util::logger().log(krn::util::LogLevel::Warn,
                                    "feed.flow_observer.exception", {{"err", "unknown"}});
        }
    }
}

} // namespace

IngressApplier::FrameResult IngressApplier::applyGated(std::vector<FeedEvent>& frame,
                                                       bool dropBook) {
    FrameResult r;
    r.dropBook = dropBook;

    // One snapshot per frame; with no observer the TobChange out-param stays
    // null and the manager does no extra work.
    std::shared_ptr<const ObserverList> obs;
    if (observers_) {
        auto snap = observers_->snapshot();
        if (snap && !snap->empty()) obs = std::move(snap);
    }

    for (auto& evt : frame) {
        TobChange tc;
        TobChange* tcp = obs ? &tc : nullptr;

        // Observe after the mutator has returned (its locks released).
        auto applied = [&](bool unattributed) {
            ++r.applied;
            if (obs) {
                notifyAll(*obs, FlowObservation{evt, tc, unattributed});
                ++r.observed;
            }
        };

        std::visit([&](auto& e) {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, TickEvent>) {
                if (dispatcher_) {
                    Event tick;
                    tick.symbol  = std::move(e.symbol);
                    tick.payload = std::move(e.payload);
                    dispatcher_->onTick(tick);
                }
            }
            else if constexpr (std::is_same_v<T, ObAddEvent>) {
                if (obManager_ && !dropBook &&
                    obManager_->onAdd(e.symbol, e.orderId, e.side, e.price, e.size, e.priority,
                                      FeedScope{}, /*idMissing=*/false, tcp)) {
                    applied(false);
                }
            }
            else if constexpr (std::is_same_v<T, ObUpdateEvent>) {
                if (obManager_ && !dropBook &&
                    obManager_->onUpdate(e.symbol, e.orderId, FeedScope{}, e.newPrice, e.newSize,
                                         /*synthetic=*/false, tcp)) {
                    applied(false);
                }
            }
            else if constexpr (std::is_same_v<T, ObDeleteEvent>) {
                if (obManager_ && !dropBook &&
                    obManager_->onDelete(e.symbol, e.orderId, FeedScope{}, /*synthetic=*/false, tcp)) {
                    applied(false);
                }
            }
            else if constexpr (std::is_same_v<T, ObTradeEvent>) {
                if (!obManager_ || dropBook) return;
                if (!e.bookNeutral) {
                    // Consumes resting liquidity through OrderBook::applyTrade,
                    // with no per-order event: an unattributed execution (D3).
                    if (obManager_->onTrade(e.symbol, e.price, e.size, e.aggressor, tcp))
                        applied(true);
                    return;
                }
                // C1/M1: a print-only (bookNeutral) trade must NOT consume the
                // book — executions already mutate it via explicit
                // add/update/delete, and ITCH `trade` prints are hidden
                // executions. Nothing is applied; it is still observed (hidden
                // absorption, sweep grouping), unless the symbol is stale.
                if (obs && !obManager_->isStale(e.symbol)) {
                    tc.pre  = obManager_->topOfBook(e.symbol);
                    tc.post = tc.pre;
                    notifyAll(*obs, FlowObservation{evt, tc, false});
                    ++r.observed;
                }
            }
            else if constexpr (std::is_same_v<T, ObTickSizeEvent>) {
                if (obManager_) obManager_->setTickSize(e.symbol, e.tickSize);
            }
            else if constexpr (std::is_same_v<T, ObResetEvent>) {
                if (obManager_) {
                    obManager_->onReset(e.symbol, e.epoch);
                    if (obs) {
                        for (const auto& ob : *obs) {
                            try { ob->onBookReset(e); }
                            catch (...) { KRN_METRIC_HIT("feed.flow_observer_error"); }
                        }
                    }
                }
            }
        }, evt);
    }
    return r;
}

} // namespace krn::feed
