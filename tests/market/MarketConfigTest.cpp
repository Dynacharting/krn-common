// The market keys' parse and the legacy ingress synthesis (ENC-1576). These
// are krn's ConfigTest cases that ENC-1574 retired when the keys left the
// engine, re-homed against krn::market::MarketConfig with the same names and
// the same expected values.
#include <gtest/gtest.h>

#include "krn/market/MarketConfig.hpp"
#include "krn/util/Config.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

using krn::market::MarketConfig;
using krn::util::Config;

TEST(ConfigTest, DefaultValues) {
    MarketConfig cfg;
    EXPECT_EQ(cfg.taMACD_fast, 12);
    EXPECT_EQ(cfg.taMACD_slow, 26);
    EXPECT_EQ(cfg.taBBands_n, 20);
    EXPECT_DOUBLE_EQ(cfg.taBBands_stdK, 2.0);
}

TEST(ConfigTest, SwapsSlowFastIfInverted) {
    const char* path = "test_config_swap.ini";
    {
        std::ofstream f(path);
        f << "taMACD_fast=30\n"
          << "taMACD_slow=10\n";
    }
    MarketConfig cfg;
    cfg.loadFromFile(path);
    EXPECT_LE(cfg.taMACD_fast, cfg.taMACD_slow);
    EXPECT_EQ(cfg.taMACD_fast, 10);
    EXPECT_EQ(cfg.taMACD_slow, 30);
    std::remove(path);
}

TEST(ConfigTest, NewTAFieldsParsing) {
    const char* path = "test_config_ta.ini";
    {
        std::ofstream f(path);
        f << "taSMA=3,7,15\n"
          << "taEMA=5,10\n"
          << "taRSI=7\n"
          << "taATR=10\n"
          << "taMomentum=5\n"
          << "taMACD_signal=7\n"
          << "taVolAvg=10\n";
    }
    MarketConfig cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taSMA, (std::vector<int>{3, 7, 15}));
    EXPECT_EQ(cfg.taEMA, (std::vector<int>{5, 10}));
    EXPECT_EQ(cfg.taRSI, 7);
    EXPECT_EQ(cfg.taATR, 10);
    EXPECT_EQ(cfg.taMomentum, 5);
    EXPECT_EQ(cfg.taMACD_signal, 7);
    EXPECT_EQ(cfg.taVolAvg, 10);
    std::remove(path);
}

TEST(ConfigTest, DefaultNewTAFields) {
    MarketConfig cfg;
    EXPECT_EQ(cfg.taSMA, (std::vector<int>{5, 20}));
    EXPECT_EQ(cfg.taEMA, (std::vector<int>{12, 26}));
    EXPECT_EQ(cfg.taRSI, 14);
    EXPECT_EQ(cfg.taATR, 14);
    EXPECT_EQ(cfg.taMomentum, 10);
    EXPECT_EQ(cfg.taMACD_signal, 9);
    EXPECT_EQ(cfg.taVolAvg, 20);
}

// ENC-1331 — `krn_server <ws> <conf> <feed>` must BIND <feed>. The connector
// binds `ingress[].params["port"]`, so these assert the resolved params.

TEST(ConfigTest, LoadFromFileDoesNotSynthesizeIngress) {
    const char* path = "test_config_enc1331_nosynth.ini";
    {
        std::ofstream f(path);
        f << "wsPort=8180\n"
          << "feedPort=9101\n";
    }
    Config cfg;
    MarketConfig market;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_TRUE(market.loadFromFile(path));
    EXPECT_EQ(market.feedPort, 9101);
    EXPECT_TRUE(cfg.ingress.empty())
        << "loading must not synthesize ingress — it would freeze the "
           "file's feedPort into the params that bind the socket";
    std::remove(path);
}

TEST(ConfigTest, FeedPortOverrideAfterLoadReachesIngressParams) {
    const char* path = "test_config_enc1331_override.ini";
    {
        std::ofstream f(path);
        f << "wsPort=8180\n"
          << "feedPort=9101\n";
    }
    Config cfg;
    MarketConfig market;
    ASSERT_TRUE(cfg.loadFromFile(path));
    ASSERT_TRUE(market.loadFromFile(path));
    std::remove(path);

    // What src/main.cpp does with argv[3], in the order it does it.
    market.feedPort = 19191;
    EXPECT_TRUE(market.synthesizeIngressFromLegacy(cfg));

    ASSERT_FALSE(cfg.ingress.empty());
    EXPECT_EQ(cfg.ingress[0].kind, "market.feedserver");
    ASSERT_EQ(cfg.ingress[0].params.count("port"), 1u);
    EXPECT_EQ(cfg.ingress[0].params.at("port"), "19191")
        << "the ingress entry that binds the feed socket must carry the "
           "overridden port, not the config file's";
}

TEST(ConfigTest, SynthesizeIsANoOpAndReportsItWhenIngressIsExplicit) {
    const char* path = "test_config_enc1331_explicit.ini";
    {
        std::ofstream f(path);
        f << "feedPort=9101\n"
          << "ingress.0.kind=market.feedserver\n"
          << "ingress.0.port=9300\n";
    }
    Config cfg;
    MarketConfig market;
    ASSERT_TRUE(cfg.loadFromFile(path));
    ASSERT_TRUE(market.loadFromFile(path));
    std::remove(path);

    market.feedPort = 19191;  // as if argv[3] had been passed
    EXPECT_FALSE(market.synthesizeIngressFromLegacy(cfg));
    ASSERT_EQ(cfg.ingress.size(), 1u);
    EXPECT_EQ(cfg.ingress[0].params.at("port"), "9300");
}

TEST(ConfigTest, SynthesizeWithoutAnyFileUsesCompiledFeedPort) {
    Config cfg;           // no loadFromFile at all — the `./krn_server` path
    MarketConfig market;
    EXPECT_TRUE(market.synthesizeIngressFromLegacy(cfg));
    ASSERT_EQ(cfg.ingress.size(), 1u);
    EXPECT_EQ(cfg.ingress[0].kind, "market.feedserver");
    EXPECT_EQ(cfg.ingress[0].params.at("port"), "9001");
}
