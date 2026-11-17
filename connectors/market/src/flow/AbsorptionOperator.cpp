#include "krn/flow/AbsorptionOperator.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace krn::flow {

const std::string AbsorptionOperator::kBidVolume = "flow.absorb.bid.volume";
const std::string AbsorptionOperator::kBidRatio  = "flow.absorb.bid.ratio";
const std::string AbsorptionOperator::kBidPx     = "flow.absorb.bid.px";
const std::string AbsorptionOperator::kAskVolume = "flow.absorb.ask.volume";
const std::string AbsorptionOperator::kAskRatio  = "flow.absorb.ask.ratio";
const std::string AbsorptionOperator::kAskPx     = "flow.absorb.ask.px";
const std::string AbsorptionOperator::kRefillBid = "flow.refill.bid";
const std::string AbsorptionOperator::kRefillAsk = "flow.refill.ask";

namespace {

// One touch tenure.
struct Tenure {
  bool     active = false;
  Price    price{};
  double   priceD = 0.0;
  uint64_t d0     = 0;       // displayed size when the tenure began
  uint64_t e      = 0;       // executed volume so far (displayed + hidden)
  bool     fired  = false;   // absorption already published this tenure
  bool     haveExec   = false;   // a qualifying execution opened a refill window
  uint64_t lastExecTs = 0;
};

// Tenures that left the touch within their grace: a level that empties or
// is stepped behind may come back at its price within windowNs of its last
// per-order execution (a lone reserve order reloading), and then continues.
// At most a handful live at once (5 ms by default), so a small vector.
constexpr std::size_t kMaxSuspended = 4;

struct SideState {
  Tenure              cur;
  std::vector<Tenure> suspended;
};

struct AbsorbState final : OperatorState {
  SideState bid, ask;
  SideState& side(Side s) { return s == Side::Bid ? bid : ask; }
};

constexpr int idx(Side s) { return s == Side::Bid ? 0 : 1; }

const TobLevel& level(const TopOfBook& t, Side s) { return s == Side::Bid ? t.bid : t.ask; }

bool better(Side s, Price a, Price b) {
  return s == Side::Bid ? a.ticks > b.ticks : a.ticks < b.ticks;
}

bool inWindow(const Tenure& t, uint64_t ts, uint64_t windowNs) {
  return t.haveExec && ts >= t.lastExecTs && ts - t.lastExecTs <= windowNs;
}

// The touch's size decrease on side `s` (P1's rule): the whole touch when the
// level went or a worse one replaced it; zero when a better level opened.
uint64_t touchDecrease(const TobChange& tc, Side s) {
  const TobLevel& a = level(tc.pre, s);
  const TobLevel& b = level(tc.post, s);
  if (!a.present) return 0;
  if (b.present && b.price == a.price) return b.size < a.size ? a.size - b.size : 0;
  const bool worse = !b.present ||
                     (s == Side::Bid ? b.price.ticks < a.price.ticks : b.price.ticks > a.price.ticks);
  return worse ? a.size : 0;
}

// Is `px` (the feed's price) the price of touch level `l`? Compared in ticks,
// with the tick recovered from the level itself (priceD = ticks x tickSize).
bool priceIs(const TobLevel& l, double px) {
  if (!l.present) return false;
  if (l.price.ticks == 0) return std::fabs(px - l.priceD) < 1e-9;
  const double tick = l.priceD / static_cast<double>(l.price.ticks);
  if (!(tick > 0.0)) return false;
  return std::llround(px / tick) == l.price.ticks;
}

// The side whose touch absorbed a hidden print at `px`: by price, and by the
// aggressor only when the book is locked (both touches at `px`).
std::optional<Side> hiddenPrintSide(const TopOfBook& tob, double px, Aggressor aggr) {
  const bool atBid = priceIs(tob.bid, px);
  const bool atAsk = priceIs(tob.ask, px);
  if (atBid && !atAsk) return Side::Bid;
  if (atAsk && !atBid) return Side::Ask;
  if (atBid && atAsk) {
    if (aggr == Aggressor::Sell) return Side::Bid;
    if (aggr == Aggressor::Buy)  return Side::Ask;
  }
  return std::nullopt;
}

} // namespace

// The side's touch is now `lvl` at time `ts`. Keeps the current tenure when
// the price is unchanged; otherwise ends it (suspending it when it has a
// grace window), drops suspended tenures that expired or that a better touch
// stepped ahead of, and resumes one at `lvl`'s price. With `start`, a touch
// that resumes nothing begins a new tenure (D0 = lvl.size). Returns true when
// a suspended tenure resumed.
static bool transition(SideState& ss, Side s, const TobLevel& lvl, uint64_t ts,
                       uint64_t windowNs, bool start) {
  Tenure& t = ss.cur;
  if (t.active && lvl.present && t.price == lvl.price) return false;
  std::optional<Tenure> ending;
  if (t.active && t.haveExec) ending = t;
  t = Tenure{};

  auto& sus = ss.suspended;
  for (auto it = sus.begin(); it != sus.end();) {
    const bool gone = !inWindow(*it, ts, windowNs) ||
                      (lvl.present && better(s, lvl.price, it->price));
    it = gone ? sus.erase(it) : it + 1;
  }
  bool resumed = false;
  if (lvl.present) {
    for (auto it = sus.begin(); it != sus.end(); ++it) {
      if (it->price == lvl.price) {
        t = *it;
        sus.erase(it);
        resumed = true;
        break;
      }
    }
    if (!resumed && start) {
      t.active = true;
      t.price  = lvl.price;
      t.priceD = lvl.priceD;
      t.d0     = lvl.size;
    }
  }
  if (ending && inWindow(*ending, ts, windowNs)) {
    if (sus.size() >= kMaxSuspended) sus.erase(sus.begin());
    sus.push_back(*ending);
  }
  return resumed;
}

std::vector<FlowKey> AbsorptionOperator::keys() const {
  return {
    {kBidVolume, Publication::Pushed}, {kBidRatio, Publication::Pushed},
    {kBidPx, Publication::Pushed},     {kAskVolume, Publication::Pushed},
    {kAskRatio, Publication::Pushed},  {kAskPx, Publication::Pushed},
    {kRefillBid, Publication::Pushed}, {kRefillAsk, Publication::Pushed},
  };
}

void AbsorptionOperator::configure(const FlowConfig& cfg, FlowComponent& /*component*/) {
  windowNs_ = cfg.refillWindowNs;
  minRatio_ = cfg.absorbMinRatio;
}

std::unique_ptr<OperatorState> AbsorptionOperator::makeState() const {
  return std::make_unique<AbsorbState>();
}

void AbsorptionOperator::onReset(FlowContext& ctx) {
  auto& st = ctx.stateAs<AbsorbState>();
  st.bid = SideState{};
  st.ask = SideState{};
}

void AbsorptionOperator::onEvent(FlowContext& ctx) {
  const FlowEvent& ev = *ctx.event;
  // An observation older than one already seen (two ingress strands on one
  // symbol, review F1): its TobChange describes a book that has since moved,
  // so syncing the tenure to it would end and restart the level spuriously.
  // It is counted in flow.meta.reordered and skipped here.
  if (ev.reordered) return;
  const TobChange& tc = ev.tob();
  auto& st = ctx.stateAs<AbsorbState>();
  for (Side s : {Side::Bid, Side::Ask})
    transition(st.side(s), s, level(tc.pre, s), ev.tsNs, windowNs_, /*start=*/true);

  uint64_t executed[2] = {0, 0};
  bool     qualifies[2] = {false, false};   // opens a refill window
  uint64_t refill[2] = {0, 0};

  switch (ev.kind) {
    case FlowEventKind::Trade: {
      if (ev.bookNeutral) {
        // A print with a resting order is the print of a displayed
        // execution: its Execution mutation carries the shares.
        if (ev.orderId != 0) break;
        const auto* t = std::get_if<feed::ObTradeEvent>(&ev.obs.event);
        if (!t || t->size == 0) break;
        // bookNeutral: pre == post == the book this print traded against.
        if (auto s = hiddenPrintSide(tc.pre, t->price, t->aggressor))
          executed[idx(*s)] += t->size;
      } else if (ev.unattributed) {
        for (Side s : {Side::Bid, Side::Ask}) executed[idx(s)] += touchDecrease(tc, s);
      }
      break;
    }
    case FlowEventKind::Add: {
      if (!ev.after || ev.cause == feed::MutationCause::Replace || ev.duplicateAdd) break;
      const ShadowOrder& o = *ev.after;
      SideState& ss = st.side(o.side);
      // At the touch, or reloading a suspended level at its own price (the
      // add re-establishes it; transition() below resumes the tenure).
      const Tenure* t = (ss.cur.active && ss.cur.price == o.price) ? &ss.cur : nullptr;
      if (!t)
        for (const Tenure& s : ss.suspended)
          if (s.price == o.price) { t = &s; break; }
      if (t && inWindow(*t, ev.tsNs, windowNs_)) refill[idx(o.side)] += o.size;
      break;
    }
    case FlowEventKind::Update:
    case FlowEventKind::Delete: {
      if (ev.cause != feed::MutationCause::Execution) break;
      if (ev.orderUnknown || !ev.before) {
        for (Side s : {Side::Bid, Side::Ask}) {
          const uint64_t d = touchDecrease(tc, s);
          executed[idx(s)] += d;
          if (d) qualifies[idx(s)] = true;
        }
        break;
      }
      const ShadowOrder& b = *ev.before;
      const Tenure& t = st.side(b.side).cur;
      if (!t.active || t.price != b.price) break;   // not at the touch
      uint64_t gone = b.size;
      if (ev.kind == FlowEventKind::Update && ev.after && ev.after->price == b.price)
        gone = ev.after->size < b.size ? b.size - ev.after->size : 0;
      executed[idx(b.side)] += gone;
      if (gone) qualifies[idx(b.side)] = true;
      break;
    }
  }

  for (Side s : {Side::Bid, Side::Ask}) {
    SideState& ss = st.side(s);
    const bool bid = s == Side::Bid;
    if (refill[idx(s)]) ctx.pub.increment(ctx.symbol, bid ? kRefillBid : kRefillAsk,
                                          static_cast<double>(refill[idx(s)]));
    auto maybeFire = [&](Tenure& t) {
      if (t.fired || t.d0 == 0 ||
          static_cast<double>(t.e) < minRatio_ * static_cast<double>(t.d0)) return;
      t.fired = true;
      ctx.pub.increment(ctx.symbol, bid ? kBidVolume : kAskVolume, static_cast<double>(t.e));
      ctx.pub.increment(ctx.symbol, bid ? kBidRatio : kAskRatio,
                        static_cast<double>(t.e) / static_cast<double>(t.d0));
      ctx.pub.increment(ctx.symbol, bid ? kBidPx : kAskPx, t.priceD);
    };
    const TobLevel& post = level(tc.post, s);
    Tenure& t = ss.cur;
    const bool holds = t.active && post.present && post.price == t.price;
    if (t.active && executed[idx(s)]) {
      t.e += executed[idx(s)];
      if (qualifies[idx(s)]) { t.haveExec = true; t.lastExecTs = ev.tsNs; }
      if (holds) maybeFire(t);
    }
    // The touch moved or emptied: the tenure ends here — or, back at a
    // suspended level's price within its grace, that tenure resumes, and
    // the level holds again, so it may fire now.
    if (!holds && transition(ss, s, post, ev.tsNs, windowNs_, /*start=*/false)) maybeFire(ss.cur);
  }
}

} // namespace krn::flow
