#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <rapidjson/document.h>

#include "krn/book/OrderBook.hpp"   // Side, Aggressor

namespace krn::feed {

// ---- Canonical events that any feed adapter can produce ----
//
// Field order is part of the contract: every construction site in this repo
// uses positional aggregate init, so new fields are only ever APPENDED (after
// `seq`) and always carry a default. Order-flow fields (ENC-1644, SPEC
// 2026-11-17-order-flow-pipelines D2):
//   - `cause`  — why a resting order shrank or left the book;
//   - `tsNs`   — exchange event time, ns since the Unix epoch (0 = unknown);
//   - match identity on trades (`restingOrderId`, `matchNumber`);
//   - `replacesOrderId` on the add half of a replace.

/// Why a book mutation happened. `Unknown` when the source does not say.
enum class MutationCause : uint8_t {
    Unknown   = 0,
    Execution = 1,   // the resting order traded (fully or partly)
    Cancel    = 2,   // the owner pulled shares (partial cancel or delete)
    Replace   = 3,   // the order was replaced (ITCH U delete half; MBO M)
};

/// A market-data tick (price, volume, arbitrary numeric fields).
struct TickEvent {
    std::string symbol;
    std::shared_ptr<rapidjson::Document> payload;
    uint64_t timestampNs = 0;   // nanoseconds since epoch (0 = not provided)
};

/// Add an order to the book.
struct ObAddEvent {
    std::string symbol;
    uint64_t    orderId;
    Side        side;
    double      price;
    uint64_t    size;
    uint64_t    priority = 0;
    // Optional per-message wire sequence for gap detection (OrderBookManager
    // ::onSeq). Empty when the source carries no sequence — no behavior change.
    std::optional<uint64_t> seq;
    uint64_t    tsNs = 0;   // exchange time, ns since epoch (0 = unknown)
    // The add half of a replace: the id of the order this add replaces, so a
    // replaced order keeps its identity. Empty for an ordinary add.
    std::optional<uint64_t> replacesOrderId;
};

/// Update an existing order (price and/or size).
struct ObUpdateEvent {
    std::string              symbol;
    uint64_t                 orderId;
    std::optional<double>    newPrice;
    std::optional<uint64_t>  newSize;
    std::optional<uint64_t>  seq;   // optional per-message sequence (gap detect)
    MutationCause            cause = MutationCause::Unknown;
    uint64_t                 tsNs  = 0;   // exchange time, ns since epoch (0 = unknown)
};

/// Delete an order from the book.
struct ObDeleteEvent {
    std::string symbol;
    uint64_t    orderId;
    std::optional<uint64_t> seq;   // optional per-message sequence (gap detect)
    MutationCause cause = MutationCause::Unknown;
    uint64_t    tsNs  = 0;   // exchange time, ns since epoch (0 = unknown)
};

/// A trade executed on the venue.
struct ObTradeEvent {
    std::string symbol;
    double      price;
    uint64_t    size;
    Aggressor   aggressor = Aggressor::Unknown;
    uint64_t    timestampNs = 0;   // nanoseconds since epoch (0 = not provided)
    // Print-only flag: when true this trade is a TA/print signal ONLY and must
    // NOT consume resting liquidity. Executions already mutate the book via an
    // explicit add/update/delete, and ITCH `trade` prints are non-displayable
    // hidden executions — in both cases consuming the book would double-count.
    bool        bookNeutral = false;
    std::optional<uint64_t> seq;   // optional per-message sequence (gap detect)
    // The resting order this execution traded against (empty for a print with
    // no displayed resting order, e.g. an ITCH `P`).
    std::optional<uint64_t> restingOrderId;
    // The venue's match number for this execution (empty when not provided).
    // Per execution — NOT a grouping key for one aggressive order.
    std::optional<uint64_t> matchNumber;
};

/// Set the tick size for a symbol.
struct ObTickSizeEvent {
    std::string symbol;
    double      tickSize;
};

/// Reset / new epoch for a symbol's order book.
struct ObResetEvent {
    std::string symbol;
    uint32_t    epoch = 0;
};

/// Union of every event a feed adapter may emit.
using FeedEvent = std::variant<
    TickEvent,
    ObAddEvent,
    ObUpdateEvent,
    ObDeleteEvent,
    ObTradeEvent,
    ObTickSizeEvent,
    ObResetEvent
>;

} // namespace krn::feed
