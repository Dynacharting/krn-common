#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "krn/feed/IFlowObserver.hpp"
#include "krn/flow/FlowConfig.hpp"
#include "krn/flow/FlowPublisher.hpp"
#include "krn/flow/IFlowOperator.hpp"
#include "krn/flow/ShadowOrderBook.hpp"

namespace boost::asio { class io_context; }

namespace krn {
class AtomicStore;
class Dispatcher;
class OrderBookManager;
} // namespace krn

namespace krn::flow {

// The order-flow component of the market plugin (ENC-1646, SPEC
// 2026-11-17-order-flow-pipelines D1, D3-D7). One IFlowObserver registered
// with MarketConnector::flowObservers(); it resolves each observed book event
// (effective time, shadow order before/after), then hands it to every
// operator in registration order under the symbol's own mutex.
//
// Core keys it publishes itself (all Stored, AtomicAccessor-only):
//   flow.meta.ts_source      0 = exchange tsNs, 1 = receive-clock fallback (D6); last event
//   flow.meta.unattributed   count of unattributed executions (D3)
//   flow.meta.unknown_order  count of update/delete events on orders the shadow never saw
//   flow.meta.unknown_cause  count of update/delete events with MutationCause::Unknown
//   flow.meta.shadow_orders  shadow map size after the last event
class FlowComponent final : public feed::IFlowObserver {
public:
  struct Env {
    Dispatcher*              dispatcher = nullptr;  // pushed increments
    AtomicStore*             store      = nullptr;  // stored state
    OrderBookManager*        books      = nullptr;  // tick conversion only (toTicks)
    boost::asio::io_context* io         = nullptr;  // for operator timers; may be null in tests
  };

  // `operators` defaults to the full V1 set (makeFlowOperators()).
  explicit FlowComponent(Env env);
  FlowComponent(Env env, std::vector<std::unique_ptr<IFlowOperator>> operators);
  ~FlowComponent() override;

  FlowComponent(const FlowComponent&) = delete;
  FlowComponent& operator=(const FlowComponent&) = delete;

  // The config reader's target; register it with
  // ConfigNamespaceRegistry::registerNamespace("flow", …).
  const std::shared_ptr<FlowConfigSource>& configSource() const noexcept { return config_; }

  // Snapshot the config and configure every operator. Idempotent; the first
  // event calls it too if start() has not run, so a test needs no start().
  void start();
  bool started() const noexcept { return started_.load(std::memory_order_acquire); }
  // Valid after start().
  const FlowConfig& config() const noexcept { return cfg_; }
  bool enabled() const noexcept { return started() && cfg_.enabled; }

  // Every key the component and its operators publish.
  std::vector<FlowKey> vocabulary() const;
  static const std::vector<FlowKey>& coreKeys();

  const FlowPublisher& publisher() const noexcept { return pub_; }
  boost::asio::io_context* io() const noexcept { return env_.io; }
  const std::vector<std::unique_ptr<IFlowOperator>>& operators() const noexcept { return ops_; }

  // Run `fn` for `op` on `symbol` under that symbol's mutex (ctx.event null).
  // For operator timers and cross-symbol closes (P2). Returns false when the
  // symbol has never been seen or `op` is not one of this component's.
  bool withSymbol(const std::string& symbol, const IFlowOperator& op,
                  const std::function<void(FlowContext&)>& fn);

  // Introspection (tests, diagnostics): shadow size; 0 for an unseen symbol.
  std::size_t shadowSize(const std::string& symbol) const;

  // IFlowObserver
  void onFlowEvent(const feed::FlowObservation& obs) override;
  void onBookReset(const feed::ObResetEvent& reset) override;

private:
  struct CoreMeta {
    int      tsSource     = -1;   // last published; -1 = never
    uint64_t unattributed = 0;
    uint64_t unknownOrder = 0;
    uint64_t unknownCause = 0;
    uint64_t unattributedPublished = 0;
    uint64_t unknownOrderPublished = 0;
    uint64_t unknownCausePublished = 0;
    std::size_t shadowPublished = static_cast<std::size_t>(-1);
  };
  struct SymbolFlow {
    std::mutex                                  mu;
    ShadowOrderBook                             shadow;
    std::vector<std::unique_ptr<OperatorState>> states;   // parallel to ops_
    CoreMeta                                    meta;
  };

  SymbolFlow& symbolFlow_(const std::string& symbol);
  SymbolFlow* findSymbolFlow_(const std::string& symbol) const;
  Price       toTicks_(const std::string& symbol, double px) const;
  void        resolve_(SymbolFlow& sf, FlowEvent& ev);
  void        publishMeta_(const std::string& symbol, SymbolFlow& sf, bool exchangeTs);
  void        runOperators_(const std::string& symbol, SymbolFlow& sf, const FlowEvent* ev, bool reset);

  Env                                          env_;
  std::vector<std::unique_ptr<IFlowOperator>>  ops_;
  FlowPublisher                                pub_;
  std::shared_ptr<FlowConfigSource>            config_;
  FlowConfig                                   cfg_;
  std::once_flag                               startOnce_;
  std::atomic<bool>                            started_{false};

  mutable std::mutex                                           symbolsMu_;
  std::unordered_map<std::string, std::unique_ptr<SymbolFlow>> symbols_;
};

} // namespace krn::flow
