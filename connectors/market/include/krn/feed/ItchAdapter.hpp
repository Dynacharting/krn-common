#pragma once

#include "krn/feed/IFeedAdapter.hpp"
#include "krn/book/OrderBook.hpp"   // Side

#include <chrono>
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
/// Session date. ITCH's `timestamp` is ns since midnight in the *session
/// time zone* (`Options::sessionTz`): NASDAQ counts from midnight US Eastern,
/// feed-simulator from midnight UTC, so the default is "UTC". The adapter
/// carries a session midnight across messages and re-anchors it against the
/// wall clock (read once per timestamped message) whenever
///   - it has no session date yet (first valid timestamp, or after
///     onConnected()), or
///   - the carried date puts the event more than 12 h from the wall clock: a
///     midnight wrap, an overnight close-to-open gap (16:00 Mon -> 09:30 Tue),
///     a multi-day gap in either direction (10:00 Mon -> 11:00 Wed).
/// While the carried date is within 12 h of now it is the nearest candidate
/// already, so reordering (a small backward step) and feed lag never move it.
/// Re-anchoring picks, among the session-tz dates d-1, d, d+1 around the wall
/// clock's local date d, the one that puts the event nearest to now. So a
/// 23:59:59.9 message received at 00:00:00.1 stays on the previous day, and a
/// 09:30 message after a 16:00 close lands on the next morning. The premise is
/// a live feed whose events are within 12 h of the receiving wall clock; a
/// historical ITCH replay at a different time of day is not dated correctly
/// (replay is out of scope, SPEC 2026-11-17-order-flow-pipelines §0). A missing
/// or out-of-range (>= 24 h) `timestamp` yields 0 = unknown and leaves the
/// session state untouched. Midnight in a DST zone is always a valid local
/// time (US transitions are at 02:00), and the 23 h / 25 h transition Sundays
/// carry no NASDAQ session.
class ItchAdapter : public IFeedAdapter {
public:
    /// Wall clock in ns since the Unix epoch, read once per timestamped
    /// message, to anchor / re-anchor the session date.
    using WallClockNs = std::function<uint64_t()>;

    struct Options {
        /// Empty = std::chrono::system_clock. Inject a fixed value in tests.
        WallClockNs wallClockNs;
        /// The zone whose midnight ITCH's `timestamp` counts from: "UTC"
        /// (default; feed-simulator) or an IANA name resolved through the
        /// C++20 tzdb, e.g. "America/New_York" for real NASDAQ (DST-correct).
        /// An unknown name throws std::invalid_argument from the constructor;
        /// check it first with isValidSessionTz(). Config key: itch.sessionTz.
        std::string sessionTz = "UTC";
        /// Hard cap on tracked orders (M4). Defaults to kMaxTrackedOrders.
        std::size_t maxTrackedOrders = 0;
    };

    ItchAdapter();
    explicit ItchAdapter(Options opts);

    std::vector<FeedEvent> translate(const std::string& rawMessage) override;

    /// A (re)connected stream may resume after any gap: forget the session
    /// date so the next timestamp re-anchors against the wall clock. Order
    /// tracking is kept (the book is not reset on reconnect either).
    void onConnected() override;

    /// True for "UTC" and for any name the tzdb resolves.
    static bool isValidSessionTz(const std::string& name);

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
    /// anchoring / re-anchoring the session date. 0 when absent or out of range.
    uint64_t epochNsFor(const rapidjson::Value& doc);

    /// Epoch ns of midnight of local day `day` (days since 1970-01-01) in the
    /// session zone, and the session-zone local day containing `epochNs`.
    int64_t midnightEpochNs(int64_t day) const;
    int64_t localDayOf(int64_t epochNs) const;

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
    const std::chrono::time_zone* tz_ = nullptr;   // nullptr = UTC
    std::size_t  maxTrackedOrders_;
    bool         haveSessionDate_ = false;
    uint64_t     sessionMidnightNs_ = 0;   // epoch ns of the current session's midnight
    uint64_t     msgTsNs_ = 0;             // epoch ns of the message being translated
};

} // namespace krn::feed
