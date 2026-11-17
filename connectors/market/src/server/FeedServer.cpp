#include "krn/market/FeedServer.hpp"

#include <boost/asio/strand.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>

#include <deque>
#include <string>
#include <utility>

// Project headers (adjust paths if needed)
#include "krn/Dispatcher.hpp"
#include "krn/Event.hpp"
#include "krn/book/OrderBookManager.hpp"
#include "krn/feed/IngressApplier.hpp"
#include "krn/feed/ObJsonTranslator.hpp"
#include "krn/util/Logger.hpp"
#include "krn/util/Metrics.hpp"

#include <rapidjson/document.h>

namespace krn {

using tcp = boost::asio::ip::tcp;

// ---------------------- FeedSession ----------------------
class FeedServer::FeedSession : public std::enable_shared_from_this<FeedSession> {
public:
  FeedSession(tcp::socket socket,
              Dispatcher* dispatcher,
              OrderBookManager* obManager,
              FeedServer* owner,
              std::shared_ptr<feed::FlowObserverRegistry> observers)
    : socket_(std::move(socket))
    , dispatcher_(dispatcher)
    , obManager_(obManager)
    , owner_(owner)
    , applier_(dispatcher, obManager, std::move(observers))
    , idleTimer_(socket_.get_executor())
  {}

  void start() { resetIdleTimer(); doRead(); }

  // M5: close() touches the (non-thread-safe) socket_ + idleTimer_, which are
  // only safe to touch from this session's strand. Callers running off-strand
  // (e.g. FeedServer::stop on the caller's thread) must route through here so
  // the work is posted to the strand with a live shared_ptr held across it.
  void closeOnStrand() {
    auto self = shared_from_this();
    boost::asio::post(socket_.get_executor(), [self]{ self->close(); });
  }

  void close() {
    boost::system::error_code ec;
    idleTimer_.cancel();
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    // Remove ourselves from the owner's session set.
    if (owner_) {
      std::lock_guard<std::mutex> lk(owner_->mu_);
      owner_->sessions_.erase(shared_from_this());
    }
  }

private:
  void doRead() {
    auto self = shared_from_this();
    socket_.async_read_some(
      boost::asio::buffer(buf_),
      [self](boost::system::error_code ec, std::size_t n) {
        self->onRead(ec, n);
      });
  }

  void onRead(boost::system::error_code ec, std::size_t n) {
    if (ec) {
      // Session ends on error
      close();
      return;
    }

    resetIdleTimer();

    // Very simple framing: treat incoming as newline-delimited text messages
    pending_.insert(pending_.end(), buf_.data(), buf_.data() + n);

    // Guard against slow-send DoS: disconnect if pending data exceeds limit
    // without producing a complete line.
    if (pending_.size() > MAX_PENDING) {
      KRN_METRIC_HIT("feed.pending_overflow");
      krn::util::logger().log(krn::util::LogLevel::Warn,
                              "feed.pending_overflow",
                              {{"size", std::to_string(pending_.size())}});
      close();
      return;
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i] == '\n') {
        std::size_t lineEnd = i;
        if (lineEnd > start && pending_[lineEnd - 1] == '\r') {
          --lineEnd;
        }
        const std::string line(pending_.data() + start, pending_.data() + lineEnd);
        handleLine(line);
        start = i + 1;
      }
    }
    if (start > 0) {
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(start));
    }

    doRead(); // continue reading
  }

  static constexpr std::size_t MAX_LINE_SIZE = 64 * 1024;  // 64 KB
  static constexpr std::size_t MAX_PENDING  = 128 * 1024; // 128 KB

  void handleLine(const std::string& line) {
    KRN_METRIC_HIT("feed.line_in");

    // Guard against oversized lines (DoS prevention).
    if (line.size() > MAX_LINE_SIZE) {
      KRN_METRIC_HIT("feed.tick_bad");
      krn::util::logger().log(krn::util::LogLevel::Warn,
                              "feed.line.too_large",
                              {{"size", std::to_string(line.size())}});
      return;
    }

    try {
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        KRN_METRIC_HIT("feed.tick_bad");
        krn::util::logger().log(krn::util::LogLevel::Warn,
                                "feed.line.bad_json",
                                {{"line", line.substr(0, 200)}});
        return;
      }

      // Route by "type" field
      if (doc.HasMember("type") && doc["type"].IsString()) {
        const std::string type = doc["type"].GetString();
        if (type == "ob") {
          handleObMessage(doc);
          return;
        }
        if (type == "control") {
          handleControlMessage(doc);
          return;
        }
        // Unknown type — fall through to tick path
      }

      // Default: market tick path
      handleTickMessage(doc, line);

    } catch (const std::exception& ex) {
      KRN_METRIC_HIT("feed.tick_bad");
      krn::util::logger().log(krn::util::LogLevel::Error,
                              "feed.handleLine exception",
                              {{"err", ex.what()}});
    } catch (...) {
      KRN_METRIC_HIT("feed.tick_bad");
      krn::util::logger().log(krn::util::LogLevel::Error,
                              "feed.handleLine unknown exception");
    }
  }

  void handleTickMessage(rapidjson::Document& doc, const std::string& line) {
    if (!dispatcher_) return;

    if (!doc.HasMember("symbol") || !doc["symbol"].IsString()) {
      KRN_METRIC_HIT("feed.tick_bad");
      krn::util::logger().log(krn::util::LogLevel::Warn,
                              "feed.line.missing_symbol",
                              {{"line", line.substr(0, 200)}});
      return;
    }

    std::string symbol = doc["symbol"].GetString();

    // Reject empty or absurdly long symbols.
    if (symbol.empty() || symbol.size() > 64) {
      KRN_METRIC_HIT("feed.tick_bad");
      krn::util::logger().log(krn::util::LogLevel::Warn,
                              "feed.line.bad_symbol",
                              {{"len", std::to_string(symbol.size())}});
      return;
    }

    // Move doc onto heap for shared ownership in the tick
    auto shared = std::make_shared<rapidjson::Document>(std::move(doc));

    KRN_METRIC_HIT("feed.tick_ok");
    KRN_METRIC_HIT("dispatch.tick");
    std::vector<feed::FeedEvent> frame;
    frame.emplace_back(feed::TickEvent{std::move(symbol), std::move(shared)});
    applier_.applyGated(frame, /*dropBook=*/false);
  }

  void handleObMessage(const rapidjson::Document& doc) {
    if (!obManager_) {
      KRN_METRIC_HIT("feed.ob_no_manager");
      return;
    }

    // ENC-1645: translate first (pure), then gate, then apply through the
    // IngressApplier. The order of checks and the metric each failure hits are
    // unchanged: a bad header never consumes a seq; a bad body or unknown
    // action does (the gate runs between them, as before).
    feed::ObJsonFrame f = feed::translateObJson(doc);
    using St = feed::ObJsonFrame::Status;
    if (f.status == St::BadHeader) {
      KRN_METRIC_HIT("feed.ob_bad");
      return;
    }

    // M7: per-message gap detection. If the producer stamps a monotonic "seq",
    // gate this message through OrderBookManager::onSeq before applying it; a
    // detected gap marks the symbol stale (subsequent messages are auto-gated
    // until a snapshot resyncs). Conservative: producers that send no "seq"
    // field are unaffected (no behavior change).
    if (f.seq) {
      if (!obManager_->onSeq(f.symbol, *f.seq)) {
        KRN_METRIC_HIT("feed.ob_seq_gap");
        return;
      }
    }

    if (f.status == St::BadBody) {
      KRN_METRIC_HIT("feed.ob_bad");
      return;
    }
    if (f.status == St::UnknownAction) {
      KRN_METRIC_HIT("feed.ob_bad");
      krn::util::logger().log(krn::util::LogLevel::Warn,
                              "feed.ob.unknown_action",
                              {{"action", f.action}});
      return;
    }

    applier_.applyGated(f.events, /*dropBook=*/false);

    if      (f.action == "ticksize") KRN_METRIC_HIT("feed.ob_ticksize");
    else if (f.action == "add")      KRN_METRIC_HIT("feed.ob_add");
    else if (f.action == "update")   KRN_METRIC_HIT("feed.ob_update");
    else if (f.action == "delete")   KRN_METRIC_HIT("feed.ob_delete");
    else if (f.action == "trade")    KRN_METRIC_HIT("feed.ob_trade");
  }

  void handleControlMessage(const rapidjson::Document& doc) {
    if (!doc.HasMember("action") || !doc["action"].IsString()) {
      KRN_METRIC_HIT("feed.control_bad");
      return;
    }

    const std::string action = doc["action"].GetString();

    if (action == "reset") {
      if (!doc.HasMember("symbol") || !doc["symbol"].IsString()) {
        KRN_METRIC_HIT("feed.control_bad");
        return;
      }
      uint32_t epoch = 0;
      if (doc.HasMember("epoch") && doc["epoch"].IsUint())
        epoch = doc["epoch"].GetUint();

      if (obManager_) {
        std::vector<feed::FeedEvent> frame;
        frame.emplace_back(feed::ObResetEvent{doc["symbol"].GetString(), epoch});
        applier_.applyGated(frame, /*dropBook=*/false);
        KRN_METRIC_HIT("feed.control_reset");
      }
    } else {
      KRN_METRIC_HIT("feed.control_bad");
      krn::util::logger().log(krn::util::LogLevel::Warn,
                              "feed.control.unknown_action",
                              {{"action", action}});
    }
  }

  static constexpr std::chrono::minutes IDLE_TIMEOUT{5};

  void resetIdleTimer() {
    auto self = shared_from_this();
    idleTimer_.expires_after(IDLE_TIMEOUT);
    idleTimer_.async_wait([self](boost::system::error_code ec) {
      if (!ec) {
        krn::util::logger().log(krn::util::LogLevel::Warn,
                                "feed.session.idle_timeout");
        KRN_METRIC_HIT("feed.idle_timeout");
        self->close();
      }
    });
  }

private:
  tcp::socket        socket_;
  Dispatcher*  dispatcher_{nullptr};  // not owned
  OrderBookManager*  obManager_{nullptr};   // not owned
  FeedServer*        owner_{nullptr};       // not owned
  feed::IngressApplier applier_;            // this session's strand only
  boost::asio::steady_timer idleTimer_;

  std::array<char, 8 * 1024> buf_{};
  std::vector<char>          pending_;
};

// ---------------------- FeedServer ----------------------

FeedServer::FeedServer(boost::asio::io_context& ioc,
                       Dispatcher* dispatcher,
                       unsigned short port)
  : FeedServer(ioc, dispatcher, nullptr, port)
{}

FeedServer::FeedServer(boost::asio::io_context& ioc,
                       Dispatcher* dispatcher,
                       OrderBookManager* obManager,
                       unsigned short port,
                       std::shared_ptr<feed::FlowObserverRegistry> observers)
  : ioc_(ioc),
    acceptor_(ioc),
    port_(port),
    dispatcher_(dispatcher),
    obManager_(obManager),
    observers_(std::move(observers))
{
  // Constructor is intentionally side-effect-free — no bind, no listen.
  // run() opens the socket so callers can construct ahead of start (per
  // the IConnector lifecycle: registerWith allocates, start brings up).
}

void FeedServer::run() {
  bool expected = false;
  if (!accepting_.compare_exchange_strong(expected, true)) return;

  boost::system::error_code ec;
  tcp::endpoint ep{tcp::v4(), port_};

  // ENC-1006: carry the operation + port in the exception so a failed bind
  // surfaces as an actionable message rather than a bare errno string.
  const unsigned short port = port_;
  auto fail = [port](const boost::system::error_code& e, const char* op) {
    throw boost::system::system_error(
        e, std::string("feed server: ") + op + " 0.0.0.0:" + std::to_string(port));
  };

  acceptor_.open(ep.protocol(), ec);
  if (ec) fail(ec, "open");

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) fail(ec, "set_option(reuse_address) on");

  acceptor_.bind(ep, ec);
  if (ec) fail(ec, "bind");

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) fail(ec, "listen on");

  doAccept();
}

void FeedServer::stop() {
  accepting_.store(false);

  boost::system::error_code ec;
  acceptor_.cancel(ec);

  std::unordered_set<std::shared_ptr<FeedSession>> copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    copy = sessions_;
    sessions_.clear();
  }
  for (auto& s : copy) {
    s->closeOnStrand();
  }
}

void FeedServer::doAccept() {
  if (!accepting_.load()) return;

  acceptor_.async_accept(
    boost::asio::make_strand(ioc_),
    [this](boost::system::error_code ec, tcp::socket socket) {
      onAccept(ec, std::move(socket));
    });
}

void FeedServer::onAccept(boost::system::error_code ec, tcp::socket socket) {
  if (!accepting_.load()) return;

  if (!ec) {
    // Guard against fd-exhaustion DoS: reject new connections once the limit
    // is reached.  Feed producers are few; 64 is generous.
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (sessions_.size() >= MAX_FEED_SESSIONS) {
        KRN_METRIC_HIT("feed.conn_rejected");
        krn::util::logger().log(krn::util::LogLevel::Warn,
                                "feed.max_sessions_reached",
                                {{"max", std::to_string(MAX_FEED_SESSIONS)}});
        // Let socket close on scope exit.
        doAccept();
        return;
      }
    }

    auto sp = std::make_shared<FeedSession>(std::move(socket), dispatcher_, obManager_, this,
                                           observers_);
    {
      std::lock_guard<std::mutex> lk(mu_);
      sessions_.insert(sp);
    }
    sp->start();
  }

  doAccept();
}

} // namespace krn
