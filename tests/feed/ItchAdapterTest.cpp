#include "krn/feed/ItchAdapter.hpp"
#include "krn/feed/FeedEvent.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>
#include <vector>

using namespace krn;
using namespace krn::feed;

// ---------------------------------------------------------------------------
// Helper — build a JSON string from key/value pairs
// ---------------------------------------------------------------------------
namespace {

class JsonBuilder {
public:
    JsonBuilder() { w_.StartObject(); }

    JsonBuilder& str(const char* key, const char* val) {
        w_.Key(key); w_.String(val); return *this;
    }
    JsonBuilder& num(const char* key, double val) {
        w_.Key(key); w_.Double(val); return *this;
    }
    JsonBuilder& uint(const char* key, uint64_t val) {
        w_.Key(key); w_.Uint64(val); return *this;
    }
    JsonBuilder& integer(const char* key, int val) {
        w_.Key(key); w_.Int(val); return *this;
    }

    std::string build() {
        w_.EndObject();
        return sb_.GetString();
    }

private:
    rapidjson::StringBuffer sb_;
    rapidjson::Writer<rapidjson::StringBuffer> w_{sb_};
};

template <typename T>
bool hasEvent(const std::vector<FeedEvent>& events) {
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) return true;
    }
    return false;
}

template <typename T>
const T& getEvent(const std::vector<FeedEvent>& events, size_t nth = 0) {
    size_t count = 0;
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) {
            if (count == nth) return std::get<T>(e);
            ++count;
        }
    }
    throw std::runtime_error("Event not found");
}

template <typename T>
size_t countEvents(const std::vector<FeedEvent>& events) {
    size_t n = 0;
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) ++n;
    }
    return n;
}

} // anonymous namespace

// ===========================================================================
// stock_directory
// ===========================================================================

TEST(ItchAdapterTest, StockDirectoryRegistersSymbolAndTickSize) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "stock_directory")
        .integer("stockLocate", 42)
        .str("stock", "AAPL    ")   // ITCH pads to 8 chars
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(hasEvent<ObTickSizeEvent>(events));

    const auto& ts = getEvent<ObTickSizeEvent>(events);
    EXPECT_EQ(ts.symbol, "AAPL");          // trailing spaces trimmed
    EXPECT_DOUBLE_EQ(ts.tickSize, 0.01);   // NASDAQ default
}

TEST(ItchAdapterTest, StockDirectoryMissingFieldsProducesNoEvents) {
    ItchAdapter adapter;

    // Missing "stock" field
    auto msg = JsonBuilder()
        .str("type", "stock_directory")
        .integer("stockLocate", 1)
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// add_order / add_order_mpid
// ===========================================================================

TEST(ItchAdapterTest, AddOrderProducesObAddEvent) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "MSFT    ")
        .uint("orderRef", 100)
        .str("side", "B")
        .uint("shares", 500)
        .num("price", 350.25)
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(events.size(), 1u);
    const auto& add = getEvent<ObAddEvent>(events);
    EXPECT_EQ(add.symbol, "MSFT");
    EXPECT_EQ(add.orderId, 100u);
    EXPECT_EQ(add.side, Side::Bid);
    EXPECT_DOUBLE_EQ(add.price, 350.25);
    EXPECT_EQ(add.size, 500u);
    EXPECT_EQ(add.priority, 0u);
}

TEST(ItchAdapterTest, AddOrderMpidRoutesSameAsAddOrder) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "add_order_mpid")
        .str("stock", "TSLA")
        .uint("orderRef", 200)
        .str("side", "S")
        .uint("shares", 100)
        .num("price", 180.0)
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(events.size(), 1u);
    const auto& add = getEvent<ObAddEvent>(events);
    EXPECT_EQ(add.side, Side::Ask);
}

TEST(ItchAdapterTest, AddOrderMissingFieldsProducesNoEvents) {
    ItchAdapter adapter;

    // Missing "shares"
    auto msg = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "X")
        .uint("orderRef", 1)
        .str("side", "B")
        .num("price", 10.0)
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// order_executed — full fill
// ===========================================================================

TEST(ItchAdapterTest, OrderExecutedFullFillDeletesAndEmitsTradeTick) {
    ItchAdapter adapter;

    // First add the order
    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "AAPL")
        .uint("orderRef", 10)
        .str("side", "B")
        .uint("shares", 100)
        .num("price", 150.0)
        .build();
    adapter.translate(add);

    // Execute the full amount
    auto exec = JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 10)
        .uint("shares", 100)
        .build();

    auto events = adapter.translate(exec);

    // Should produce: ObDeleteEvent + ObTradeEvent + TickEvent
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObTradeEvent>(events), 1u);
    EXPECT_EQ(countEvents<TickEvent>(events), 1u);

    const auto& del = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(del.symbol, "AAPL");
    EXPECT_EQ(del.orderId, 10u);

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_DOUBLE_EQ(trade.price, 150.0);
    EXPECT_EQ(trade.size, 100u);
    EXPECT_EQ(trade.aggressor, Aggressor::Sell);  // Resting Bid → incoming Sell aggressor

    const auto& tick = getEvent<TickEvent>(events);
    EXPECT_EQ(tick.symbol, "AAPL");
    ASSERT_TRUE(tick.payload);
    EXPECT_TRUE(tick.payload->HasMember("lastPrice"));
    EXPECT_DOUBLE_EQ((*tick.payload)["lastPrice"].GetDouble(), 150.0);
}

// ===========================================================================
// order_executed — partial fill
// ===========================================================================

TEST(ItchAdapterTest, OrderExecutedPartialFillUpdatesAndEmitsTradeTick) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "GOOG")
        .uint("orderRef", 20)
        .str("side", "S")
        .uint("shares", 200)
        .num("price", 2800.0)
        .build();
    adapter.translate(add);

    // Partial fill: 50 of 200
    auto exec = JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 20)
        .uint("shares", 50)
        .build();

    auto events = adapter.translate(exec);

    // Should produce: ObUpdateEvent + ObTradeEvent + TickEvent (no delete)
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 0u);
    EXPECT_EQ(countEvents<ObUpdateEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObTradeEvent>(events), 1u);
    EXPECT_EQ(countEvents<TickEvent>(events), 1u);

    const auto& upd = getEvent<ObUpdateEvent>(events);
    EXPECT_EQ(upd.symbol, "GOOG");
    EXPECT_EQ(upd.orderId, 20u);
    ASSERT_TRUE(upd.newSize.has_value());
    EXPECT_EQ(*upd.newSize, 150u);   // 200 - 50
    EXPECT_FALSE(upd.newPrice.has_value());

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(trade.aggressor, Aggressor::Buy);  // Resting Ask → incoming Buy aggressor
}

TEST(ItchAdapterTest, OrderExecutedUnknownOrderProducesNoEvents) {
    ItchAdapter adapter;

    auto exec = JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 999)
        .uint("shares", 10)
        .build();

    EXPECT_TRUE(adapter.translate(exec).empty());
}

// ===========================================================================
// order_cancel — partial and full
// ===========================================================================

TEST(ItchAdapterTest, OrderCancelPartialReducesSize) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "NFLX")
        .uint("orderRef", 30)
        .str("side", "B")
        .uint("shares", 300)
        .num("price", 450.0)
        .build();
    adapter.translate(add);

    auto cancel = JsonBuilder()
        .str("type", "order_cancel")
        .uint("orderRef", 30)
        .uint("shares", 100)
        .build();

    auto events = adapter.translate(cancel);

    ASSERT_EQ(countEvents<ObUpdateEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 0u);

    const auto& upd = getEvent<ObUpdateEvent>(events);
    EXPECT_EQ(upd.symbol, "NFLX");
    ASSERT_TRUE(upd.newSize.has_value());
    EXPECT_EQ(*upd.newSize, 200u);  // 300 - 100
}

TEST(ItchAdapterTest, OrderCancelFullDeletesOrder) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "AMZN")
        .uint("orderRef", 40)
        .str("side", "S")
        .uint("shares", 50)
        .num("price", 3400.0)
        .build();
    adapter.translate(add);

    // Cancel all remaining
    auto cancel = JsonBuilder()
        .str("type", "order_cancel")
        .uint("orderRef", 40)
        .uint("shares", 50)
        .build();

    auto events = adapter.translate(cancel);

    ASSERT_EQ(countEvents<ObDeleteEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObUpdateEvent>(events), 0u);
}

// ===========================================================================
// order_delete
// ===========================================================================

TEST(ItchAdapterTest, OrderDeleteRemovesOrder) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "META")
        .uint("orderRef", 50)
        .str("side", "B")
        .uint("shares", 100)
        .num("price", 300.0)
        .build();
    adapter.translate(add);

    auto del = JsonBuilder()
        .str("type", "order_delete")
        .uint("orderRef", 50)
        .build();

    auto events = adapter.translate(del);

    ASSERT_EQ(events.size(), 1u);
    const auto& d = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(d.symbol, "META");
    EXPECT_EQ(d.orderId, 50u);

    // Deleting again should produce nothing (order already gone)
    EXPECT_TRUE(adapter.translate(del).empty());
}

// ===========================================================================
// order_replace
// ===========================================================================

TEST(ItchAdapterTest, OrderReplaceDeletesOldAndAddsNew) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "NVDA")
        .uint("orderRef", 60)
        .str("side", "B")
        .uint("shares", 100)
        .num("price", 500.0)
        .build();
    adapter.translate(add);

    auto replace = JsonBuilder()
        .str("type", "order_replace")
        .uint("origOrderRef", 60)
        .uint("orderRef", 61)
        .uint("shares", 200)
        .num("price", 505.0)
        .build();

    auto events = adapter.translate(replace);

    ASSERT_EQ(countEvents<ObDeleteEvent>(events), 1u);
    ASSERT_EQ(countEvents<ObAddEvent>(events), 1u);

    const auto& del = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(del.orderId, 60u);
    EXPECT_EQ(del.symbol, "NVDA");

    const auto& newAdd = getEvent<ObAddEvent>(events);
    EXPECT_EQ(newAdd.orderId, 61u);
    EXPECT_EQ(newAdd.symbol, "NVDA");
    EXPECT_EQ(newAdd.side, Side::Bid);   // preserves original side
    EXPECT_DOUBLE_EQ(newAdd.price, 505.0);
    EXPECT_EQ(newAdd.size, 200u);
}

TEST(ItchAdapterTest, OrderReplaceUnknownOrigProducesNoEvents) {
    ItchAdapter adapter;

    auto replace = JsonBuilder()
        .str("type", "order_replace")
        .uint("origOrderRef", 999)
        .uint("orderRef", 1000)
        .uint("shares", 100)
        .num("price", 10.0)
        .build();

    EXPECT_TRUE(adapter.translate(replace).empty());
}

// ===========================================================================
// trade
// ===========================================================================

TEST(ItchAdapterTest, TradeEmitsObTradeAndTickEvent) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "trade")
        .str("stock", "INTC    ")
        .num("price", 45.50)
        .uint("shares", 1000)
        .str("side", "B")
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);
    ASSERT_EQ(countEvents<TickEvent>(events), 1u);

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(trade.symbol, "INTC");
    EXPECT_DOUBLE_EQ(trade.price, 45.50);
    EXPECT_EQ(trade.size, 1000u);
    EXPECT_EQ(trade.aggressor, Aggressor::Buy);

    const auto& tick = getEvent<TickEvent>(events);
    EXPECT_EQ(tick.symbol, "INTC");
    ASSERT_TRUE(tick.payload);
    EXPECT_DOUBLE_EQ((*tick.payload)["lastPrice"].GetDouble(), 45.50);
    EXPECT_DOUBLE_EQ((*tick.payload)["volume"].GetDouble(), 1000.0);
}

TEST(ItchAdapterTest, TradeNoSideDefaultsToUnknownAggressor) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "trade")
        .str("stock", "AMD")
        .num("price", 120.0)
        .uint("shares", 50)
        .build();

    auto events = adapter.translate(msg);
    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(trade.aggressor, Aggressor::Unknown);
}

TEST(ItchAdapterTest, TradePriceAsStringParsesCorrectly) {
    ItchAdapter adapter;

    // ITCH sometimes sends price as string "185.2500"
    // Build this manually since JsonBuilder uses typed methods
    std::string msg = R"({"type":"trade","stock":"TEST","price":"185.2500","shares":10})";

    auto events = adapter.translate(msg);
    ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_DOUBLE_EQ(trade.price, 185.25);
}

// ===========================================================================
// system_event
// ===========================================================================

TEST(ItchAdapterTest, SystemEventProducesNoFeedEvents) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "system_event")
        .str("eventCode", "S")
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// Invalid / malformed messages
// ===========================================================================

TEST(ItchAdapterTest, InvalidJsonProducesNoEvents) {
    ItchAdapter adapter;
    EXPECT_TRUE(adapter.translate("not json at all").empty());
    EXPECT_TRUE(adapter.translate("{broken").empty());
    EXPECT_TRUE(adapter.translate("").empty());
}

TEST(ItchAdapterTest, MissingTypeFieldProducesNoEvents) {
    ItchAdapter adapter;
    EXPECT_TRUE(adapter.translate(R"({"stock":"AAPL"})").empty());
}

TEST(ItchAdapterTest, UnknownTypeProducesNoEvents) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "unknown_message_type")
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// Stateful: order lifecycle across multiple messages
// ===========================================================================

TEST(ItchAdapterTest, FullOrderLifecycle) {
    ItchAdapter adapter;

    // 1) Register symbol
    adapter.translate(JsonBuilder()
        .str("type", "stock_directory")
        .integer("stockLocate", 1)
        .str("stock", "LIFE    ")
        .build());

    // 2) Add order
    auto addEvents = adapter.translate(JsonBuilder()
        .str("type", "add_order")
        .str("stock", "LIFE")
        .uint("orderRef", 1)
        .str("side", "B")
        .uint("shares", 500)
        .num("price", 100.0)
        .build());
    ASSERT_EQ(countEvents<ObAddEvent>(addEvents), 1u);

    // 3) Partial fill: 200 of 500
    auto execEvents = adapter.translate(JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 1)
        .uint("shares", 200)
        .build());
    ASSERT_EQ(countEvents<ObUpdateEvent>(execEvents), 1u);
    EXPECT_EQ(getEvent<ObUpdateEvent>(execEvents).newSize.value(), 300u);

    // 4) Partial cancel: 100 of remaining 300
    auto cancelEvents = adapter.translate(JsonBuilder()
        .str("type", "order_cancel")
        .uint("orderRef", 1)
        .uint("shares", 100)
        .build());
    ASSERT_EQ(countEvents<ObUpdateEvent>(cancelEvents), 1u);
    EXPECT_EQ(getEvent<ObUpdateEvent>(cancelEvents).newSize.value(), 200u);

    // 5) Replace: remaining 200 → new order 2 at different price
    auto replaceEvents = adapter.translate(JsonBuilder()
        .str("type", "order_replace")
        .uint("origOrderRef", 1)
        .uint("orderRef", 2)
        .uint("shares", 200)
        .num("price", 101.0)
        .build());
    ASSERT_EQ(countEvents<ObDeleteEvent>(replaceEvents), 1u);
    ASSERT_EQ(countEvents<ObAddEvent>(replaceEvents), 1u);
    EXPECT_DOUBLE_EQ(getEvent<ObAddEvent>(replaceEvents).price, 101.0);

    // 6) Full fill of replacement
    auto fillEvents = adapter.translate(JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 2)
        .uint("shares", 200)
        .build());
    ASSERT_EQ(countEvents<ObDeleteEvent>(fillEvents), 1u);
    ASSERT_EQ(countEvents<ObTradeEvent>(fillEvents), 1u);
    ASSERT_EQ(countEvents<TickEvent>(fillEvents), 1u);

    // 7) Accessing deleted order produces nothing
    EXPECT_TRUE(adapter.translate(JsonBuilder()
        .str("type", "order_delete")
        .uint("orderRef", 2)
        .build()).empty());
}

// ===========================================================================
// Order-flow fields (ENC-1644, SPEC 2026-11-17-order-flow-pipelines D2):
// one case per row of the D2 ITCH table, plus the epoch conversion.
// ===========================================================================

namespace {

constexpr uint64_t kSec      = 1'000'000'000ULL;
constexpr uint64_t kDay      = ItchAdapter::kNsPerDay;
constexpr uint64_t kMidnight = 1'794'873'600ULL * kSec;   // 2026-11-17T00:00:00Z

// An adapter whose wall clock reads `nowNs` (default 2026-11-17 14:30:00Z).
ItchAdapter clockedAdapter(uint64_t nowNs = kMidnight + 52'200 * kSec) {
    ItchAdapter::Options o;
    o.wallClockNs = [nowNs] { return nowNs; };
    return ItchAdapter(std::move(o));
}

std::string addMsg(uint64_t ref, const char* side, uint64_t shares, double px,
                   uint64_t ts) {
    return JsonBuilder().str("type", "add_order").uint("timestamp", ts)
        .str("stock", "FLOW").uint("orderRef", ref).str("side", side)
        .uint("shares", shares).num("price", px).build();
}

constexpr uint64_t kT0 = 34'200 * kSec;   // 09:30:00 since midnight

} // anonymous namespace

TEST(ItchAdapterTest, AddCarriesEpochTsNs) {
    auto adapter = clockedAdapter();
    auto events = adapter.translate(addMsg(1, "B", 100, 10.0, kT0 + 17));

    ASSERT_EQ(events.size(), 1u);
    const auto& add = getEvent<ObAddEvent>(events);
    EXPECT_EQ(add.tsNs, kMidnight + kT0 + 17);
    EXPECT_FALSE(add.replacesOrderId.has_value());
}

TEST(ItchAdapterTest, AddMpidCarriesEpochTsNs) {
    auto adapter = clockedAdapter();
    auto events = adapter.translate(JsonBuilder().str("type", "add_order_mpid")
        .uint("timestamp", kT0).str("stock", "FLOW").uint("orderRef", 1)
        .str("side", "S").uint("shares", 10).num("price", 10.0).build());
    EXPECT_EQ(getEvent<ObAddEvent>(events).tsNs, kMidnight + kT0);
}

TEST(ItchAdapterTest, ExecutedPartialIsExecutionWithMatchIdentity) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(7, "B", 100, 10.0, kT0));

    auto events = adapter.translate(JsonBuilder().str("type", "order_executed")
        .uint("timestamp", kT0 + 5).uint("orderRef", 7).uint("shares", 40)
        .uint("matchNumber", 9001).build());

    // Book mutation first, then the print (D2 order).
    ASSERT_GE(events.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<ObUpdateEvent>(events[0]));
    ASSERT_TRUE(std::holds_alternative<ObTradeEvent>(events[1]));

    const auto& upd = std::get<ObUpdateEvent>(events[0]);
    EXPECT_EQ(upd.cause, MutationCause::Execution);
    EXPECT_EQ(upd.tsNs, kMidnight + kT0 + 5);
    EXPECT_EQ(upd.newSize.value(), 60u);

    const auto& tr = std::get<ObTradeEvent>(events[1]);
    EXPECT_TRUE(tr.bookNeutral);
    EXPECT_EQ(tr.timestampNs, kMidnight + kT0 + 5);
    ASSERT_TRUE(tr.restingOrderId.has_value());
    EXPECT_EQ(*tr.restingOrderId, 7u);
    ASSERT_TRUE(tr.matchNumber.has_value());
    EXPECT_EQ(*tr.matchNumber, 9001u);
}

TEST(ItchAdapterTest, ExecutedFullIsExecutionDelete) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(8, "S", 100, 10.0, kT0));

    auto events = adapter.translate(JsonBuilder().str("type", "order_executed")
        .uint("timestamp", kT0 + 6).uint("orderRef", 8).uint("shares", 100)
        .uint("matchNumber", 9002).build());

    ASSERT_GE(events.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<ObDeleteEvent>(events[0]));
    const auto& del = std::get<ObDeleteEvent>(events[0]);
    EXPECT_EQ(del.cause, MutationCause::Execution);
    EXPECT_EQ(del.tsNs, kMidnight + kT0 + 6);

    const auto& tr = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(tr.restingOrderId.value(), 8u);
    EXPECT_EQ(tr.matchNumber.value(), 9002u);
    EXPECT_EQ(tr.timestampNs, kMidnight + kT0 + 6);
    EXPECT_TRUE(tr.bookNeutral);
}

TEST(ItchAdapterTest, ExecutedWithoutMatchNumberLeavesItEmpty) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(9, "B", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder().str("type", "order_executed")
        .uint("orderRef", 9).uint("shares", 1).build());
    const auto& tr = getEvent<ObTradeEvent>(events);
    EXPECT_FALSE(tr.matchNumber.has_value());
    EXPECT_EQ(tr.restingOrderId.value(), 9u);
    EXPECT_EQ(tr.timestampNs, 0u);                       // no timestamp → unknown
    EXPECT_EQ(getEvent<ObUpdateEvent>(events).tsNs, 0u);
}

TEST(ItchAdapterTest, PartialCancelIsCancelUpdate) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(10, "B", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder().str("type", "order_cancel")
        .uint("timestamp", kT0 + 7).uint("orderRef", 10).uint("shares", 30).build());

    ASSERT_EQ(events.size(), 1u);
    const auto& upd = getEvent<ObUpdateEvent>(events);
    EXPECT_EQ(upd.cause, MutationCause::Cancel);
    EXPECT_EQ(upd.tsNs, kMidnight + kT0 + 7);
    EXPECT_EQ(upd.newSize.value(), 70u);
}

TEST(ItchAdapterTest, CancelOfRemainderIsCancelDelete) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(11, "S", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder().str("type", "order_cancel")
        .uint("timestamp", kT0 + 8).uint("orderRef", 11).uint("shares", 100).build());

    ASSERT_EQ(events.size(), 1u);
    const auto& del = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(del.cause, MutationCause::Cancel);
    EXPECT_EQ(del.tsNs, kMidnight + kT0 + 8);
}

TEST(ItchAdapterTest, DeleteIsCancelDelete) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(12, "B", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder().str("type", "order_delete")
        .uint("timestamp", kT0 + 9).uint("orderRef", 12).build());

    ASSERT_EQ(events.size(), 1u);
    const auto& del = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(del.cause, MutationCause::Cancel);
    EXPECT_EQ(del.tsNs, kMidnight + kT0 + 9);
}

TEST(ItchAdapterTest, ReplaceIsReplaceDeletePlusAddCarryingOrigId) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(13, "B", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder().str("type", "order_replace")
        .uint("timestamp", kT0 + 10).uint("origOrderRef", 13).uint("orderRef", 14)
        .uint("shares", 50).num("price", 10.01).build());

    ASSERT_EQ(events.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<ObDeleteEvent>(events[0]));
    ASSERT_TRUE(std::holds_alternative<ObAddEvent>(events[1]));

    const auto& del = std::get<ObDeleteEvent>(events[0]);
    EXPECT_EQ(del.orderId, 13u);
    EXPECT_EQ(del.cause, MutationCause::Replace);
    EXPECT_EQ(del.tsNs, kMidnight + kT0 + 10);

    const auto& add = std::get<ObAddEvent>(events[1]);
    EXPECT_EQ(add.orderId, 14u);
    ASSERT_TRUE(add.replacesOrderId.has_value());
    EXPECT_EQ(*add.replacesOrderId, 13u);
    EXPECT_EQ(add.tsNs, kMidnight + kT0 + 10);
}

TEST(ItchAdapterTest, NonDisplayedPrintIsBookNeutralWithMatchNumber) {
    auto adapter = clockedAdapter();
    auto events = adapter.translate(JsonBuilder().str("type", "trade")
        .uint("timestamp", kT0 + 11).str("stock", "FLOW").uint("orderRef", 0)
        .str("side", "B").uint("shares", 25).num("price", 10.0)
        .uint("matchNumber", 9003).build());

    // P only: a print, no book mutation.
    EXPECT_EQ(countEvents<ObAddEvent>(events), 0u);
    EXPECT_EQ(countEvents<ObUpdateEvent>(events), 0u);
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 0u);
    ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);

    const auto& tr = getEvent<ObTradeEvent>(events);
    EXPECT_TRUE(tr.bookNeutral);
    EXPECT_EQ(tr.matchNumber.value(), 9003u);
    EXPECT_EQ(tr.timestampNs, kMidnight + kT0 + 11);
    EXPECT_FALSE(tr.restingOrderId.has_value());
}

TEST(ItchAdapterTest, ExecutedWithPriceStaysUnrouted) {
    // ITCH C is out of V1 (SPEC D2): it emits nothing, even for a known order.
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(15, "B", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder()
        .str("type", "order_executed_with_price").uint("timestamp", kT0 + 12)
        .uint("orderRef", 15).uint("shares", 10).uint("matchNumber", 1)
        .str("printable", "Y").num("executionPrice", 10.0).build());
    EXPECT_TRUE(events.empty());
}

TEST(ItchAdapterTest, EvictionEmitsNothing) {
    // M4: at the tracking cap, an add evicts one tracked order and emits only
    // its own ObAdd — no delete for the evicted order (it stays on the book).
    ItchAdapter::Options o;
    o.wallClockNs      = [] { return kMidnight + 52'200 * kSec; };
    o.maxTrackedOrders = 2;
    ItchAdapter adapter(std::move(o));

    adapter.translate(addMsg(1, "B", 10, 10.0, kT0));
    adapter.translate(addMsg(2, "B", 10, 10.0, kT0));
    auto third = adapter.translate(addMsg(3, "B", 10, 10.0, kT0));
    ASSERT_EQ(third.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ObAddEvent>(third[0]));

    // Exactly one of 1/2 was evicted: it now resolves to nothing.
    size_t resolvable = 0;
    for (uint64_t ref : {1u, 2u}) {
        auto ev = adapter.translate(JsonBuilder().str("type", "order_delete")
            .uint("orderRef", ref).build());
        resolvable += ev.size();
    }
    EXPECT_EQ(resolvable, 1u);
}

// ---- epoch conversion ------------------------------------------------------

TEST(ItchAdapterTest, SessionDateAnchoredOnFirstMessageAndCarried) {
    auto adapter = clockedAdapter();
    // system_event emits nothing but still anchors the session date.
    adapter.translate(JsonBuilder().str("type", "system_event")
        .uint("timestamp", 3'600 * kSec).str("eventCode", "O").build());
    auto events = adapter.translate(addMsg(1, "B", 1, 1.0, kT0));
    EXPECT_EQ(getEvent<ObAddEvent>(events).tsNs, kMidnight + kT0);
}

TEST(ItchAdapterTest, MidnightWrapAdvancesSessionDate) {
    auto adapter = clockedAdapter(kMidnight + kDay - 1 * kSec);   // 23:59:59
    const uint64_t late = kDay - 1'000'000;                       // 23:59:59.999
    auto a = adapter.translate(addMsg(1, "B", 100, 10.0, late));
    EXPECT_EQ(getEvent<ObAddEvent>(a).tsNs, kMidnight + late);

    // Since-midnight drops from ~24 h to 1 ms: the next day.
    auto b = adapter.translate(JsonBuilder().str("type", "order_executed")
        .uint("timestamp", 1'000'000).uint("orderRef", 1).uint("shares", 10)
        .uint("matchNumber", 1).build());
    EXPECT_EQ(getEvent<ObUpdateEvent>(b).tsNs, kMidnight + kDay + 1'000'000);
    EXPECT_EQ(getEvent<ObTradeEvent>(b).timestampNs, kMidnight + kDay + 1'000'000);

    // And it stays on the new day.
    auto c = adapter.translate(JsonBuilder().str("type", "order_delete")
        .uint("timestamp", 2'000'000).uint("orderRef", 1).build());
    EXPECT_EQ(getEvent<ObDeleteEvent>(c).tsNs, kMidnight + kDay + 2'000'000);
}

TEST(ItchAdapterTest, SmallBackwardStepIsNotAWrap) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(1, "B", 1, 1.0, kT0 + 1'000));
    auto b = adapter.translate(addMsg(2, "B", 1, 1.0, kT0));   // 1 µs earlier
    EXPECT_EQ(getEvent<ObAddEvent>(b).tsNs, kMidnight + kT0);
}

TEST(ItchAdapterTest, LateMessageReceivedJustAfterMidnightIsYesterday) {
    // Wall clock reads 00:00:00.1 on the 17th; the message says 23:59:59.9.
    auto adapter = clockedAdapter(kMidnight + 100'000'000);
    const uint64_t late = kDay - 100'000'000;
    auto a = adapter.translate(addMsg(1, "B", 1, 1.0, late));
    EXPECT_EQ(getEvent<ObAddEvent>(a).tsNs, kMidnight - kDay + late);
}

TEST(ItchAdapterTest, EarlyMessageReceivedJustBeforeMidnightIsTomorrow) {
    // Wall clock reads 23:59:59.9; the message says 00:00:00.1 (clock skew).
    auto adapter = clockedAdapter(kMidnight + kDay - 100'000'000);
    auto a = adapter.translate(addMsg(1, "B", 1, 1.0, 100'000'000));
    EXPECT_EQ(getEvent<ObAddEvent>(a).tsNs, kMidnight + kDay + 100'000'000);
}

TEST(ItchAdapterTest, OutOfRangeOrMissingTimestampIsUnknown) {
    auto adapter = clockedAdapter();
    auto a = adapter.translate(addMsg(1, "B", 1, 1.0, kDay));   // == 24 h: invalid
    EXPECT_EQ(getEvent<ObAddEvent>(a).tsNs, 0u);
    auto b = adapter.translate(R"({"type":"add_order","stock":"FLOW","orderRef":2,"side":"B","shares":1,"price":1,"timestamp":-5})");
    EXPECT_EQ(getEvent<ObAddEvent>(b).tsNs, 0u);
    // An invalid timestamp does not anchor the session date.
    auto c = adapter.translate(addMsg(3, "B", 1, 1.0, kT0));
    EXPECT_EQ(getEvent<ObAddEvent>(c).tsNs, kMidnight + kT0);
}

TEST(ItchAdapterTest, SeqStampingCoexistsWithOrderFlowFields) {
    auto adapter = clockedAdapter();
    adapter.translate(addMsg(1, "B", 100, 10.0, kT0));
    auto events = adapter.translate(JsonBuilder().str("type", "order_executed")
        .uint("timestamp", kT0 + 1).uint("orderRef", 1).uint("shares", 10)
        .uint("matchNumber", 5).uint("seq", 77).build());
    const auto& upd = getEvent<ObUpdateEvent>(events);
    EXPECT_EQ(upd.seq.value(), 77u);
    EXPECT_EQ(upd.cause, MutationCause::Execution);
    const auto& tr = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(tr.seq.value(), 77u);
    EXPECT_EQ(tr.matchNumber.value(), 5u);
}
