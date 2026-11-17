#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "krn/feed/FeedEvent.hpp"

namespace krn::feed {

// FeedServer's JSON `{"type":"ob",...}` line -> FeedEvents (ENC-1645, SPEC
// 2026-11-17-order-flow-pipelines D3). Pure: no book, no metrics, no seq
// side effects — FeedServer gates `seq` between the header and the body, so
// the result says which part failed.
//
// Accepted (unchanged from the pre-ENC-1645 FeedServer):
//   action "ticksize": tickSize (number)                    -> ObTickSizeEvent
//   action "add":      id, side ("ask" | anything = bid), price, size,
//                      [priority]                           -> ObAddEvent
//   action "update":   id, [price], [size]                  -> ObUpdateEvent
//   action "delete":   id                                   -> ObDeleteEvent
//   action "trade":    price, size, [aggressor "buy"|"sell"] -> ObTradeEvent
//                      (never bookNeutral: it consumes the book)
// Added (D2/D3), optional, same names as the FeedEvent fields:
//   "cause": "execution" | "cancel" | "replace" | "unknown"  (update, delete)
//   "tsNs":  uint64 exchange time, ns since epoch            (add, update,
//            delete; trade -> ObTradeEvent::timestampNs)
// Like "priority", an optional field of the wrong type (or an unrecognised
// cause string) is ignored, not an error, so no existing line changes fate.
// `seq` (uint64) is copied onto the event as well as reported.
struct ObJsonFrame {
    enum class Status {
        Ok,             // events holds exactly one event
        BadHeader,      // symbol/action missing, not strings, or symbol empty / > 64
        BadBody,        // a required field for `action` missing or mistyped
        UnknownAction,  // header fine, action not recognised
    };
    Status                  status = Status::BadHeader;
    std::string             symbol;   // set unless BadHeader
    std::string             action;   // set unless BadHeader
    std::optional<uint64_t> seq;      // set unless BadHeader, when present as uint64
    std::vector<FeedEvent>  events;
};

ObJsonFrame translateObJson(const rapidjson::Value& doc);

// "execution" | "cancel" | "replace" | "unknown" -> MutationCause; nullopt otherwise.
std::optional<MutationCause> parseMutationCause(const std::string& s);

} // namespace krn::feed
