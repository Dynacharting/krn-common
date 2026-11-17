// ENC-1646 (SPEC 2026-11-17-order-flow-pipelines D7): the `flow` config
// namespace declares every D7 key with its default and parses each.
#include "krn/flow/FlowComponent.hpp"
#include "krn/flow/FlowConfig.hpp"

#include <gtest/gtest.h>

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
