#include "krn/flow/FlowPublisher.hpp"

#include <cassert>

#include "krn/AtomicStore.hpp"
#include "krn/Dispatcher.hpp"

namespace krn::flow {

void FlowPublisher::setVocabulary(const std::vector<FlowKey>& keys) {
  declared_.clear();
  for (const auto& k : keys) declared_.emplace(k.name, k.publication);
}

void FlowPublisher::increment(const std::string& symbol, const std::string& key,
                              double value) const {
#ifndef NDEBUG
  if (!declared_.empty()) {
    auto it = declared_.find(key);
    assert(it != declared_.end() && it->second == Publication::Pushed &&
           "flow key published as an increment is not declared Pushed");
  }
#endif
  if (value == 0.0 || !dispatcher_) return;
  dispatcher_->notifyListeners(symbol, key, value);
}

void FlowPublisher::level(const std::string& symbol, const std::string& key,
                          double value) const {
#ifndef NDEBUG
  if (!declared_.empty()) {
    auto it = declared_.find(key);
    assert(it != declared_.end() && it->second == Publication::Pushed &&
           "flow key published as a level is not declared Pushed");
  }
#endif
  if (!dispatcher_) return;
  dispatcher_->notifyListeners(symbol, key, value);
}

void FlowPublisher::state(const std::string& symbol, const std::string& key,
                          double value) const {
#ifndef NDEBUG
  if (!declared_.empty()) {
    auto it = declared_.find(key);
    assert(it != declared_.end() && it->second == Publication::Stored &&
           "flow key published as state is not declared Stored");
  }
#endif
  if (!store_) return;
  store_->set(symbol, key, value);
}

} // namespace krn::flow
