#include "krn/flow/FlowComponent.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

#include "krn/book/OrderBookManager.hpp"
#include "krn/flow/FlowOperators.hpp"
#include "krn/util/Logger.hpp"
#include "krn/util/Metrics.hpp"

namespace krn::flow {

namespace {

const std::string kMetaTsSource     = "flow.meta.ts_source";
const std::string kMetaUnattributed = "flow.meta.unattributed";
const std::string kMetaUnknownOrder = "flow.meta.unknown_order";
const std::string kMetaUnknownCause = "flow.meta.unknown_cause";
const std::string kMetaShadowOrders = "flow.meta.shadow_orders";

uint64_t receiveClockNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void operatorFailed(const IFlowOperator& op, const char* what) {
  KRN_METRIC_HIT("flow.operator_error");
  util::logger().log(util::LogLevel::Warn, "flow.operator.exception",
                     {{"operator", std::string(op.name())}, {"err", what}});
}

} // namespace

FlowComponent::FlowComponent(Env env) : FlowComponent(env, makeFlowOperators()) {}

FlowComponent::FlowComponent(Env env, std::vector<std::unique_ptr<IFlowOperator>> operators)
  : env_(env),
    ops_(std::move(operators)),
    pub_(env.dispatcher, env.store),
    config_(std::make_shared<FlowConfigSource>()) {}

FlowComponent::~FlowComponent() = default;

const std::vector<FlowKey>& FlowComponent::coreKeys() {
  static const std::vector<FlowKey> k{
    {kMetaTsSource,     Publication::Stored},
    {kMetaUnattributed, Publication::Stored},
    {kMetaUnknownOrder, Publication::Stored},
    {kMetaUnknownCause, Publication::Stored},
    {kMetaShadowOrders, Publication::Stored},
  };
  return k;
}

std::vector<FlowKey> FlowComponent::vocabulary() const {
  std::vector<FlowKey> out = coreKeys();
  for (const auto& op : ops_) {
    auto ks = op->keys();
    out.insert(out.end(), ks.begin(), ks.end());
  }
  return out;
}

void FlowComponent::start() {
  std::call_once(startOnce_, [this] {
    cfg_ = config_->snapshot();
    pub_.setVocabulary(vocabulary());
    for (auto& op : ops_) {
      try { op->configure(cfg_, *this); }
      catch (const std::exception& ex) { operatorFailed(*op, ex.what()); }
      catch (...) { operatorFailed(*op, "unknown"); }
    }
    started_.store(true, std::memory_order_release);
  });
}

FlowComponent::SymbolFlow& FlowComponent::symbolFlow_(const std::string& symbol) {
  std::lock_guard<std::mutex> lk(symbolsMu_);
  auto it = symbols_.find(symbol);
  if (it != symbols_.end()) return *it->second;
  auto sf = std::make_unique<SymbolFlow>();
  sf->states.reserve(ops_.size());
  for (const auto& op : ops_) sf->states.push_back(op->makeState());
  return *symbols_.emplace(symbol, std::move(sf)).first->second;
}

FlowComponent::SymbolFlow* FlowComponent::findSymbolFlow_(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(symbolsMu_);
  auto it = symbols_.find(symbol);
  return it == symbols_.end() ? nullptr : it->second.get();
}

std::size_t FlowComponent::shadowSize(const std::string& symbol) const {
  SymbolFlow* sf = findSymbolFlow_(symbol);
  if (!sf) return 0;
  std::lock_guard<std::mutex> lk(sf->mu);
  return sf->shadow.size();
}

Price FlowComponent::toTicks_(const std::string& symbol, double px) const {
  if (env_.books) return env_.books->toTicks(symbol, px);
  // No manager (a bare test rig): OrderBookManager's own default tick, 1e-4.
  return Price{static_cast<int64_t>(std::llround(px / 1e-4))};
}

bool FlowComponent::withSymbol(const std::string& symbol, const IFlowOperator& op,
                               const std::function<void(FlowContext&)>& fn) {
  std::size_t idx = ops_.size();
  for (std::size_t i = 0; i < ops_.size(); ++i)
    if (ops_[i].get() == &op) { idx = i; break; }
  if (idx == ops_.size()) return false;
  SymbolFlow* sf = findSymbolFlow_(symbol);
  if (!sf) return false;
  std::lock_guard<std::mutex> lk(sf->mu);
  FlowContext ctx{symbol, nullptr, sf->shadow, pub_, cfg_, sf->states[idx].get()};
  fn(ctx);
  return true;
}

// ---------------------------------------------------------------------------
// Resolution: update the shadow and fill FlowEvent's before/after. Runs under
// the symbol's mutex.
// ---------------------------------------------------------------------------
void FlowComponent::resolve_(SymbolFlow& sf, FlowEvent& ev) {
  auto& orders = sf.shadow.orders_;
  // A pending replace lives exactly until the next event: its add half is the
  // very next event for ITCH U. Anything else means the add never came.
  std::optional<ReplacedOrder> pending = std::move(sf.shadow.pendingReplace_);
  sf.shadow.pendingReplace_.reset();

  std::visit([&](const auto& e) {
    using T = std::decay_t<decltype(e)>;
    if constexpr (std::is_same_v<T, feed::ObAddEvent>) {
      ShadowOrder o;
      o.orderId       = e.orderId;
      o.side          = e.side;
      o.price         = toTicks_(e.symbol, e.price);
      o.priceD        = e.price;
      o.size          = e.size;
      o.addSize       = e.size;
      o.addTsNs       = ev.tsNs;
      o.originOrderId = e.orderId;
      o.originTsNs    = ev.tsNs;
      o.originSize    = e.size;
      ev.orderId      = e.orderId;
      ev.eventPrice   = o.price;
      if (e.replacesOrderId) {
        ev.cause = feed::MutationCause::Replace;
        if (pending && pending->order.orderId == *e.replacesOrderId) {
          const ShadowOrder& p = pending->order;
          o.originOrderId = p.originOrderId;
          o.originTsNs    = p.originTsNs;
          o.originSize    = p.originSize;
          o.originKnown   = p.originKnown;
          o.replaces      = p.replaces + 1;
          o.filled        = p.filled;
          o.cancelled     = p.cancelled;
          ev.before       = p;
          ev.replacedTob  = pending->tob;
        } else {
          o.originOrderId = *e.replacesOrderId;
          o.originKnown   = false;
          o.replaces      = 1;
        }
      }
      orders[e.orderId] = o;
      ev.after = o;
    }
    else if constexpr (std::is_same_v<T, feed::ObUpdateEvent>) {
      ev.orderId = e.orderId;
      ev.cause   = e.cause;
      if (e.cause == feed::MutationCause::Unknown) ++sf.meta.unknownCause;
      if (e.newPrice) ev.eventPrice = toTicks_(e.symbol, *e.newPrice);
      auto it = orders.find(e.orderId);
      if (it == orders.end()) {
        ev.orderUnknown = true;
        ++sf.meta.unknownOrder;
        return;
      }
      ShadowOrder& o = it->second;
      ev.before = o;
      if (e.newPrice) { o.price = ev.eventPrice; o.priceD = *e.newPrice; }
      if (e.newSize) {
        const uint64_t ns = *e.newSize;
        if (ns < o.size) {
          const uint64_t gone = o.size - ns;
          if (e.cause == feed::MutationCause::Execution)   o.filled    += gone;
          else if (e.cause == feed::MutationCause::Cancel) o.cancelled += gone;
        }
        o.size = ns;
      }
      if (o.size == 0) { orders.erase(it); return; }   // terminal
      ev.after = o;
    }
    else if constexpr (std::is_same_v<T, feed::ObDeleteEvent>) {
      ev.orderId = e.orderId;
      ev.cause   = e.cause;
      if (e.cause == feed::MutationCause::Unknown) ++sf.meta.unknownCause;
      auto it = orders.find(e.orderId);
      if (it == orders.end()) {
        ev.orderUnknown = true;
        ++sf.meta.unknownOrder;
        return;
      }
      ShadowOrder o = it->second;
      if (e.cause == feed::MutationCause::Execution)   o.filled    += o.size;
      else if (e.cause == feed::MutationCause::Cancel) o.cancelled += o.size;
      ev.before = o;
      if (e.cause == feed::MutationCause::Replace)
        sf.shadow.pendingReplace_ = ReplacedOrder{o, ev.obs.tob};
      orders.erase(it);   // terminal (a replace's chain continues in its add)
    }
    else if constexpr (std::is_same_v<T, feed::ObTradeEvent>) {
      ev.bookNeutral = e.bookNeutral;
      ev.orderId     = e.restingOrderId.value_or(0);
      if (!ev.obs.unattributed) return;
      ev.unattributed = true;
      ev.cause        = feed::MutationCause::Execution;
      ++sf.meta.unattributed;
      // The book consumed this level FIFO with no per-order event: the shadow
      // can no longer say what is left of any order there. Forget them; later
      // events on them count as unknown orders (OrderBook::applyTrade's own
      // passive-side rule, read off the pre-trade top of book).
      const Price tp = toTicks_(e.symbol, e.price);
      ev.eventPrice = tp;
      const TopOfBook& pre = ev.obs.tob.pre;
      std::optional<Side> passive;
      if (e.aggressor == Aggressor::Buy)       passive = Side::Ask;
      else if (e.aggressor == Aggressor::Sell) passive = Side::Bid;
      else if (pre.bid.present && tp.ticks <= pre.bid.price.ticks) passive = Side::Bid;
      else if (pre.ask.present && tp.ticks >= pre.ask.price.ticks) passive = Side::Ask;
      if (!passive) return;
      for (auto it = orders.begin(); it != orders.end();) {
        if (it->second.side == *passive && it->second.price == tp) it = orders.erase(it);
        else ++it;
      }
    }
  }, ev.obs.event);
}

void FlowComponent::publishMeta_(const std::string& symbol, SymbolFlow& sf, bool exchangeTs) {
  CoreMeta& m = sf.meta;
  const int src = exchangeTs ? 0 : 1;
  if (src != m.tsSource) {
    m.tsSource = src;
    pub_.state(symbol, kMetaTsSource, src);
  }
  // Counters only move up; publish on the event that moved them.
  auto counter = [&](const std::string& key, uint64_t value, uint64_t& published) {
    if (value == published) return;
    published = value;
    pub_.state(symbol, key, static_cast<double>(value));
  };
  counter(kMetaUnattributed, m.unattributed, m.unattributedPublished);
  counter(kMetaUnknownOrder, m.unknownOrder, m.unknownOrderPublished);
  counter(kMetaUnknownCause, m.unknownCause, m.unknownCausePublished);
  if (sf.shadow.size() != m.shadowPublished) {
    m.shadowPublished = sf.shadow.size();
    pub_.state(symbol, kMetaShadowOrders, static_cast<double>(m.shadowPublished));
  }
}

void FlowComponent::runOperators_(const std::string& symbol, SymbolFlow& sf,
                                  const FlowEvent* ev, bool reset) {
  for (std::size_t i = 0; i < ops_.size(); ++i) {
    FlowContext ctx{symbol, ev, sf.shadow, pub_, cfg_, sf.states[i].get()};
    try {
      if (reset) ops_[i]->onReset(ctx);
      else       ops_[i]->onEvent(ctx);
    }
    catch (const std::exception& ex) { operatorFailed(*ops_[i], ex.what()); }
    catch (...) { operatorFailed(*ops_[i], "unknown"); }
  }
}

void FlowComponent::onFlowEvent(const feed::FlowObservation& obs) {
  start();
  if (!cfg_.enabled) return;

  FlowEventKind kind{};
  const std::string* symbol = nullptr;
  uint64_t rawTs = 0;
  const bool known = std::visit([&](const auto& e) -> bool {
    using T = std::decay_t<decltype(e)>;
    if constexpr (std::is_same_v<T, feed::ObAddEvent>) {
      kind = FlowEventKind::Add;    symbol = &e.symbol; rawTs = e.tsNs; return true;
    } else if constexpr (std::is_same_v<T, feed::ObUpdateEvent>) {
      kind = FlowEventKind::Update; symbol = &e.symbol; rawTs = e.tsNs; return true;
    } else if constexpr (std::is_same_v<T, feed::ObDeleteEvent>) {
      kind = FlowEventKind::Delete; symbol = &e.symbol; rawTs = e.tsNs; return true;
    } else if constexpr (std::is_same_v<T, feed::ObTradeEvent>) {
      kind = FlowEventKind::Trade;  symbol = &e.symbol; rawTs = e.timestampNs; return true;
    } else {
      return false;
    }
  }, obs.event);
  if (!known) return;

  const bool exchangeTs = rawTs != 0;
  const uint64_t tsNs = exchangeTs ? rawTs : receiveClockNs();

  for (auto& op : ops_) {
    try { op->onBeforeEvent(*symbol, tsNs); }
    catch (const std::exception& ex) { operatorFailed(*op, ex.what()); }
    catch (...) { operatorFailed(*op, "unknown"); }
  }

  SymbolFlow& sf = symbolFlow_(*symbol);
  std::lock_guard<std::mutex> lk(sf.mu);
  FlowEvent ev{kind, obs, *symbol};
  ev.tsNs       = tsNs;
  ev.exchangeTs = exchangeTs;
  resolve_(sf, ev);
  publishMeta_(*symbol, sf, exchangeTs);
  runOperators_(*symbol, sf, &ev, /*reset=*/false);
}

void FlowComponent::onBookReset(const feed::ObResetEvent& reset) {
  start();
  if (!cfg_.enabled) return;
  SymbolFlow& sf = symbolFlow_(reset.symbol);
  std::lock_guard<std::mutex> lk(sf.mu);
  sf.shadow.orders_.clear();
  sf.shadow.pendingReplace_.reset();
  if (sf.meta.shadowPublished != 0) {
    sf.meta.shadowPublished = 0;
    pub_.state(reset.symbol, kMetaShadowOrders, 0.0);
  }
  runOperators_(reset.symbol, sf, nullptr, /*reset=*/true);
}

} // namespace krn::flow
