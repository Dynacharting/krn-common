// What MarketConnector::registerWith declares to the engine (ENC-1576). The
// test bootstrap has already called registerWith() once, so each case reads
// the engine's registries as krn_server leaves them.
#include <gtest/gtest.h>

#include "krn/AtomicStore.hpp"
#include "krn/Dispatcher.hpp"
#include "krn/atomic/AtomicProviderRegistry.hpp"
#include "krn/engine/ProtocolIngressRegistry.hpp"
#include "krn/forum/ConnectorsClient.hpp"
#include "krn/nodes/INode.hpp"
#include "krn/nodes/Listener.hpp"
#include "krn/rt/ThreadPool.hpp"

#include <memory>
#include <string>

using namespace krn;

namespace {
struct NullNode : INode {
  void onValue(const StreamValue&) override {}
  void shutdown() noexcept override {}
};
} // namespace

TEST(MarketRegistrationTest, ObIsPipelineOnly) {
  EXPECT_TRUE(AtomicProviderRegistry::isPipelineOnlyKey("ob.best.bid.price"));
  EXPECT_FALSE(AtomicProviderRegistry::isPipelineOnlyKey("obv"));
}

TEST(MarketRegistrationTest, ListenerOverObIsRefused) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);

  auto res = nodes::Listener::Create("NEXO", "ob.best.bid.price",
                                     std::make_shared<NullNode>(), &pool, &dispatcher);
  ASSERT_FALSE(res.has_value()) << "a Listener over ob.* must be refused";
  EXPECT_NE(res.error().message.find("pipeline-only"), std::string::npos)
      << res.error().message;

  auto bare = nodes::Listener::Create("NEXO", "lastPrice",
                                      std::make_shared<NullNode>(), &pool, &dispatcher);
  ASSERT_TRUE(bare.has_value());
  (*bare)->shutdown();
  pool.shutdown();
}

TEST(MarketRegistrationTest, ItchProtocolMapsToWsClient) {
  EXPECT_EQ(engine::ProtocolIngressRegistry::find("itch"),
            std::optional<std::string>("market.wsclient"));

  const auto ingress = forum::ConnectorsClient::parseConnectorsJson(
      R"([{"name": "feed-simulator", "endpointUrl": "ws://127.0.0.1:8100/feed",)"
      R"( "protocol": "itch", "symbols": ["NEXO","VALT"]}])");
  ASSERT_EQ(ingress.size(), 1u);
  EXPECT_EQ(ingress[0].kind, "market.wsclient");
  EXPECT_EQ(ingress[0].params.at("url"), "ws://127.0.0.1:8100/feed");
  EXPECT_EQ(ingress[0].params.at("symbols"), "NEXO,VALT");
}
