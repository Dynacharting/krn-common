#pragma once

#include <string>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// P0 — order-flow imbalance (SPEC 2026-11-17-order-flow-pipelines D5).
// Cont, Kukanov & Stoikov (2014) top-of-book OFI, one term per observed
// event, from TobChange (primes = post):
//
//   e = 1{Pb' >= Pb} qb' - 1{Pb' <= Pb} qb - 1{Pa' <= Pa} qa' + 1{Pa' >= Pa} qa
//
// Prices compare as tick indices. A side that is absent before OR after the
// event contributes no term (the formula needs a price on both sides of the
// comparison; an empty side has none). So the first add into an empty bid
// gives e = 0, and the delete that empties a side gives e = 0 for that side.
//
//   flow.ofi      Pushed  e, when non-zero
//   flow.ofi.cum  Stored  running sum of e for the symbol (kept across book resets)
class OfiOperator final : public IFlowOperator {
public:
  static const std::string kOfi;
  static const std::string kOfiCum;

  // The CKS term for one TobChange (exposed for tests).
  static double term(const TobChange& tob);

  std::string_view name() const override { return "ofi"; }
  std::vector<FlowKey> keys() const override;
  std::unique_ptr<OperatorState> makeState() const override;
  void onEvent(FlowContext& ctx) override;
};

} // namespace krn::flow
