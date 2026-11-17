#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace krn {
class AtomicStore;
class Dispatcher;
} // namespace krn

namespace krn::flow {

// How a flow.* key reaches a consumer (SPEC 2026-11-17-order-flow-pipelines
// D4). docs/atomic-keys.md marks every key with one of these, and
// tests/flow/FlowVocabularyTest.cpp holds the doc and the code to each other.
enum class Publication {
  // A per-event increment, delivered with Dispatcher::notifyListeners and
  // never written to the AtomicStore. Subscribe with a Listener; bucket with
  // TumblingWindow -> VectorReducer.
  Pushed,
  // Cumulative or instantaneous state, written with AtomicStore::set and
  // never notified. Read it with an AtomicAccessor: a Listener on it is
  // accepted (flow is not pipeline-only) and would never fire.
  Stored,
};

struct FlowKey {
  std::string name;   // full key, e.g. "flow.ofi"
  Publication publication;
};

// The publication helper every flow operator writes through. Stateless apart
// from the two engine pointers; either may be null (that half is then a
// no-op). Thread-safe: both engine calls are.
//
// Hot path: pass keys as long-lived std::strings (an operator's own static
// members), so a publish allocates nothing.
class FlowPublisher {
public:
  FlowPublisher(Dispatcher* dispatcher, AtomicStore* store)
    : dispatcher_(dispatcher), store_(store) {}

  // A per-event increment. Notified only when `value != 0` (D4): a zero
  // increment carries no information and would only inflate bucket counts.
  // Never stored. `key` must be declared Publication::Pushed.
  void increment(const std::string& symbol, const std::string& key, double value) const;

  // Cumulative / instantaneous state: AtomicStore::set, never notified, so it
  // is AtomicAccessor-only. `key` must be declared Publication::Stored. Note
  // AtomicStore silently drops a NEW field past its per-symbol cap.
  void state(const std::string& symbol, const std::string& key, double value) const;

  // Debug-build guard: when set, increment()/state() assert that the key is
  // declared with the matching Publication. FlowComponent installs its
  // vocabulary here; no effect with NDEBUG.
  void setVocabulary(const std::vector<FlowKey>& keys);

  Dispatcher*  dispatcher() const noexcept { return dispatcher_; }
  AtomicStore* store() const noexcept { return store_; }

private:
  Dispatcher*  dispatcher_;
  AtomicStore* store_;
  std::unordered_map<std::string, Publication> declared_;
};

} // namespace krn::flow
