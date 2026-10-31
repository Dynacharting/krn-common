// File: src/main.cpp — krn_server: krn_host + the market connector (ENC-1559,
// purity SPEC D6). The engine composition is in src/host/Host.cpp; this file
// holds only what belongs to the market connector: its config keys (read from
// argv[2] here, since the engine no longer parses them; ENC-1576), the
// feedPort argument (argv[3]), the legacy feed-key ingress synthesis, and the
// feedPort fields on the boot and listening log lines.
//
// CLI: krn_server [wsPort] [configFile] [feedPort]
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "krn/host/Host.hpp"
#include "krn/market/MarketConfig.hpp"
#include "krn/market/MarketConnector.hpp"
#include "krn/util/Config.hpp"
#include "krn/util/Logger.hpp"

static unsigned short parsePort(const char* str, unsigned short fallback) {
  try {
    unsigned long p = std::stoul(str);
    if (p == 0 || p > 65535) return fallback;
    return static_cast<unsigned short>(p);
  } catch (...) {
    return fallback;
  }
}

int main(int argc, char* argv[]) {
  using namespace krn::util;

  // The market keys, from the same file the engine loads (argv[2]). Read in
  // applyExtraArgs, before the connector is constructed.
  auto market = std::make_shared<krn::market::MarketConfig>();

  krn::host::HostOptions opts;
  opts.programName = "krn_server";
  opts.connectors.push_back([market] {
    return std::make_unique<krn::market::MarketConnector>(*market);
  });

  // argv[3] = feedPort. CLI args win over the config file.
  opts.hooks.applyExtraArgs = [market](int argc, char* argv[], Config& /*cfg*/) {
    if (argc > 2) market->loadFromFile(argv[2]);  // the engine reports a load failure
    if (market->feedPort <= 0 || market->feedPort > 65535) {
      std::cerr << "[config] warning: invalid feedPort=" << market->feedPort << ", using default 9001\n";
      market->feedPort = 9001;
    }
    unsigned short feedPort = static_cast<unsigned short>(market->feedPort);
    if (argc > 3) feedPort = parsePort(argv[3], feedPort);
    // ENC-1331: this must stay AHEAD of synthesizeIngressFromLegacy()
    // below, which copies feedPort into the market.feedserver ingress
    // params that actually bind the socket.

  };

  opts.hooks.bootFields = [market](const Config& /*cfg*/, std::vector<Field>& fields) {
    fields.push_back({"feedPort", std::to_string(market->feedPort)});
  };

  // ENC-31: ensure cfg.ingress[] is populated even when the user provided
  // no INI (loadFromFile not called). Idempotent.
  //
  // ENC-1331: this is the ONLY caller. It runs here, after the argv overrides
  // above have settled feedPort, so the synthesized market.feedserver entry
  // carries the port that was actually asked for. Config::loadFromFile() used
  // to call it too, which froze the file's feedPort into the ingress params
  // while the boot log printed argv[3] — the two disagreed and only the bind
  // was true.
  opts.hooks.afterConnectorsRegistered = [market](int argc, char* /*argv*/[], Config& cfg) {
    const bool ingressSynthesized = market->synthesizeIngressFromLegacy(cfg);
    if (argc > 3 && !ingressSynthesized) {
      // The config file declared explicit `ingress.N.*` entries, which are more
      // specific than the legacy feedPort key and are left alone. Say so, rather
      // than silently binding a port the user did not pass.
      logger().log(LogLevel::Warn, "config.feedport_arg_ignored",
                   {{"feedPort", std::to_string(market->feedPort)},
                    {"reason", "config file declares explicit ingress.N.* entries"},
                    {"fix", "set ingress.N.port in the config file instead"}});
    }
  };

  opts.hooks.ingressStartFailHint =
      "        (is another krn_server already listening there? "
      "pass a different feedPort as argv[3] / ingress.N.port)";

  // ENC-1331: report the port that was actually RESOLVED into the ingress entry
  // the acceptor was opened from, not the intent in feedPort. A log that
  // printed the intent is precisely what hid this bug: it showed argv[3] while
  // the socket carried the config file's port, and only the bind was true.
  opts.hooks.listeningFields = [](const Config& cfg, std::vector<Field>& fields) {
    std::string boundFeedPort = "none";
    for (const auto& entry : cfg.ingress) {
      if (entry.kind != "market.feedserver") continue;
      auto it = entry.params.find("port");
      if (it != entry.params.end()) boundFeedPort = it->second;
      break;
    }
    fields.push_back({"feedPort", boundFeedPort});
  };

  return krn::host::hostMain(argc, argv, opts);
}
