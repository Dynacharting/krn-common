#include "krn/flow/SweepOperator.hpp"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "krn/flow/FlowComponent.hpp"

namespace krn::flow {

const std::string SweepOperator::kLevels   = "flow.sweep.levels";
const std::string SweepOperator::kNotional = "flow.sweep.notional";
const std::string SweepOperator::kPx       = "flow.sweep.px";
const std::string SweepOperator::kSide     = "flow.sweep.side";
const std::string SweepOperator::kCount    = "flow.sweep.count";

namespace {

uint64_t steadyNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One execution, as P2 needs it.
struct Exec {
  Side     resting = Side::Bid;
  int64_t  ticks   = 0;
  double   px      = 0.0;
  uint64_t shares  = 0;
};

// One open aggressive event on one resting side.
struct Run {
  bool                 open   = false;
  uint64_t             seq    = 0;   // open order, so a two-sided close publishes in open order
  std::vector<int64_t> prices;       // distinct tick prices, in the order reached
  double               notional = 0.0;
  double               lastPx   = 0.0;

  void add(const Exec& x) {
    if (std::find(prices.begin(), prices.end(), x.ticks) == prices.end()) prices.push_back(x.ticks);
    notional += static_cast<double>(x.shares);
    lastPx = x.px;
  }
  void clear() {
    open = false;
    prices.clear();
    notional = 0.0;
    lastPx   = 0.0;
  }
};

struct SweepState final : OperatorState {
  Run      runs[2];          // [0] resting bid (sell aggressor), [1] resting ask (buy aggressor)
  uint64_t tsNs     = 0;     // the open runs' shared exchange tsNs
  uint64_t dueNs    = 0;     // receive clock at which the timer closes them
  uint64_t nextSeq  = 0;
  bool anyOpen() const { return runs[0].open || runs[1].open; }
};

int sideIndex(Side s) { return s == Side::Bid ? 0 : 1; }

// The side whose pre touch lost shares in this event, and how many.
bool fromTob(const TobChange& t, Exec& x) {
  auto lost = [](const TobLevel& pre, const TobLevel& post) -> uint64_t {
    if (!pre.present) return 0;
    if (post.present && post.price == pre.price)
      return post.size < pre.size ? pre.size - post.size : 0;
    return pre.size;   // the level left the touch
  };
  const uint64_t b = lost(t.pre.bid, t.post.bid);
  const uint64_t a = lost(t.pre.ask, t.post.ask);
  if ((b == 0) == (a == 0)) return false;   // neither, or both: not one level
  const TobLevel& lvl = b ? t.pre.bid : t.pre.ask;
  x.resting = b ? Side::Bid : Side::Ask;
  x.ticks   = lvl.price.ticks;
  x.px      = lvl.priceD;
  x.shares  = b ? b : a;
  return true;
}

// Is this event an execution P2 groups, and of what?
bool execution(const FlowEvent& ev, Exec& x) {
  if (ev.kind == FlowEventKind::Trade) {
    if (!ev.unattributed) return false;   // bookNeutral prints (E or hidden P) and the rest
    const auto* t = std::get_if<feed::ObTradeEvent>(&ev.obs.event);
    if (!t) return false;
    const TopOfBook& pre = ev.tob().pre;
    const int64_t tp = ev.eventPrice.ticks;
    if (t->aggressor == Aggressor::Buy)       x.resting = Side::Ask;
    else if (t->aggressor == Aggressor::Sell) x.resting = Side::Bid;
    else if (pre.bid.present && tp <= pre.bid.price.ticks) x.resting = Side::Bid;
    else if (pre.ask.present && tp >= pre.ask.price.ticks) x.resting = Side::Ask;
    else return false;
    x.ticks  = tp;
    x.px     = t->price;
    x.shares = t->size;
    return x.shares > 0;
  }
  if (ev.kind != FlowEventKind::Update && ev.kind != FlowEventKind::Delete) return false;
  if (ev.cause != feed::MutationCause::Execution) return false;
  if (ev.orderUnknown || !ev.before) return fromTob(ev.tob(), x);
  const ShadowOrder& b = *ev.before;
  const uint64_t left = ev.after ? ev.after->size : 0;
  if (left >= b.size) return false;
  x.resting = b.side;
  x.ticks   = b.price.ticks;
  x.px      = b.priceD;
  x.shares  = b.size - left;
  return true;
}

void publishIfSweep(const FlowContext& ctx, const Run& r, Side resting, uint32_t minLevels) {
  if (r.prices.size() < minLevels) return;
  const std::string& s = ctx.symbol;
  ctx.pub.increment(s, SweepOperator::kLevels, static_cast<double>(r.prices.size()));
  ctx.pub.increment(s, SweepOperator::kNotional, r.notional);
  ctx.pub.increment(s, SweepOperator::kPx, r.lastPx);
  ctx.pub.increment(s, SweepOperator::kSide, resting == Side::Bid ? -1.0 : 1.0);
  ctx.pub.increment(s, SweepOperator::kCount, 1.0);
}

} // namespace

SweepOperator::SweepOperator() = default;

SweepOperator::SweepOperator(Timing timing) : injected_(true), timing_(std::move(timing)) {}

SweepOperator::~SweepOperator() {
  // A scheduled close may still be pending on the io_context; it holds only a
  // weak reference and sees `alive == false`.
  std::lock_guard<std::mutex> lk(life_->mu);
  life_->alive = false;
}

std::vector<FlowKey> SweepOperator::keys() const {
  return {{kLevels, Publication::Pushed}, {kNotional, Publication::Pushed},
          {kPx, Publication::Pushed},     {kSide, Publication::Pushed},
          {kCount, Publication::Pushed}};
}

void SweepOperator::configure(const FlowConfig& cfg, FlowComponent& component) {
  component_ = &component;
  minLevels_ = cfg.sweepMinLevels;
  closeNs_   = cfg.sweepCloseMs * 1'000'000ULL;
  if (injected_) {
    if (!timing_.now) timing_.now = steadyNs;
    return;
  }
  timing_.now = steadyNs;
  if (boost::asio::io_context* io = component.io()) {
    timing_.schedule = [io](uint64_t delayNs, std::function<void()> fire) {
      auto t = std::make_shared<boost::asio::steady_timer>(*io, std::chrono::nanoseconds(delayNs));
      t->async_wait([t, fire = std::move(fire)](const boost::system::error_code& ec) {
        if (!ec) fire();
      });
    };
  }
}

std::unique_ptr<OperatorState> SweepOperator::makeState() const {
  return std::make_unique<SweepState>();
}

std::size_t SweepOperator::openSymbols() const {
  std::lock_guard<std::mutex> lk(mu_);
  return open_.size();
}

// Under mu_.
void SweepOperator::armLocked_(uint64_t delayNs) {
  if (armed_ || !timing_.schedule) return;
  armed_ = true;
  std::weak_ptr<Life> w = life_;
  timing_.schedule(delayNs, [w, this] {
    auto life = w.lock();
    if (!life) return;
    std::lock_guard<std::mutex> lk(life->mu);
    if (life->alive) fireTimer();
  });
}

// Under the symbol's mutex.
void SweepOperator::closeAll_(FlowContext& ctx) {
  auto& st = ctx.stateAs<SweepState>();
  if (!st.anyOpen()) return;
  int first = 0, second = 1;
  if (st.runs[1].open && (!st.runs[0].open || st.runs[1].seq < st.runs[0].seq)) std::swap(first, second);
  for (int i : {first, second}) {
    Run& r = st.runs[i];
    if (!r.open) continue;
    publishIfSweep(ctx, r, i == 0 ? Side::Bid : Side::Ask, minLevels_);
    r.clear();
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (open_.erase(ctx.symbol)) openCount_.fetch_sub(1, std::memory_order_release);
}

void SweepOperator::onBeforeEvent(const std::string& /*symbol*/, uint64_t tsNs) {
  if (openCount_.load(std::memory_order_acquire) == 0 || !component_) return;
  std::vector<std::string> due;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [sym, o] : open_)
      if (tsNs > o.tsNs) due.push_back(sym);
  }
  for (const auto& sym : due) {
    component_->withSymbol(sym, *this, [&](FlowContext& ctx) {
      auto& st = ctx.stateAs<SweepState>();
      if (st.anyOpen() && tsNs > st.tsNs) closeAll_(ctx);
    });
  }
}

void SweepOperator::fireTimer() {
  if (!component_) return;
  const uint64_t now = timing_.now();
  std::vector<std::string> due;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [sym, o] : open_)
      if (o.dueNs <= now) due.push_back(sym);
  }
  for (const auto& sym : due) {
    component_->withSymbol(sym, *this, [&](FlowContext& ctx) {
      auto& st = ctx.stateAs<SweepState>();
      if (st.anyOpen() && st.dueNs <= now) closeAll_(ctx);
    });
  }
  std::lock_guard<std::mutex> lk(mu_);
  armed_ = false;
  if (open_.empty()) return;
  uint64_t next = UINT64_MAX;
  for (const auto& [sym, o] : open_) next = std::min(next, o.dueNs);
  armLocked_(next > now ? next - now : 0);
}

void SweepOperator::onEvent(FlowContext& ctx) {
  const FlowEvent& ev = *ctx.event;
  auto& st = ctx.stateAs<SweepState>();
  // A different instant ends the aggressive event (also covers an earlier
  // tsNs, which onBeforeEvent leaves alone, and every receive-clock event).
  if (st.anyOpen() && (!ev.exchangeTs || ev.tsNs != st.tsNs)) closeAll_(ctx);

  Exec x;
  if (!execution(ev, x)) return;

  if (!ev.exchangeTs) {   // D6 fallback: an aggressive event of its own
    Run one;
    one.add(x);
    publishIfSweep(ctx, one, x.resting, minLevels_);
    return;
  }

  Run& r = st.runs[sideIndex(x.resting)];
  if (!r.open) {
    const bool first = !st.anyOpen();
    r.open = true;
    r.seq  = st.nextSeq++;
    if (first) {
      st.tsNs  = ev.tsNs;
      st.dueNs = timing_.now() + closeNs_;
      std::lock_guard<std::mutex> lk(mu_);
      if (open_.emplace(ctx.symbol, Open{st.tsNs, st.dueNs}).second)
        openCount_.fetch_add(1, std::memory_order_release);
      armLocked_(closeNs_);
    }
  }
  r.add(x);
}

void SweepOperator::onReset(FlowContext& ctx) {
  closeAll_(ctx);
}

} // namespace krn::flow
