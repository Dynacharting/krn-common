#pragma once

#include <string>
#include <vector>

#include "krn/util/Config.hpp"

namespace krn::market {

// The market connector's own config keys (ENC-1576). krn's util::Config
// parsed these until ENC-1574 removed them from the engine; the fields,
// defaults and parse rules below are krn's at ca544ec, unchanged. They are
// read from the same file the engine loads (argv[2]); the engine parks them
// and never reads them.
struct MarketConfig {
  // TA params
  int    taMACD_fast  = 12;   // fast EMA period
  int    taMACD_slow  = 26;   // slow EMA period
  int    taBBands_n   = 20;   // lookback
  double taBBands_stdK = 2.0; // width in standard deviations

  std::vector<int> taSMA = {5, 20};
  std::vector<int> taEMA = {12, 26};
  int taRSI = 14;
  int taATR = 14;
  int taMomentum = 10;
  int taMACD_signal = 9;
  int taVolAvg = 20;

  // The TCP FeedServer port (market.feedserver).
  int feedPort = 9001;

  // External WebSocket feed URL (empty = disabled).
  std::string feedUrl;
  // Symbols to subscribe to on the feed (empty or ["*"] = all).
  std::vector<std::string> feedSymbols = {"*"};

  // Per-feed configuration (feed.N.url / adapter / symbols).
  struct FeedConfig {
    std::string url;
    std::string adapter = "itch";
    std::vector<std::string> symbols = {"*"};
  };
  std::vector<FeedConfig> feeds;

  // Reads the market keys from a key=value file; every other key is ignored.
  // Returns false if the file cannot be opened.
  bool loadFromFile(const std::string& path);

  // When cfg.ingress is empty, appends the entries the legacy keys describe:
  // market.feedserver on feedPort, then a market.wsclient for feedUrl and for
  // each feed.N with a url. Returns false (and changes nothing) when cfg
  // already has ingress entries.
  bool synthesizeIngressFromLegacy(util::Config& cfg) const;
};

} // namespace krn::market
