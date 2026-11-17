#pragma once
#include <vector>
#include <optional>
#include <string>
#include "krn/book/OrderBook.hpp"  // for Price, Side

namespace krn {

// One level’s new total after a mutation
struct LevelDelta {
    Side side;
    Price price;
    uint64_t totalSize = 0;
};

// Streamed incremental change
struct BookDelta {
    std::string symbol;
    uint64_t seq = 0;  // manager-emitted monotonically increasing sequence (per symbol)
    std::vector<LevelDelta> levels;  // only levels whose totals changed
    std::optional<std::pair<Price,uint64_t>> bid; // present iff TOB bid changed
    std::optional<std::pair<Price,uint64_t>> ask; // present iff TOB ask changed
};

// One side of the top of book. `present` is false for an empty side, and then
// `price`/`size` are zero. `price` is the tick index; `priceD` the same price
// in the symbol's units (ticks * tickSize), for comparing against FeedEvent
// prices. Compare levels on `price` (exact), never on `priceD`.
struct TobLevel {
    bool     present = false;
    Price    price{};
    double   priceD  = 0.0;
    uint64_t size    = 0;
};

// Best bid and best ask at one instant.
struct TopOfBook {
    TobLevel bid;
    TobLevel ask;
};

// Top of book immediately before and after one mutation, both read under the
// manager's per-symbol lock in the same critical section as the mutation
// (ENC-1645, SPEC 2026-11-17-order-flow-pipelines D3). For a mutation that
// changed nothing (the mutator returned false) `post` equals `pre`.
struct TobChange {
    TopOfBook pre;
    TopOfBook post;
    uint64_t  mutationSeq = 0;
};

// Top-N snapshot at a moment in time
struct DepthSnapshot {
    std::string symbol;
    uint64_t seq = 0;     // same counter as deltas
    uint32_t epoch = 0;   // from FeedState
    std::vector<std::pair<Price,uint64_t>> bids; // ordered best->worse
    std::vector<std::pair<Price,uint64_t>> asks; // ordered best->worse
};

} // namespace krn
