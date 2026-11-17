#include "krn/flow/AbsorptionOperator.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>

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

// One side's touch tenure.
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

struct AbsorbState final : OperatorState {
  Tenure bid, ask;
  Tenure& side(Side s) { return s == Side::Bid ? bid : ask; }
};

constexpr int idx(Side s) { return s == Side::Bid ? 0 : 1; }

const TobLevel& level(const TopOfBook& t, Side s) { return s == Side::Bid ? t.bid : t.ask; }

// Start a tenure when the side's touch is at a new price; clear it when the
// side has no touch.
void sync(Tenure& t, const TobLevel& pre) {
  if (!pre.present) { t = Tenure{}; return; }
  if (t.active && t.price == pre.price) return;
  t        = Tenure{};
  t.active = true;
  t.price  = pre.price;
  t.priceD = pre.priceD;
  t.d0     = pre.size;
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
  st.bid = Tenure{};
  st.ask = Tenure{};
}

void AbsorptionOperator::onEvent(FlowContext& ctx) {
  const FlowEvent& ev = *ctx.event;
  const TobChange& tc = ev.tob();
  auto& st = ctx.stateAs<AbsorbState>();
  for (Side s : {Side::Bid, Side::Ask}) sync(st.side(s), level(tc.pre, s));

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
      if (!ev.after || ev.cause == feed::MutationCause::Replace) break;
      const ShadowOrder& o = *ev.after;
      const Tenure& t = st.side(o.side);
      if (t.active && t.price == o.price && t.haveExec && ev.tsNs >= t.lastExecTs &&
          ev.tsNs - t.lastExecTs <= windowNs_)
        refill[idx(o.side)] += o.size;
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
      const Tenure& t = st.side(b.side);
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
    Tenure& t = st.side(s);
    const bool bid = s == Side::Bid;
    if (refill[idx(s)]) ctx.pub.increment(ctx.symbol, bid ? kRefillBid : kRefillAsk,
                                          static_cast<double>(refill[idx(s)]));
    if (!t.active) continue;
    const TobLevel& post = level(tc.post, s);
    const bool holds = post.present && post.price == t.price;
    if (executed[idx(s)]) {
      t.e += executed[idx(s)];
      if (qualifies[idx(s)]) { t.haveExec = true; t.lastExecTs = ev.tsNs; }
      if (!t.fired && holds && t.d0 > 0 &&
          static_cast<double>(t.e) >= minRatio_ * static_cast<double>(t.d0)) {
        t.fired = true;
        ctx.pub.increment(ctx.symbol, bid ? kBidVolume : kAskVolume, static_cast<double>(t.e));
        ctx.pub.increment(ctx.symbol, bid ? kBidRatio : kAskRatio,
                          static_cast<double>(t.e) / static_cast<double>(t.d0));
        ctx.pub.increment(ctx.symbol, bid ? kBidPx : kAskPx, t.priceD);
      }
    }
    if (!holds) t = Tenure{};   // the touch moved or emptied: the tenure ends here
  }
}

} // namespace krn::flow
