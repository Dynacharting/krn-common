#pragma once

#include "krn/feed/IFeedAdapter.hpp"
#include "krn/book/OrderBook.hpp"   // Side

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace krn::feed {

/// Feed adapter for the NASDAQ ITCH JSON protocol.
///
/// Translates ITCH message types (stock_directory, add_order, order_executed,
/// order_cancel, order_delete, order_replace, trade, system_event) into
/// canonical FeedEvents.
///
/// Maintains internal state for:
///   - stockLocate → ticker mapping (ITCH pads to 8 chars)
///   - Live order tracking (needed to resolve partial fills / cancels)
///   - The session date, to turn ITCH's ns-since-midnight `timestamp` into
///     epoch ns (`tsNs` / `timestampNs` on the emitted events)
///
/// Session date: the UTC midnight of the wall clock at the first message that
/// carries a valid `timestamp` (the midnight that puts that message closest to
/// the wall clock, so a 23:59:59.9 message received at 00:00:00.1 stays on the
/// previous day). It is then carried across the run and advanced by one day
/// whenever the since-midnight value wraps (drops by more than 12 h). A missing
/// or out-of-range (>= 24 h) `timestamp` yields 0 = unknown and leaves the
/// session date untouched.
class ItchAdapter : public IFeedAdapter {
public:
    /// Wall clock in ns since the Unix epoch, read once, to anchor the session date.
    using WallClockNs = std::function<uint64_t()>;

    struct Options {
        /// Empty = std::chrono::system_clock. Inject a fixed value in tests.
        WallClockNs wallClockNs;
        /// Hard cap on tracked orders (M4). Defaults to kMaxTrackedOrders.
        std::size_t maxTrackedOrders = 0;
    };

    ItchAdapter();
    explicit ItchAdapter(Options opts);

    std::vector<FeedEvent> translate(const std::string& rawMessage) override;

    static constexpr uint64_t kNsPerDay = 86'400ULL * 1'000'000'000ULL;

private:
    // ---- ITCH message handlers (each appends to `out`) ----
    void routeStockDirectory(const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeAddOrder      (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderExecuted (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderCancel   (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderDelete   (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderReplace  (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeTrade         (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeSystemEvent   (const rapidjson::Value& doc);

    // ---- Helpers ----
    static double parsePrice(const rapidjson::Value& v);

    /// Convert the message's ITCH `timestamp` (ns since midnight) to epoch ns,
    /// anchoring / advancing the session date. 0 when absent or out of range.
    uint64_t epochNsFor(const rapidjson::Value& doc);

    /// The message's `matchNumber`, when present as an unsigned integer.
    static std::optional<uint64_t> parseMatchNumber(const rapidjson::Value& doc);

    /// Build a TickEvent with lastPrice + volume fields for TA computation.
    static TickEvent makeTradeTickEvent(const std::string& symbol,
                                        double price, uint64_t size);

    // ---- ITCH protocol state ----

    /// stockLocate → ticker
    std::unordered_map<int, std::string> locateToSymbol_;

    /// Live order tracking for partial fill / cancel resolution.
    struct OrderState {
        std::string symbol;
        uint64_t    remainingShares;
        Side        side;
        double      price;
    };

    /// Hard cap on tracked orders (M4 / ENC-797). Bounds memory against a feed
    /// that adds orders without ever deleting them; when exceeded the oldest
    /// map bucket entry is evicted on the next add. ~1M entries keeps the map
    /// well under ~100 MB while comfortably covering any sane live session.
    static constexpr std::size_t kMaxTrackedOrders = 1'000'000;
    std::unordered_map<uint64_t, OrderState> orders_;

    // ---- Session clock (ITCH timestamp → epoch ns) ----
    WallClockNs  wallClockNs_;
    std::size_t  maxTrackedOrders_;
    bool         haveSessionDate_ = false;
    uint64_t     sessionMidnightNs_ = 0;   // epoch ns of the current session's midnight
    uint64_t     lastSinceMidnightNs_ = 0; // last valid since-midnight value seen
};

} // namespace krn::feed
