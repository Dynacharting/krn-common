#pragma once

#include <memory>
#include <vector>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// The V1 operator set, in run order (src/flow/FlowOperators.cpp — one line
// per operator).
std::vector<std::unique_ptr<IFlowOperator>> makeFlowOperators();

} // namespace krn::flow
