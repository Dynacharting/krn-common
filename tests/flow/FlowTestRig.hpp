// Shared rig for the flow tests (ENC-1646): a real OrderBookManager fed
// through a real IngressApplier, with a FlowComponent registered as its flow
// observer, publishing into a real Dispatcher (no thread pool, so a
// notifyListeners delivers inline and in order) and AtomicStore.
#pragma once

#include "krn/AtomicStore.hpp"
#include "krn/Dispatcher.hpp"
#include "krn/StreamValue.hpp"
#include "krn/book/OrderBookManager.hpp"
#include "krn/feed/FeedEvent.hpp"
#include "krn/feed/IFlowObserver.hpp"
#include "krn/feed/IngressApplier.hpp"
#include "krn/flow/FlowComponent.hpp"
#include "krn/nodes/INode.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace krn::flowtest {

using krn::feed::FeedEvent;
using krn::feed::MutationCause;

inline const std::string kSym = "FLOW";
constexpr uint64_t kTs = 1'794'907'800'000'000'000ULL;   // 2026-11-17T09:30:00Z, epoch ns

inline FeedEvent add(uint64_t id, Side side, double px, uint64_t size, uint64_t ts = kTs,
                     std::optional<uint64_t> replaces = std::nullopt) {
  return feed::ObAddEvent{kSym, id, side, px, size, 0, std::nullopt, ts, replaces};
}
inline FeedEvent upd(uint64_t id, uint64_t newSize, MutationCause cause, uint64_t ts = kTs,
                     std::optional<double> newPrice = std::nullopt) {
  return feed::ObUpdateEvent{kSym, id, newPrice, newSize, std::nullopt, cause, ts};
}
inline FeedEvent del(uint64_t id, MutationCause cause, uint64_t ts = kTs) {
  return feed::ObDeleteEvent{kSym, id, std::nullopt, cause, ts};
}
inline FeedEvent trade(double px, uint64_t size, Aggressor aggr, bool bookNeutral,
                       uint64_t ts = kTs, std::optional<uint64_t> resting = std::nullopt) {
  return feed::ObTradeEvent{kSym, px, size, aggr, ts, bookNeutral, std::nullopt, resting,
                            std::nullopt};
}

// Records every value notified on one (symbol, field).
class KeyRecorder final : public INode {
public:
  KeyRecorder(std::string key, std::vector<std::pair<std::string, double>>* log)
    : key_(std::move(key)), log_(log) {}
  void onValue(const StreamValue& sv) override {
    double v = 0;
    if (std::holds_alternative<double>(sv.value)) v = std::get<double>(sv.value);
    else if (std::holds_alternative<int>(sv.value)) v = std::get<int>(sv.value);
    log_->emplace_back(key_, v);
  }
  void shutdown() noexcept override {}

private:
  std::string key_;
  std::vector<std::pair<std::string, double>>* log_;
};

struct Rig {
  OrderBookManager                            mgr;
  AtomicStore                                 store;
  Dispatcher                                  dispatcher{nullptr, &store};
  std::shared_ptr<feed::FlowObserverRegistry> reg = std::make_shared<feed::FlowObserverRegistry>();
  std::shared_ptr<flow::FlowComponent>        flow;
  feed::IngressApplier                        applier{&dispatcher, &mgr, reg};
  std::vector<std::pair<std::string, double>> log;   // (key, value) in notify order
  std::vector<std::shared_ptr<KeyRecorder>>   recorders;

  // attach=false: the component exists but is not yet an observer, so
  // frames applied now are "before connect" for it.
  explicit Rig(bool attach = true) {
    mgr.setTickSize(kSym, 0.01);
    flow = std::make_shared<flow::FlowComponent>(
        flow::FlowComponent::Env{&dispatcher, &store, &mgr, nullptr});
    for (const auto& k : flow->vocabulary()) {
      auto r = std::make_shared<KeyRecorder>(k.name, &log);
      dispatcher.registerListener(kSym, k.name, r);
      recorders.push_back(r);
    }
    if (attach) reg->add(flow);
  }
  void attach() { reg->add(flow); }

  feed::IngressApplier::FrameResult frame(std::vector<FeedEvent> f) {
    return applier.apply(f);
  }

  // Sum of the values notified per key since the last take(), and clear.
  std::map<std::string, double> take() {
    std::map<std::string, double> out;
    for (const auto& [k, v] : log) out[k] += v;
    log.clear();
    return out;
  }

  double stored(const std::string& key) const {
    auto v = store.get(kSym, key);
    if (!v) return -1;
    if (std::holds_alternative<double>(*v)) return std::get<double>(*v);
    if (std::holds_alternative<int>(*v)) return std::get<int>(*v);
    return -2;
  }
};

// The standard book: bid 10.00 = #1 100 + #2 50, bid 9.99 = #3 200;
// ask 10.01 = #4 80, ask 10.02 = #5 300.
inline void seed(Rig& r) {
  r.frame({add(1, Side::Bid, 10.00, 100)});
  r.frame({add(2, Side::Bid, 10.00, 50)});
  r.frame({add(3, Side::Bid, 9.99, 200)});
  r.frame({add(4, Side::Ask, 10.01, 80)});
  r.frame({add(5, Side::Ask, 10.02, 300)});
}

} // namespace krn::flowtest
