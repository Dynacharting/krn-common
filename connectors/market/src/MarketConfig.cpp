// The market keys' parse and the legacy ingress synthesis, moved out of krn's
// src/util/Config.cpp at ca544ec (ENC-1574 removed them there; ENC-1576). The
// line reader, trim and key=value split are that file's, so a line the engine
// reads as key=value is read the same way here.
#include "krn/market/MarketConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "krn/util/Logger.hpp"

namespace krn::market {

namespace {

std::string trim(const std::string& s) {
  const auto is_ws = [](unsigned char c){ return std::isspace(c) != 0; };
  auto b = std::find_if_not(s.begin(), s.end(), is_ws);
  auto e = std::find_if_not(s.rbegin(), s.rend(), is_ws).base();
  if (b >= e) return {};
  return std::string(b, e);
}

bool parseLineKV(const std::string& line, std::string& k, std::string& v) {
  auto pos = line.find('=');
  if (pos == std::string::npos) return false;
  k = trim(line.substr(0, pos));
  v = trim(line.substr(pos + 1));
  if (k.empty()) return false;
  return true;
}

} // namespace

bool MarketConfig::loadFromFile(const std::string& path) {
  FILE* f = nullptr;

#ifdef _WIN32
  if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
    return false;
  }
#else
  f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
#endif

  auto closeFile = [](FILE* fp) { if (fp) std::fclose(fp); };
  std::unique_ptr<FILE, decltype(closeFile)> fileGuard(f, closeFile);

  std::string line;
  line.reserve(1024);

  while (true) {
    char tmp[1024];
    if (!std::fgets(tmp, sizeof(tmp), f)) break;
    line.assign(tmp);

    // Truncated line: discard the rest of it, as the engine does.
    if (line.size() == sizeof(tmp) - 1 && !line.empty() && line.back() != '\n') {
      while (std::fgets(tmp, sizeof(tmp), f)) {
        if (std::strchr(tmp, '\n')) break;
      }
      continue;
    }

    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    auto s = trim(line);
    if (s.empty()) continue;
    if (s[0] == '#' || s[0] == ';') continue;

    std::string key, val;
    if (!parseLineKV(s, key, val)) continue;

    // NOTE: std::atoi returns 0 for non-numeric input. All period values are
    // guarded by std::max(1, ...) and the port has an explicit range check.
    if      (key == "taMACD_fast")    taMACD_fast    = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMACD_slow")    taMACD_slow    = std::max(1, std::atoi(val.c_str()));
    else if (key == "taBBands_n")     taBBands_n     = std::max(1, std::atoi(val.c_str()));
    else if (key == "taBBands_stdK")  taBBands_stdK  = std::atof(val.c_str());
    else if (key == "taRSI")          taRSI          = std::max(1, std::atoi(val.c_str()));
    else if (key == "taATR")          taATR          = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMomentum")     taMomentum     = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMACD_signal")  taMACD_signal  = std::max(1, std::atoi(val.c_str()));
    else if (key == "taVolAvg")       taVolAvg       = std::max(1, std::atoi(val.c_str()));
    else if (key == "feedPort")      { int p = std::atoi(val.c_str()); if (p > 0 && p <= 65535) feedPort = p; }
    else if (key == "allowNegativePrices") { allowNegativePrices = (val == "true" || val == "1" || val == "yes"); }
    // Multi-feed config: feed.0.url, feed.0.adapter, feed.0.symbols, etc.
    else if (key.substr(0, 5) == "feed." && key.size() > 5) {
      auto dot2 = key.find('.', 5);
      if (dot2 != std::string::npos) {
        std::string idxStr = key.substr(5, dot2 - 5);
        if (idxStr.empty() || !std::isdigit(static_cast<unsigned char>(idxStr[0]))) continue;
        int idx = std::atoi(idxStr.c_str());
        std::string field = key.substr(dot2 + 1);
        if (idx >= 0 && idx < 64) {
          while (static_cast<int>(feeds.size()) <= idx) feeds.emplace_back();
          auto& fc = feeds[static_cast<size_t>(idx)];
          if (field == "url") fc.url = val;
          else if (field == "adapter") fc.adapter = val;
          else if (field == "symbols") {
            fc.symbols.clear();
            std::istringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
              auto t = trim(tok);
              if (!t.empty()) fc.symbols.push_back(t);
            }
            if (fc.symbols.empty()) fc.symbols = {"*"};
          }
        }
      }
    }
    else if (key == "feedUrl") { feedUrl = val; }
    else if (key == "feedSymbols") {
      feedSymbols.clear();
      std::istringstream ss(val);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        auto t = trim(tok);
        if (!t.empty()) feedSymbols.push_back(t);
      }
      if (feedSymbols.empty()) feedSymbols = {"*"};
    }
    else if (key == "taSMA") {
      taSMA.clear();
      std::istringstream ss(val);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        int v = std::atoi(trim(tok).c_str());
        if (v > 0) taSMA.push_back(v);
      }
      if (taSMA.empty()) taSMA = {5, 20};
    }
    else if (key == "taEMA") {
      taEMA.clear();
      std::istringstream ss(val);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        int v = std::atoi(trim(tok).c_str());
        if (v > 0) taEMA.push_back(v);
      }
      if (taEMA.empty()) taEMA = {12, 26};
    }
  }

  // Basic sanity: slow >= fast for MACD; stdK positive.
  if (taMACD_slow < taMACD_fast) std::swap(taMACD_slow, taMACD_fast);
  if (taBBands_stdK <= 0.0) taBBands_stdK = 2.0;

  return true;
}

bool MarketConfig::synthesizeIngressFromLegacy(util::Config& cfg) const {
  if (!cfg.ingress.empty()) return false;

  util::Config::Ingress fsEntry;
  fsEntry.kind = "market.feedserver";
  fsEntry.params["port"] = std::to_string(feedPort);
  cfg.ingress.push_back(std::move(fsEntry));

  bool synthesizedWS = false;

  if (!feedUrl.empty()) {
    util::Config::Ingress wsEntry;
    wsEntry.kind = "market.wsclient";
    wsEntry.params["url"] = feedUrl;
    wsEntry.params["adapter"] = "itch";
    if (!feedSymbols.empty()) {
      std::string joined;
      for (size_t i = 0; i < feedSymbols.size(); ++i) {
        if (i) joined += ",";
        joined += feedSymbols[i];
      }
      wsEntry.params["symbols"] = joined;
    }
    cfg.ingress.push_back(std::move(wsEntry));
    synthesizedWS = true;
  }
  for (const auto& fc : feeds) {
    if (fc.url.empty()) continue;
    util::Config::Ingress wsEntry;
    wsEntry.kind = "market.wsclient";
    wsEntry.params["url"] = fc.url;
    wsEntry.params["adapter"] = fc.adapter;
    if (!fc.symbols.empty()) {
      std::string joined;
      for (size_t i = 0; i < fc.symbols.size(); ++i) {
        if (i) joined += ",";
        joined += fc.symbols[i];
      }
      wsEntry.params["symbols"] = joined;
    }
    cfg.ingress.push_back(std::move(wsEntry));
    synthesizedWS = true;
  }
  if (synthesizedWS) {
    krn::util::logger().log(
        krn::util::LogLevel::Warn,
        "config.deprecated_keys",
        {{"keys", "feedUrl/feedSymbols/feeds.N.*"},
         {"replacement", "ingress.N.kind = market.wsclient + ingress.N.url|adapter|symbols"},
         {"window", "one release"}});
  }
  return true;
}

} // namespace krn::market
