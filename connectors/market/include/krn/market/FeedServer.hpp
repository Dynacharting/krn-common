#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>

#include <boost/asio/ip/tcp.hpp>

namespace krn {

class Dispatcher;   // fwd-declare to keep header light
class OrderBookManager;   // fwd-declare for OB routing
namespace feed { class FlowObserverRegistry; }

/// Minimal TCP feed server that accepts producer connections and forwards
/// incoming messages to a Dispatcher. The actual deserialization is
/// done inside the session implementation (cpp), not exposed here: each JSON
/// line is translated to FeedEvents (`ob` lines via feed::translateObJson) and
/// applied through a per-session feed::IngressApplier, which notifies
/// `observers` (ENC-1645, SPEC 2026-11-17-order-flow-pipelines D3).
class FeedServer {
public:
  using tcp = boost::asio::ip::tcp;

  FeedServer(boost::asio::io_context& ioc,
             Dispatcher* dispatcher,
             unsigned short port);

  FeedServer(boost::asio::io_context& ioc,
             Dispatcher* dispatcher,
             OrderBookManager* obManager,
             unsigned short port,
             std::shared_ptr<feed::FlowObserverRegistry> observers = nullptr);

  FeedServer(const FeedServer&) = delete;
  FeedServer& operator=(const FeedServer&) = delete;

  void run();
  void stop();

private:
  class FeedSession; // pimpl session

  void doAccept();
  void onAccept(boost::system::error_code ec, tcp::socket socket);

private:
  static constexpr std::size_t MAX_FEED_SESSIONS = 64;

  boost::asio::io_context& ioc_;
  tcp::acceptor            acceptor_;
  std::atomic<bool>        accepting_{false};
  unsigned short           port_{0};

  Dispatcher*        dispatcher_; // not owned
  OrderBookManager*        obManager_{nullptr}; // not owned
  std::shared_ptr<feed::FlowObserverRegistry> observers_;

  std::mutex                                  mu_;
  std::unordered_set<std::shared_ptr<FeedSession>> sessions_;
};

} // namespace krn
