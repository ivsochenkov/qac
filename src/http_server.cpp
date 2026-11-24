#include "qac/http_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "qac/log.hpp"

namespace qac {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

struct HttpServer::Impl : std::enable_shared_from_this<Impl> {
  class Session;

  Impl(Api& a, HttpServerConfig c)
      : api(a),
        cfg(std::move(c)),
        ioc(static_cast<int>(std::max(1u, cfg.io_threads))),
        workers(cfg.worker_threads ? cfg.worker_threads
                                   : std::max(2u, std::thread::hardware_concurrency())),
        acceptor(ioc),
        guard(net::make_work_guard(ioc)) {}

  Api& api;
  HttpServerConfig cfg;
  net::io_context ioc;
  net::thread_pool workers;
  tcp::acceptor acceptor;
  net::executor_work_guard<net::io_context::executor_type> guard;
  std::vector<std::thread> io_threads;

  std::atomic<bool> started{false};
  std::atomic<bool> stopping{false};
  unsigned short bound_port = 0;

  std::mutex mu;  // guards `sessions` and the shutdown wait
  std::condition_variable cv;
  std::unordered_map<Session*, std::weak_ptr<Session>> sessions;

  void do_accept();
  void register_session(Session* s, std::weak_ptr<Session> w) {
    std::lock_guard<std::mutex> lock(mu);
    sessions.emplace(s, std::move(w));
  }
  void unregister_session(Session* s) {
    std::lock_guard<std::mutex> lock(mu);
    sessions.erase(s);
    cv.notify_all();
  }
};

// One client connection. All members are touched on the connection's strand only.
class HttpServer::Impl::Session : public std::enable_shared_from_this<Session> {
 public:
  Session(Impl& srv, tcp::socket&& socket) : srv_(srv), stream_(std::move(socket)) {}
  ~Session() { srv_.unregister_session(this); }

  void run() {
    srv_.register_session(this, weak_from_this());
    net::dispatch(stream_.get_executor(), [self = shared_from_this()] {
      if (self->srv_.stopping) return self->close();
      self->read_header();
    });
  }

  // Called when the server shuts down.
  void on_stop() {
    stopping_ = true;
    if (!busy_) close();  // an idle keep-alive connection; a busy one closes after its response
  }

  void request_stop() {
    net::dispatch(stream_.get_executor(), [self = shared_from_this()] { self->on_stop(); });
  }

  // Closes even if a request is in flight (its response is then lost).
  void request_close() {
    net::dispatch(stream_.get_executor(), [self = shared_from_this()] {
      self->stopping_ = true;
      self->close();
    });
  }

  // Only for use after every I/O thread has been joined, when nothing else touches the session.
  void close_unsynchronized() { close(); }

 private:
  using Parser = http::request_parser<http::string_body>;

  void close() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream_.close();
  }

  static std::chrono::seconds secs(unsigned s) { return std::chrono::seconds(std::max(1u, s)); }

  void read_header() {
    parser_.emplace();
    parser_->body_limit(srv_.cfg.max_body_bytes);
    parser_->header_limit(srv_.cfg.max_header_bytes);
    stream_.expires_after(secs(srv_.cfg.idle_timeout_s));
    http::async_read_header(stream_, buf_, *parser_,
                            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                              self->on_header(ec);
                            });
  }

  void on_header(beast::error_code ec) {
    if (ec == http::error::end_of_stream || ec == net::error::operation_aborted ||
        ec == beast::error::timeout || ec == net::error::connection_reset)
      return close();
    if (ec) return fail(ec);
    if (stopping_ || srv_.stopping) return close();

    if (beast::iequals(parser_->get()[http::field::expect], "100-continue")) {
      continue_.emplace(http::status::continue_, parser_->get().version());
      stream_.expires_after(secs(srv_.cfg.io_timeout_s));
      http::async_write(stream_, *continue_, [self = shared_from_this()](beast::error_code e, std::size_t) {
        if (e) return self->close();
        self->read_body();
      });
      return;
    }
    read_body();
  }

  void read_body() {
    stream_.expires_after(secs(srv_.cfg.io_timeout_s));
    http::async_read(stream_, buf_, *parser_,
                     [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_body(ec); });
  }

  void on_body(beast::error_code ec) {
    if (ec == net::error::operation_aborted || ec == beast::error::timeout ||
        ec == net::error::connection_reset || ec == http::error::end_of_stream)
      return close();
    if (ec) return fail(ec);

    // Stopping: the worker pool may already be draining, so no new work is started.
    // The client sees the connection close without a response and can retry.
    if (stopping_ || srv_.stopping) return close();

    http::request<http::string_body> req = parser_->release();
    keep_alive_ = req.keep_alive();
    const bool head = req.method() == http::verb::head;
    const unsigned version = req.version();
    busy_ = true;

    ApiRequest ar{std::string(req.method_string()), std::string(req.target()), std::move(req.body())};
    net::post(srv_.workers, [self = shared_from_this(), ar = std::move(ar), head, version]() mutable {
      ApiResponse resp = self->srv_.api.handle(ar);
      net::post(self->stream_.get_executor(),
                [self, resp = std::move(resp), head, version]() mutable {
                  self->respond(std::move(resp), head, version);
                });
    });
  }

  void respond(ApiResponse resp, bool head, unsigned version) {
    const bool keep = keep_alive_ && !stopping_ && !srv_.stopping;
    stream_.expires_after(secs(srv_.cfg.io_timeout_s));
    auto done = [self = shared_from_this(), keep](beast::error_code ec, std::size_t) {
      self->busy_ = false;
      // `stopping_` may have been set while the response was being written
      if (ec || !keep || self->stopping_ || self->srv_.stopping) return self->close();
      self->read_header();
    };
    auto fill = [&](auto& res) {
      res.version(version);
      res.result(static_cast<http::status>(resp.status));
      res.set(http::field::server, "qac");
      res.set(http::field::content_type, resp.content_type);
      res.set(http::field::cache_control, "no-store");
      for (const auto& [k, v] : resp.headers) res.set(k, v);
      res.keep_alive(keep);
    };
    if (head) {  // same headers (including the length) as GET, but no body
      auto res = std::make_shared<http::response<http::empty_body>>();
      fill(*res);
      res->set(http::field::content_length, std::to_string(resp.body.size()));
      http::async_write(stream_, *res, [res, done](beast::error_code ec, std::size_t n) { done(ec, n); });
    } else {
      auto res = std::make_shared<http::response<http::string_body>>();
      fill(*res);
      res->body() = std::move(resp.body);
      res->prepare_payload();
      http::async_write(stream_, *res, [res, done](beast::error_code ec, std::size_t n) { done(ec, n); });
    }
  }

  // A malformed or oversized request: answer and close.
  void fail(beast::error_code ec) {
    unsigned status = 400;
    const char* code = "bad_request";
    std::string message = "malformed HTTP request";
    if (ec == http::error::body_limit) {
      status = 413;
      code = "payload_too_large";
      message = "the request body exceeds " + std::to_string(srv_.cfg.max_body_bytes) + " bytes";
    } else if (ec == http::error::header_limit) {
      status = 431;
      code = "headers_too_large";
      message = "the request headers are too large";
    }
    auto res = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(status), 11);
    res->set(http::field::server, "qac");
    res->set(http::field::content_type, "application/json");
    res->keep_alive(false);
    res->body() = std::string("{\"error\":\"") + code + "\",\"message\":\"" + message + "\"}";
    res->prepare_payload();
    stream_.expires_after(secs(srv_.cfg.io_timeout_s));
    http::async_write(stream_, *res, [self = shared_from_this(), res](beast::error_code e, std::size_t) {
      if (e) return self->close();
      // Half-close and drain what the client is still sending: closing with unread
      // data would reset the connection and could discard the response.
      beast::error_code ignored;
      self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
      self->drain();
    });
  }

  void drain() {
    stream_.expires_after(std::chrono::seconds(2));
    stream_.socket().async_read_some(net::buffer(drain_buf_),
                                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                                       if (ec) return self->close();
                                       self->drain();
                                     });
  }

  Impl& srv_;
  beast::tcp_stream stream_;
  beast::flat_buffer buf_;
  std::optional<Parser> parser_;
  std::optional<http::response<http::empty_body>> continue_;
  std::array<char, 4096> drain_buf_{};
  bool keep_alive_ = false;
  bool busy_ = false;
  bool stopping_ = false;
};

void HttpServer::Impl::do_accept() {
  acceptor.async_accept(net::make_strand(ioc), [this](beast::error_code ec, tcp::socket socket) {
    if (ec == net::error::operation_aborted || stopping) return;
    if (!ec) {
      std::size_t active;
      {
        std::lock_guard<std::mutex> lock(mu);
        active = sessions.size();
      }
      if (active >= cfg.max_connections) {
        beast::error_code ignored;
        socket.close(ignored);  // over the limit: refuse
      } else {
        std::make_shared<Session>(*this, std::move(socket))->run();
      }
    } else {
      log_warn("accept failed: " + ec.message());
    }
    do_accept();
  });
}

HttpServer::HttpServer(Api& api, HttpServerConfig config)
    : impl_(std::make_unique<Impl>(api, std::move(config))) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
  Impl& s = *impl_;
  if (s.started.exchange(true)) throw std::logic_error("the server is already started");
  try {
    beast::error_code ec;
    const auto addr = net::ip::make_address(s.cfg.address, ec);
    if (ec) throw std::runtime_error("invalid listen address '" + s.cfg.address + "': " + ec.message());
    const tcp::endpoint ep(addr, s.cfg.port);
    s.acceptor.open(ep.protocol(), ec);
    if (!ec) s.acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if (!ec) s.acceptor.bind(ep, ec);
    if (!ec) s.acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec)
      throw std::runtime_error("cannot listen on " + s.cfg.address + ":" + std::to_string(s.cfg.port) +
                               ": " + ec.message());
    s.bound_port = s.acceptor.local_endpoint().port();
  } catch (...) {
    s.started = false;
    throw;
  }

  s.do_accept();
  for (unsigned i = 0; i < std::max(1u, s.cfg.io_threads); ++i)
    s.io_threads.emplace_back([&s] {
      for (;;) {
        try {
          s.ioc.run();
          return;
        } catch (const std::exception& e) {
          log_error(std::string("I/O thread: ") + e.what());
        }
      }
    });
}

void HttpServer::stop() {
  Impl& s = *impl_;
  if (!s.started || s.stopping.exchange(true)) return;

  auto live_sessions = [&s] {
    std::vector<std::shared_ptr<Impl::Session>> live;
    std::lock_guard<std::mutex> lock(s.mu);
    for (auto& [ptr, weak] : s.sessions)
      if (auto sp = weak.lock()) live.push_back(std::move(sp));
    return live;
  };
  auto wait_for_sessions = [&s](std::chrono::milliseconds limit) {
    std::unique_lock<std::mutex> lock(s.mu);
    return s.cv.wait_for(lock, limit, [&s] { return s.sessions.empty(); });
  };

  // The worker pool and the event loop must stay alive while any session still has
  // work in flight: a handler posted to a drained pool is silently discarded and its
  // connection would never be answered or closed.

  // 1. no new connections
  net::post(s.ioc, [&s] {
    beast::error_code ec;
    s.acceptor.close(ec);
  });

  // 2. idle connections close now, busy ones after their response
  for (auto& sp : live_sessions()) sp->request_stop();

  // 3. give the in-flight requests time to finish
  if (!wait_for_sessions(std::chrono::seconds(5))) {
    log_warn("shutdown: closing connections that are still busy");
    for (auto& sp : live_sessions()) sp->request_close();
  }

  // 4. only now drain the pool; the responses of requests that were still running are
  //    posted back to the (running) event loop and fail on the closed sockets
  s.workers.join();
  wait_for_sessions(std::chrono::seconds(5));

  // 5. stop the event loop and make sure nothing is left open
  s.guard.reset();
  s.ioc.stop();
  for (auto& t : s.io_threads) t.join();
  s.io_threads.clear();
  for (auto& sp : live_sessions()) sp->close_unsynchronized();
}

unsigned short HttpServer::port() const { return impl_->bound_port; }

std::size_t HttpServer::active_connections() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->sessions.size();
}

}  // namespace qac
