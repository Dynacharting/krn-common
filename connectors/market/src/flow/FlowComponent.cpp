#include "krn/flow/FlowComponent.hpp"

#include <algorithm>
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
const std::string kMetaReordered    = "flow.meta.reordered";

uint64_t receiveClockNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void operatorFailed(const IFlowOperator& op, const char* what) {
  KRN_METRIC_HIT("flow.operator_error");
  util::logger().log(util::LogLevel::Warn, "flow.operator.exception",
                     {{"operator", std::string(op.name())}, {"err", what}});
}

const TobLevel& sideOf(const TopOfBook& t, Side s) { return s == Side::Bid ? t.bid : t.ask; }

} // namespace

// ---------------------------------------------------------------------------
// ShadowOrderBook: the per-level FIFO index (review F4/F7).
// ---------------------------------------------------------------------------
void ShadowOrderBook::link_(const ShadowOrder& o) {
  Level& lv = levels_[LevelKey{o.side, o.price.ticks}];
  lv.ids.push_back(o.orderId);
  lv.shares += o.size;
  pos_[o.orderId] = std::prev(lv.ids.end());
}

void ShadowOrderBook::unlink_(const ShadowOrder& o) {
  auto p  = pos_.find(o.orderId);
  auto lv = levels_.find(LevelKey{o.side, o.price.ticks});
  if (lv != levels_.end()) {
    if (p != pos_.end()) lv->second.ids.erase(p->second);
    lv->second.shares -= std::min(lv->second.shares, o.size);
    if (lv->second.ids.empty()) levels_.erase(lv);
  }
  if (p != pos_.end()) pos_.erase(p);
}

ShadowOrder& ShadowOrderBook::insert_(const ShadowOrder& o) {
  erase_(o.orderId);
  ShadowOrder& in = orders_.emplace(o.orderId, o).first->second;
  link_(in);
  return in;
}

void ShadowOrderBook::erase_(uint64_t orderId) {
  auto it = orders_.find(orderId);
  if (it == orders_.end()) return;
  unlink_(it->second);
  orders_.erase(it);
}

void ShadowOrderBook::resize_(ShadowOrder& o, uint64_t newSize) {
  auto lv = levels_.find(LevelKey{o.side, o.price.ticks});
  if (lv != levels_.end()) {
    lv->second.shares -= std::min(lv->second.shares, o.size);
    lv->second.shares += newSize;
  }
  o.size = newSize;
}

void ShadowOrderBook::reprice_(ShadowOrder& o, Price p, double pd) {
  o.priceD = pd;
  if (p == o.price) return;   // OrderBook::updateImpl: same price keeps the place
  unlink_(o);
  o.price = p;
  link_(o);
}

void ShadowOrderBook::clear_() {
  orders_.clear();
  levels_.clear();
  pos_.clear();
  pendingReplace_.reset();
}

uint64_t ShadowOrderBook::sharesAhead(uint64_t orderId) const {
  const ShadowOrder* o = find(orderId);
  if (!o) return 0;
  const Queue* q = level(o->side, o->price);
  if (!q) return 0;
  uint64_t ahead = 0;
  for (uint64_t id : *q) {
    if (id == orderId) break;
    if (const ShadowOrder* x = find(id)) ahead += x->size;
  }
  return ahead;
}

void FlowComponent::TerminatedIds::note(uint64_t id, uint64_t seq) {
  if (seq == 0) return;
  auto& s = seqOf[id];
  if (seq > s) s = seq;
  ring.emplace_back(id, seq);
  if (ring.size() > kTerminatedMemory) {
    const auto [oldId, oldSeq] = ring.front();
    ring.pop_front();
    auto it = seqOf.find(oldId);
    if (it != seqOf.end() && it->second == oldSeq) seqOf.erase(it);
  }
}

uint64_t FlowComponent::TerminatedIds::seqOfId(uint64_t id) const {
  auto it = seqOf.find(id);
  return it == seqOf.end() ? 0 : it->second;
}

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
    {kMetaReordered,    Publication::Stored},
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
  ShadowOrderBook& sh = sf.shadow;
  auto& orders = sh.orders_;
  // A pending replace lives exactly until the next event: its add half is the
  // very next event for ITCH U. Anything else means the add never came.
  std::optional<ReplacedOrder> pending = std::move(sh.pendingReplace_);
  sh.pendingReplace_.reset();

  // F1: an observation older than the newest seen for this symbol was fed by
  // a second strand and overtaken. 0 = unstamped (no ordering information).
  const uint64_t seq = ev.obs.tob.mutationSeq;
  if (seq != 0) {
    if (seq < sf.lastSeq) { ev.reordered = true; ++sf.meta.reordered; }
    else                  sf.lastSeq = seq;
  }
  // The shadow already holds a state of `o` newer than this observation.
  auto newer = [seq](const ShadowOrder& o) { return seq != 0 && o.mutationSeq > seq; };

  std::visit([&](const auto& e) {
    using T = std::decay_t<decltype(e)>;
    if constexpr (std::is_same_v<T, feed::ObAddEvent>) {
      ev.orderId    = e.orderId;
      ev.eventPrice = toTicks_(e.symbol, e.price);
      if (e.replacesOrderId) ev.cause = feed::MutationCause::Replace;
      if (seq != 0) {
        // F1: the order's terminal event (or a later re-add) was observed
        // first: it no longer rests, so it gets no shadow entry.
        const uint64_t termSeq = sf.terminated.seqOfId(e.orderId);
        auto cur = orders.find(e.orderId);
        if (termSeq > seq || (cur != orders.end() && newer(cur->second))) {
          ev.shadowSkipped = true;
          return;
        }
      }
      ShadowOrder o;
      o.orderId       = e.orderId;
      o.side          = e.side;
      o.price         = ev.eventPrice;
      o.priceD        = e.price;
      o.size          = e.size;
      o.addSize       = e.size;
      o.addTsNs       = ev.tsNs;
      o.originOrderId = e.orderId;
      o.originTsNs    = ev.tsNs;
      o.originSize    = e.size;
      o.mutationSeq   = seq;
      // F2: an add of a resting id is delete-then-add (OrderBook::addImpl).
      if (auto cur = orders.find(e.orderId); cur != orders.end()) {
        ev.before       = cur->second;
        ev.duplicateAdd = true;
        sh.erase_(e.orderId);
      }
      if (e.replacesOrderId) {
        if (pending && pending->order.orderId == *e.replacesOrderId) {
          const ShadowOrder& p = pending->order;
          ev.replacedTob = pending->tob;
          if (pending->levelKnown) ev.replacedLevel = OrderLevel{p.side, p.price};
          if (pending->orderKnown) {
            o.originOrderId = p.originOrderId;
            o.originTsNs    = p.originTsNs;
            o.originSize    = p.originSize;
            o.originKnown   = p.originKnown;
            o.replaces      = p.replaces + 1;
            o.filled        = p.filled;
            o.cancelled     = p.cancelled;
            ev.before       = p;
            ev.duplicateAdd = false;
          } else {
            o.originOrderId = *e.replacesOrderId;
            o.originKnown   = false;
            o.replaces      = 1;
          }
        } else {
          o.originOrderId = *e.replacesOrderId;
          o.originKnown   = false;
          o.replaces      = 1;
        }
      }
      ev.after = sh.insert_(o);
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
        if (e.newSize && *e.newSize == 0) sf.terminated.note(e.orderId, seq);
        return;
      }
      ShadowOrder& o = it->second;
      if (newer(o)) { ev.shadowSkipped = true; return; }
      ev.before = o;
      // OrderBook::updateImpl: a price change goes to the back of the new
      // level, a same-price size change keeps the order's place.
      if (e.newPrice) sh.reprice_(o, ev.eventPrice, *e.newPrice);
      if (e.newSize) {
        const uint64_t ns = *e.newSize;
        if (ns < o.size) {
          const uint64_t gone = o.size - ns;
          if (e.cause == feed::MutationCause::Execution)   o.filled    += gone;
          else if (e.cause == feed::MutationCause::Cancel) o.cancelled += gone;
        }
        sh.resize_(o, ns);
      }
      o.mutationSeq = seq;
      if (o.size == 0) { sh.erase_(e.orderId); return; }   // terminal
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
        sf.terminated.note(e.orderId, seq);
        if (e.cause == feed::MutationCause::Replace) {
          // F3: hold the delete half anyway, with the touch it left (when its
          // TobChange shows one), so the add half can test a same-price
          // replace against the touch as it stood before the delete.
          ReplacedOrder r;
          r.order.orderId = e.orderId;
          r.tob           = ev.obs.tob;
          r.orderKnown    = false;
          r.levelKnown    = false;
          for (Side s : {Side::Bid, Side::Ask}) {
            const TobLevel& a = sideOf(r.tob.pre, s);
            const TobLevel& b = sideOf(r.tob.post, s);
            if (a.present && (!b.present || b.price != a.price || b.size < a.size)) {
              r.order.side  = s;
              r.order.price = a.price;
              r.levelKnown  = true;
              break;
            }
          }
          sh.pendingReplace_ = r;
        }
        return;
      }
      if (newer(it->second)) { ev.shadowSkipped = true; return; }
      ShadowOrder o = it->second;
      if (e.cause == feed::MutationCause::Execution)   o.filled    += o.size;
      else if (e.cause == feed::MutationCause::Cancel) o.cancelled += o.size;
      o.mutationSeq = seq;
      ev.before = o;
      if (e.cause == feed::MutationCause::Replace)
        sh.pendingReplace_ = ReplacedOrder{o, ev.obs.tob};
      sh.erase_(e.orderId);   // terminal (a replace's chain continues in its add)
    }
    else if constexpr (std::is_same_v<T, feed::ObTradeEvent>) {
      ev.bookNeutral = e.bookNeutral;
      ev.orderId     = e.restingOrderId.value_or(0);
      if (!ev.obs.unattributed) return;
      ev.unattributed = true;
      ev.cause        = feed::MutationCause::Execution;
      ++sf.meta.unattributed;
      // OrderBook::applyTrade's own passive-side rule, read off the
      // pre-trade top of book.
      const Price tp = toTicks_(e.symbol, e.price);
      ev.eventPrice = tp;
      const TopOfBook& pre = ev.obs.tob.pre;
      std::optional<Side> passive;
      if (e.aggressor == Aggressor::Buy)       passive = Side::Ask;
      else if (e.aggressor == Aggressor::Sell) passive = Side::Bid;
      else if (pre.bid.present && tp.ticks <= pre.bid.price.ticks) passive = Side::Bid;
      else if (pre.ask.present && tp.ticks >= pre.ask.price.ticks) passive = Side::Ask;
      if (passive) consumeUnattributed_(sf, ev, *passive, tp, e.size);
    }
  }, ev.obs.event);
}

// F4: the book consumed (passive, tp) FIFO by quantity with no per-order
// event (OrderBook::consumeAtLevel). Replay that on the shadow's queue.
void FlowComponent::consumeUnattributed_(SymbolFlow& sf, FlowEvent& ev, Side passive, Price tp,
                                         uint64_t qty) {
  ShadowOrderBook& sh = sf.shadow;
  const ShadowOrderBook::LevelKey key{passive, tp.ticks};
  auto lv = sh.levels_.find(key);
  if (lv == sh.levels_.end()) return;
  const uint64_t shadowShares = lv->second.shares;

  // How many shadow shares the trade took. At the pre-trade touch the
  // TobChange gives the book's own level size before and after: the level
  // emptied => every order there filled; otherwise the consumed shares, less
  // the shares there the shadow does not know — orders resting before it
  // started, which were added earlier and so queue ahead of every shadow
  // order. Off the touch the book's level size is not observed: the trade's
  // size, capped at what the shadow holds there.
  bool     all  = false;
  uint64_t take = 0;
  const TobLevel& a = sideOf(ev.obs.tob.pre, passive);
  const TobLevel& b = sideOf(ev.obs.tob.post, passive);
  if (a.present && a.price == tp) {
    if (!(b.present && b.price == tp)) {
      all = true;
    } else {
      const uint64_t consumed     = a.size > b.size ? a.size - b.size : 0;
      const uint64_t unknownAhead = 0;
      take = consumed > unknownAhead ? consumed - unknownAhead : 0;
    }
  } else {
    take = std::min(qty, shadowShares);
  }

  const uint64_t seq = ev.obs.tob.mutationSeq;
  while (all || take > 0) {
    lv = sh.levels_.find(key);
    if (lv == sh.levels_.end() || lv->second.ids.empty()) break;
    const uint64_t id = lv->second.ids.front();
    auto it = sh.orders_.find(id);
    if (it == sh.orders_.end()) { sh.erase_(id); break; }   // unreachable: the index and map agree
    ShadowOrder& o = it->second;
    const uint64_t q = all ? o.size : std::min(o.size, take);
    UnattributedFill f{o, q, q == o.size};
    if (!all) take -= q;
    if (f.terminal) {
      sh.erase_(id);
    } else {
      o.filled += q;
      sh.resize_(o, o.size - q);
      o.mutationSeq = seq;
    }
    ev.fills.push_back(std::move(f));
    if (!ev.fills.back().terminal) break;
  }
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
  counter(kMetaReordered,    m.reordered,    m.reorderedPublished);
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
  sf.shadow.clear_();
  sf.terminated.clear();   // lastSeq is kept: the manager's sequence does not restart
  if (sf.meta.shadowPublished != 0) {
    sf.meta.shadowPublished = 0;
    pub_.state(reset.symbol, kMetaShadowOrders, 0.0);
  }
  runOperators_(reset.symbol, sf, nullptr, /*reset=*/true);
}

} // namespace krn::flow
