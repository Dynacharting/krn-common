#include "krn/flow/OfiOperator.hpp"

namespace krn::flow {

const std::string OfiOperator::kOfi    = "flow.ofi";
const std::string OfiOperator::kOfiCum = "flow.ofi.cum";

namespace {
struct OfiState final : OperatorState {
  double cum = 0.0;
};
} // namespace

double OfiOperator::term(const TobChange& tob) {
  // CKS limiting case: an absent bid is -inf and an absent ask +inf, so each
  // indicator is decided by presence when a side is empty before or after.
  double e = 0.0;
  const TobLevel& b0 = tob.pre.bid;
  const TobLevel& b1 = tob.post.bid;
  const bool bidUp   = b1.present && (!b0.present || b1.price.ticks >= b0.price.ticks);   // Pb' >= Pb
  const bool bidDown = b0.present && (!b1.present || b1.price.ticks <= b0.price.ticks);   // Pb' <= Pb
  if (bidUp)   e += static_cast<double>(b1.size);
  if (bidDown) e -= static_cast<double>(b0.size);
  const TobLevel& a0 = tob.pre.ask;
  const TobLevel& a1 = tob.post.ask;
  const bool askDown = a1.present && (!a0.present || a1.price.ticks <= a0.price.ticks);   // Pa' <= Pa
  const bool askUp   = a0.present && (!a1.present || a1.price.ticks >= a0.price.ticks);   // Pa' >= Pa
  if (askDown) e -= static_cast<double>(a1.size);
  if (askUp)   e += static_cast<double>(a0.size);
  return e;
}

std::vector<FlowKey> OfiOperator::keys() const {
  return {{kOfi, Publication::Pushed}, {kOfiCum, Publication::Stored}};
}

std::unique_ptr<OperatorState> OfiOperator::makeState() const {
  return std::make_unique<OfiState>();
}

void OfiOperator::onEvent(FlowContext& ctx) {
  const double e = term(ctx.event->tob());
  if (e == 0.0) return;
  auto& st = ctx.stateAs<OfiState>();
  st.cum += e;
  ctx.pub.increment(ctx.symbol, kOfi, e);
  ctx.pub.state(ctx.symbol, kOfiCum, st.cum);
}

} // namespace krn::flow
