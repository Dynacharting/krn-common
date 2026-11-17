// ENC-1646 (SPEC 2026-11-17-order-flow-pipelines D7): the `flow` config
// namespace declares every D7 key with its default and parses each.
#include "krn/flow/FlowComponent.hpp"
#include "krn/flow/FlowConfig.hpp"

#include "krn/AtomicStore.hpp"
#include "krn/Dispatcher.hpp"
#include "krn/FunctionMap.hpp"
#include "krn/atomic/AtomicProviderRegistry.hpp"
#include "krn/engine/ConfigNamespaceRegistry.hpp"
#include "krn/engine/Registries.hpp"
#include "krn/market/MarketConnector.hpp"
#include "krn/rt/ThreadPool.hpp"
#include "krn/runtime/ShutdownCoordinator.hpp"
#include "krn/util/Config.hpp"
#include "krn/util/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <cstdlib>

using namespace krn::flow;

TEST(FlowConfigTest, DeclaresEveryD7KeyWithItsDefault) {
  const auto& tails = flowConfigKeyTails();
  EXPECT_EQ(tails.size(), 9u);
  FlowConfig c;
  EXPECT_TRUE(c.enabled);
  EXPECT_EQ(c.sweepMinLevels, 2u);
  EXPECT_EQ(c.sweepCloseMs, 50u);
  EXPECT_EQ(c.refillWindowNs, 5'000'000u);
  EXPECT_EQ(c.absorbMinRatio, 2.0);
  EXPECT_EQ(c.fleetingMaxAgeNs, 50'000'000u);
  EXPECT_EQ(c.fleetingWindow, 500u);
  EXPECT_EQ(c.largeMinShares, 5'000u);
  EXPECT_EQ(c.largeMult, 10.0);
}

TEST(FlowConfigTest, ParsesEveryKey) {
  FlowConfig c;
  EXPECT_TRUE(applyFlowConfigKey(c, "enabled", "false"));
  EXPECT_TRUE(applyFlowConfigKey(c, "sweep.minLevels", "3"));
  EXPECT_TRUE(applyFlowConfigKey(c, "sweep.closeMs", "75"));
  EXPECT_TRUE(applyFlowConfigKey(c, "refill.windowNs", "1000"));
  EXPECT_TRUE(applyFlowConfigKey(c, "absorb.minRatio", "1.5"));
  EXPECT_TRUE(applyFlowConfigKey(c, "fleeting.maxAgeNs", "2000"));
  EXPECT_TRUE(applyFlowConfigKey(c, "fleeting.window", "100"));
  EXPECT_TRUE(applyFlowConfigKey(c, "large.minShares", "777"));
  EXPECT_TRUE(applyFlowConfigKey(c, "large.mult", "4"));
  EXPECT_FALSE(c.enabled);
  EXPECT_EQ(c.sweepMinLevels, 3u);
  EXPECT_EQ(c.sweepCloseMs, 75u);
  EXPECT_EQ(c.refillWindowNs, 1000u);
  EXPECT_EQ(c.absorbMinRatio, 1.5);
  EXPECT_EQ(c.fleetingMaxAgeNs, 2000u);
  EXPECT_EQ(c.fleetingWindow, 100u);
  EXPECT_EQ(c.largeMinShares, 777u);
  EXPECT_EQ(c.largeMult, 4.0);
}

TEST(FlowConfigTest, InvalidValueIsClaimedAndKeepsTheDefault) {
  FlowConfig c;
  EXPECT_TRUE(applyFlowConfigKey(c, "sweep.minLevels", "0"));
  EXPECT_TRUE(applyFlowConfigKey(c, "fleeting.window", "-4"));
  EXPECT_TRUE(applyFlowConfigKey(c, "large.mult", "abc"));
  EXPECT_TRUE(applyFlowConfigKey(c, "enabled", "maybe"));
  EXPECT_EQ(c.sweepMinLevels, 2u);
  EXPECT_EQ(c.fleetingWindow, 500u);
  EXPECT_EQ(c.largeMult, 10.0);
  EXPECT_TRUE(c.enabled);
}

TEST(FlowConfigTest, UnknownKeyIsNotClaimed) {
  FlowConfig c;
  EXPECT_FALSE(applyFlowConfigKey(c, "sweep.minLevelz", "3"));
  EXPECT_FALSE(applyFlowConfigKey(c, "ofi", "1"));
}

// D7: values arrive after registerWith, so the component reads them at start().
TEST(FlowConfigTest, ComponentSnapshotsConfigAtStart) {
  FlowComponent comp(FlowComponent::Env{});
  comp.configSource()->apply("sweep.closeMs", "120");
  EXPECT_FALSE(comp.started());
  comp.start();
  EXPECT_EQ(comp.config().sweepCloseMs, 120u);
  comp.configSource()->apply("sweep.closeMs", "999");
  comp.start();   // idempotent: the snapshot is taken once
  EXPECT_EQ(comp.config().sweepCloseMs, 120u);
}

// Review F5: ConfigNamespaceRegistry is process-global and first-wins, so a
// second MarketConnector's `flow` reader used to be dropped and its flow.*
// keys reached the first connector. Runs in a child process: registerWith
// appends to process-global registries the other tests rely on (see
// ConnectorLifecycleRealTest's note).
namespace {
[[noreturn]] void twoConnectorsInOneProcess() {
  using namespace krn;
  util::Config cfg;
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  rt::ShutdownCoordinator shutdown;
  boost::asio::io_context ioc;
  engine::EngineRegistries regs{
    &cfg, &pool, &store, &dispatcher, &shutdown, &ioc,
    &engine::EventTypeRegistry::singleton(),
    &engine::EventComputerRegistry::singleton(),
    &engine::NodeTypeRegistry::singleton(),
    &engine::IngressRegistry::singleton(),
    &engine::ConfigNamespaceRegistry::singleton(),
    &AtomicProviderRegistry::singleton(),
    &FunctionMap::instance(),
    &util::logger(),
    &engine::ProtocolIngressRegistry::singleton(),
  };
  market::MarketConnector first, second;
  first.registerWith(regs);
  first.start();
  second.registerWith(regs);
  if (!engine::ConfigNamespaceRegistry::dispatch("flow.enabled", "false")) std::_Exit(2);
  second.start();
  if (!second.flow() || second.flow()->enabled()) std::_Exit(3);   // the defect: flow.enabled went to `first`
  if (!first.flow()->enabled()) std::_Exit(4);                      // already started: its snapshot is taken
  std::_Exit(0);
}
} // namespace

TEST(FlowConfigTest, FlowConfigReachesASecondMarketConnector) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(twoConnectorsInOneProcess(), ::testing::ExitedWithCode(0), "");
}
