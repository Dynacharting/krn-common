// The V1 flow operator set (SPEC 2026-11-17-order-flow-pipelines D5). Adding
// an operator is one include and one push_back, each in its own slot below:
// the slots are separated by unchanged lines so parallel operator branches
// (ENC-1647, ENC-1648, ENC-1649) merge without touching each other's hunk.
#include "krn/flow/FlowOperators.hpp"

#include "krn/flow/OfiOperator.hpp"
#include "krn/flow/TouchDepletionOperator.hpp"

// P2 sweeps (ENC-1647): include here.

// P3 absorption and refill (ENC-1648): include here.

// P4 fleeting rate / P5 large-order lifecycle (ENC-1649): include here.
#include "krn/flow/FleetingOperator.hpp"
#include "krn/flow/LargeOrderOperator.hpp"

namespace krn::flow {

std::vector<std::unique_ptr<IFlowOperator>> makeFlowOperators() {
  std::vector<std::unique_ptr<IFlowOperator>> ops;
  ops.push_back(std::make_unique<OfiOperator>());              // P0
  ops.push_back(std::make_unique<TouchDepletionOperator>());   // P1

  // P2 (ENC-1647): register here.

  // P3 (ENC-1648): register here.

  // P4 / P5 (ENC-1649): register here.
  ops.push_back(std::make_unique<FleetingOperator>());         // P4
  ops.push_back(std::make_unique<LargeOrderOperator>());       // P5

  return ops;
}

} // namespace krn::flow
