#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace krn::flow {

// The `flow` config namespace (ENC-1646, SPEC 2026-11-17-order-flow-pipelines
// D7). Every key is declared here, with its default, whether or not an
// operator consumes it yet; operator tickets read their own fields.
//
//   flow.enabled            bool    true      the whole component
//   flow.sweep.minLevels    int     2         P2 (ENC-1647)
//   flow.sweep.closeMs      uint    50        P2
//   flow.refill.windowNs    uint    5000000   P3 (ENC-1648)
//   flow.absorb.minRatio    double  2.0       P3
//   flow.fleeting.maxAgeNs  uint    50000000  P4 (ENC-1649)
//   flow.fleeting.window    uint    500       P4
//   flow.large.minShares    uint    5000      P5 (ENC-1649)
//   flow.large.mult         double  10.0      P5
struct FlowConfig {
  bool        enabled          = true;
  uint32_t    sweepMinLevels   = 2;
  uint64_t    sweepCloseMs     = 50;
  uint64_t    refillWindowNs   = 5'000'000;
  double      absorbMinRatio   = 2.0;
  uint64_t    fleetingMaxAgeNs = 50'000'000;
  std::size_t fleetingWindow   = 500;
  uint64_t    largeMinShares   = 5'000;
  double      largeMult        = 10.0;
};

// The key tails the namespace claims (the part after "flow."), in D7 order.
const std::vector<std::string_view>& flowConfigKeyTails();

// Apply one `flow.<tail> = value` observation. Returns true when `tail` is a
// declared key (it is then claimed, so no unclaimed-key warning fires). A
// declared key whose value does not parse, or is out of range, keeps its
// previous value and logs `flow.config.invalid` — still claimed, because the
// key itself is not a typo.
bool applyFlowConfigKey(FlowConfig& cfg, std::string_view tail, std::string_view value);

// The target of the "flow" ConfigNamespaceRegistry reader. Values arrive
// during Config::dispatchPendingKeys(), after registerWith(), so the
// component takes a snapshot() lazily at start() (D7).
class FlowConfigSource {
public:
  bool apply(std::string_view tail, std::string_view value) {
    std::lock_guard<std::mutex> lk(mu_);
    return applyFlowConfigKey(cfg_, tail, value);
  }
  FlowConfig snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
  }

private:
  mutable std::mutex mu_;
  FlowConfig         cfg_;
};

} // namespace krn::flow
