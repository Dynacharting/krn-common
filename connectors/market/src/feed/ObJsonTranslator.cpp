#include "krn/feed/ObJsonTranslator.hpp"

namespace krn::feed {

std::optional<MutationCause> parseMutationCause(const std::string& s) {
    if (s == "execution") return MutationCause::Execution;
    if (s == "cancel")    return MutationCause::Cancel;
    if (s == "replace")   return MutationCause::Replace;
    if (s == "unknown")   return MutationCause::Unknown;
    return std::nullopt;
}

namespace {

bool hasU64(const rapidjson::Value& d, const char* k) { return d.HasMember(k) && d[k].IsUint64(); }
bool hasNum(const rapidjson::Value& d, const char* k) { return d.HasMember(k) && d[k].IsNumber(); }
bool hasStr(const rapidjson::Value& d, const char* k) { return d.HasMember(k) && d[k].IsString(); }

uint64_t optTsNs(const rapidjson::Value& d) { return hasU64(d, "tsNs") ? d["tsNs"].GetUint64() : 0; }

MutationCause optCause(const rapidjson::Value& d) {
    if (!hasStr(d, "cause")) return MutationCause::Unknown;
    return parseMutationCause(d["cause"].GetString()).value_or(MutationCause::Unknown);
}

} // namespace

ObJsonFrame translateObJson(const rapidjson::Value& doc) {
    ObJsonFrame f;
    if (!doc.IsObject() || !hasStr(doc, "symbol") || !hasStr(doc, "action")) return f;

    f.symbol = doc["symbol"].GetString();
    f.action = doc["action"].GetString();
    if (f.symbol.empty() || f.symbol.size() > 64) { f.symbol.clear(); f.action.clear(); return f; }
    if (hasU64(doc, "seq")) f.seq = doc["seq"].GetUint64();

    const std::string& a = f.action;
    f.status = ObJsonFrame::Status::BadBody;

    if (a == "ticksize") {
        if (!hasNum(doc, "tickSize")) return f;
        f.events.emplace_back(ObTickSizeEvent{f.symbol, doc["tickSize"].GetDouble()});

    } else if (a == "add") {
        if (!hasU64(doc, "id") || !hasStr(doc, "side") || !hasNum(doc, "price") || !hasU64(doc, "size"))
            return f;
        const std::string sideStr = doc["side"].GetString();
        const Side side = (sideStr == "ask") ? Side::Ask : Side::Bid;
        const uint64_t priority = hasU64(doc, "priority") ? doc["priority"].GetUint64() : 0;
        f.events.emplace_back(ObAddEvent{f.symbol, doc["id"].GetUint64(), side,
                                         doc["price"].GetDouble(), doc["size"].GetUint64(),
                                         priority, f.seq, optTsNs(doc), std::nullopt});

    } else if (a == "update") {
        if (!hasU64(doc, "id")) return f;
        std::optional<double> newPrice;
        std::optional<uint64_t> newSize;
        if (hasNum(doc, "price")) newPrice = doc["price"].GetDouble();
        if (hasU64(doc, "size"))  newSize  = doc["size"].GetUint64();
        f.events.emplace_back(ObUpdateEvent{f.symbol, doc["id"].GetUint64(), newPrice, newSize,
                                            f.seq, optCause(doc), optTsNs(doc)});

    } else if (a == "delete") {
        if (!hasU64(doc, "id")) return f;
        f.events.emplace_back(ObDeleteEvent{f.symbol, doc["id"].GetUint64(), f.seq,
                                            optCause(doc), optTsNs(doc)});

    } else if (a == "trade") {
        if (!hasNum(doc, "price") || !hasU64(doc, "size")) return f;
        Aggressor aggr = Aggressor::Unknown;
        if (hasStr(doc, "aggressor")) {
            const std::string s = doc["aggressor"].GetString();
            if (s == "buy") aggr = Aggressor::Buy;
            else if (s == "sell") aggr = Aggressor::Sell;
        }
        f.events.emplace_back(ObTradeEvent{f.symbol, doc["price"].GetDouble(), doc["size"].GetUint64(),
                                           aggr, optTsNs(doc), /*bookNeutral=*/false, f.seq,
                                           std::nullopt, std::nullopt});

    } else {
        f.status = ObJsonFrame::Status::UnknownAction;
        return f;
    }

    f.status = ObJsonFrame::Status::Ok;
    return f;
}

} // namespace krn::feed
