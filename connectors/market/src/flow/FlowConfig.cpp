#include "krn/flow/FlowConfig.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

#include "krn/util/Logger.hpp"

namespace krn::flow {

namespace {

bool parseBool(std::string_view v, bool& out) {
  if (v == "true" || v == "1" || v == "yes" || v == "on")   { out = true;  return true; }
  if (v == "false" || v == "0" || v == "no" || v == "off")  { out = false; return true; }
  return false;
}

template <class U>
bool parseUnsigned(std::string_view v, U& out, U min) {
  U x{};
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
  if (ec != std::errc{} || p != v.data() + v.size() || x < min) return false;
  out = x;
  return true;
}

bool parsePositiveDouble(std::string_view v, double& out) {
  std::string s(v);
  char* end = nullptr;
  const double x = std::strtod(s.c_str(), &end);
  if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(x) || x <= 0.0) return false;
  out = x;
  return true;
}

} // namespace

const std::vector<std::string_view>& flowConfigKeyTails() {
  static const std::vector<std::string_view> k{
    "enabled", "sweep.minLevels", "sweep.closeMs", "refill.windowNs", "absorb.minRatio",
    "fleeting.maxAgeNs", "fleeting.window", "large.minShares", "large.mult",
  };
  return k;
}

bool applyFlowConfigKey(FlowConfig& c, std::string_view tail, std::string_view value) {
  bool ok = false;
  if      (tail == "enabled")           ok = parseBool(value, c.enabled);
  else if (tail == "sweep.minLevels")   ok = parseUnsigned<uint32_t>(value, c.sweepMinLevels, 1);
  else if (tail == "sweep.closeMs")     ok = parseUnsigned<uint64_t>(value, c.sweepCloseMs, 1);
  else if (tail == "refill.windowNs")   ok = parseUnsigned<uint64_t>(value, c.refillWindowNs, 0);
  else if (tail == "absorb.minRatio")   ok = parsePositiveDouble(value, c.absorbMinRatio);
  else if (tail == "fleeting.maxAgeNs") ok = parseUnsigned<uint64_t>(value, c.fleetingMaxAgeNs, 0);
  else if (tail == "fleeting.window")   ok = parseUnsigned<std::size_t>(value, c.fleetingWindow, 1);
  else if (tail == "large.minShares")   ok = parseUnsigned<uint64_t>(value, c.largeMinShares, 0);
  else if (tail == "large.mult")        ok = parsePositiveDouble(value, c.largeMult);
  else return false;   // not a flow key: let the engine warn (ENC-1595)

  if (!ok) {
    util::logger().log(util::LogLevel::Warn, "flow.config.invalid",
                       {{"key", "flow." + std::string(tail)}, {"value", std::string(value)}});
  }
  return true;
}

} // namespace krn::flow
