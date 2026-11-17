#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "krn/engine/IConnector.hpp"
#include "krn/market/MarketConfig.hpp"
#include "krn/market/MarketFieldMap.hpp"

namespace krn {
class OrderBookManager;
namespace ob       { class Provider; class FunctionalSnapshotSource; }
namespace feed     { class FlowObserverRegistry; }
namespace flow     { class FlowComponent; }
} // namespace krn

namespace krn::market {

// The market connector owns everything market-specific: order book engine,
// TA tick computer (registered with EventComputerRegistry), "ob.*" atomic
// provider namespace, TCP FeedServer, and external WsFeedClients driven by
// ItchAdapter.
//
// Lifecycle (driven by the composition root):
//   MarketConnector connector;
//   connector.registerWith(regs);   // construct + register; nothing running
//   connector.start();              // run the feed server + WS clients
//   ...
//   connector.stop();               // reverse-order teardown (idempotent)
class MarketConnector final : public engine::IConnector {
public:
  MarketConnector();
  // The market keys (TA periods) the "tick" computers are built with.
  explicit MarketConnector(MarketConfig marketCfg);
  ~MarketConnector() override;

  std::string_view name() const override { return "market"; }

  // Construct OrderBookManager, wire its provider into AtomicProviderRegistry
  // under "ob.*" and declare "ob" pipeline-only, register the two market
  // ingress kinds and the forum protocol "itch" -> "market.wsclient", and
  // register the "tick" event-computer factory.
  void registerWith(engine::EngineRegistries& reg) override;

  // Run the feed server + WS feed clients. Failures on individual feed
  // clients are logged and do not abort the rest.
  void start() override;

  // Reverse-order teardown. Idempotent: a second call is a no-op.
  void stop() noexcept override;

  // The order book registerWith() built; null before it.
  OrderBookManager* orderBooks() const noexcept { return _obManager.get(); }

  // The flow observers every market ingress source this connector builds
  // (market.feedserver sessions, market.wsclient clients) notifies through
  // its IngressApplier (ENC-1645, SPEC 2026-11-17-order-flow-pipelines D3).
  // Never null; exists from construction, so an observer may be added before
  // or after registerWith(). Ingress sources hold it by shared_ptr.
  const std::shared_ptr<feed::FlowObserverRegistry>& flowObservers() const noexcept {
    return _flowObservers;
  }

  // The order-flow component (ENC-1646, SPEC 2026-11-17-order-flow-pipelines
  // D1): built and added to flowObservers() by registerWith(), configured
  // from the `flow.*` namespace at start(), removed at stop() (and at start()
  // when flow.enabled = false). Null before registerWith().
  const std::shared_ptr<flow::FlowComponent>& flow() const noexcept { return _flow; }

private:
  std::shared_ptr<OrderBookManager>                  _obManager;
  std::shared_ptr<ob::FunctionalSnapshotSource>      _snapSource;
  std::shared_ptr<ob::Provider>                      _obProvider;

  // Populated by ConfigNamespaceRegistry callbacks during
  // Config::dispatchPendingKeys(); shared with MarketTickComputer
  // instances via the EventComputerRegistry factory closure.
  std::shared_ptr<MarketFieldMap>                    _fieldMap;

  MarketConfig                                       _marketCfg;

  std::shared_ptr<feed::FlowObserverRegistry>        _flowObservers;
  std::shared_ptr<flow::FlowComponent>               _flow;
};

} // namespace krn::market
