#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "krn/flow/IFlowOperator.hpp"

namespace krn::flow {

// P2 — sweeps (ENC-1647, SPEC 2026-11-17-order-flow-pipelines D5 P2, rev 3).
//
// An *aggressive event* is the executions of ONE symbol that share one
// exchange tsNs and one resting side. A *sweep* is an aggressive event whose
// executions span >= flow.sweep.minLevels (default 2) distinct prices. Each
// sweep publishes, once, when its run closes — every key from the same close,
// so a bucket's `last` of each pairs up:
//
//   flow.sweep.levels    Pushed  distinct prices the run executed at
//   flow.sweep.notional  Pushed  sum of shares x price over the run's executions
//   flow.sweep.px        Pushed  price of the run's last execution (the deepest level reached)
//   flow.sweep.side      Pushed  +1 buy aggressor (resting asks hit), -1 sell aggressor (resting bids hit)
//   flow.sweep.count     Pushed  1 per sweep
//
// Publication order within one close is fixed: levels, notional, px, side,
// count. `side` comes after px and notional because the orderflow-v1 recipe
// (embassy) packs {v: px|notional, side} with a combineLatest headed by the
// side push; FlowSweepTest.SideIsPublishedAfterPxAndNotional pins it.
//
// What counts as an execution:
//   - an Update/Delete with MutationCause::Execution. Side, price and shares
//     come from the shadow order (`before`; shares = before.size - after.size);
//     for an order the shadow never saw, from the TobChange (the one side whose
//     pre touch lost shares: its pre price, the shares it lost) — skipped when
//     neither or both touches moved.
//   - an unattributed trade (D3: a non-bookNeutral trade that consumed the
//     book): a single-level execution at the trade price, on the passive side
//     (the aggressor's opposite; with no aggressor, by price against the pre
//     touch, as OrderBook::applyTrade decides).
//   - a bookNeutral print is NOT an execution here — the displayed fill (E) is
//     already its own Execution event and a hidden print (P) took no displayed
//     level. With the run's tsNs it is skipped and does not break the run.
//   - adds, cancels and replaces with the run's tsNs do not break it either
//     (the unfilled remainder of an aggressive limit order rests at that tsNs).
//
// matchNumber is not the grouping key (it is per execution).
//
// A run closes (and publishes, if it is a sweep):
//   - on the first event with a different tsNs for its own symbol;
//   - in onBeforeEventTimed, on the first event of ANY symbol with a LATER
//     exchange tsNs. It does not say which ingress an event came from, so "later"
//     is judged across all ingress sources of the component;
//   - after flow.sweep.closeMs (default 50) on the receive clock since the
//     run opened, on an io_context timer, so a quiet symbol does not publish
//     late. With no io_context (a bare test rig) there is no timer;
//   - on a book reset of its symbol (the executions were observed; the run is
//     published as far as it got).
// The two resting sides are separate runs: executions on both sides at one
// tsNs are two aggressive events, and interleaving does not break either.
//
// tsNs == 0 (D6 receive-clock fallback): there is no exchange instant to
// share — the fallback clock is distinct per event — so such an execution is
// an aggressive event of its own: one level, published at once only when
// flow.sweep.minLevels is 1. It closes its own symbol's open run (a different
// instant), but never another symbol's: a receive-clock time is not
// comparable with exchange time, so only the closeMs timer closes those.
// onBeforeEventTimed's `exchangeTs` says which clock a time is on.
//
// Shutdown: shutdown() (called by ~FlowComponent before any operator is
// destroyed, and by this destructor) waits for a running timer close and
// cancels the pending timer; a cancelled handler returns on
// operation_aborted without touching the operator or the component.
class SweepOperator final : public IFlowOperator {
public:
  static const std::string kLevels, kNotional, kPx, kSide, kCount;

  // The receive clock (ns) and the close-timer scheduler. `schedule(delayNs,
  // fire)` must call `fire` later, never inline. Null `schedule` = no timer.
  struct Timing {
    std::function<uint64_t()>                                   now;
    std::function<void(uint64_t delayNs, std::function<void()>)> schedule;
  };

  // Production: steady_clock, and a steady_timer on component.io() (none when
  // it is null), installed at configure().
  SweepOperator();
  // Tests: inject both (a fake clock and a captured scheduler).
  explicit SweepOperator(Timing timing);
  ~SweepOperator() override;

  std::string_view name() const override { return "sweep"; }
  std::vector<FlowKey> keys() const override;
  void configure(const FlowConfig& cfg, FlowComponent& component) override;
  std::unique_ptr<OperatorState> makeState() const override;
  void onBeforeEventTimed(const std::string& symbol, uint64_t tsNs, bool exchangeTs) override;
  void onEvent(FlowContext& ctx) override;
  void onReset(FlowContext& ctx) override;
  void shutdown() override;

  // Close every run whose closeMs has elapsed on the receive clock. What the
  // scheduled timer runs; public for tests.
  void fireTimer();

  // Symbols with an open run (diagnostics, tests).
  std::size_t openSymbols() const;

private:
  struct Open {
    uint64_t tsNs  = 0;   // the run's exchange tsNs
    uint64_t dueNs = 0;   // receive clock at which the timer closes it
  };
  struct Life {
    std::mutex mu;
    bool       alive = true;
  };

  void closeAll_(FlowContext& ctx);
  void armLocked_(uint64_t delayNs);

  bool                   injected_ = false;
  Timing                 timing_;
  FlowComponent*         component_ = nullptr;
  uint32_t               minLevels_ = 2;
  uint64_t               closeNs_   = 50'000'000;

  // Symbols with an open run. Lock order: a symbol's mutex, then mu_ — never
  // the reverse (onBeforeEventTimed and fireTimer drop mu_ before withSymbol).
  mutable std::mutex                    mu_;
  std::unordered_map<std::string, Open> open_;
  std::atomic<std::size_t>              openCount_{0};
  bool                                  armed_ = false;
  bool                                  shut_  = false;
  std::function<void()>                 cancelTimer_;   // cancels the armed production timer

  std::shared_ptr<Life> life_ = std::make_shared<Life>();
};

} // namespace krn::flow
