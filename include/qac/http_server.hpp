// HTTP/1.1 server on Boost.Beast / Boost.Asio.
//
// Threading: a few I/O threads run the asio event loop (accept, parse, write); the
// requests themselves are executed on a separate asio::thread_pool, so a slow
// engine call (a write waiting for the exclusive lock, an inline snapshot) never
// stalls socket I/O. A connection has at most one request in flight, which bounds
// the worker queue by the connection limit.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "qac/api.hpp"

namespace qac {

struct HttpServerConfig {
  std::string address = "0.0.0.0";
  unsigned short port = 8080;  // 0 picks a free port (see HttpServer::port())
  unsigned io_threads = 2;
  unsigned worker_threads = 0;  // 0: hardware concurrency
  std::size_t max_body_bytes = 64 * 1024;
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_connections = 10000;
  unsigned idle_timeout_s = 60;   // keep-alive connections without a request
  unsigned io_timeout_s = 30;     // a stalled read of a request / write of a response
};

class HttpServer {
 public:
  HttpServer(Api& api, HttpServerConfig config);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Binds and starts serving. Throws std::runtime_error when the address cannot be used.
  void start();
  // Stops accepting, lets in-flight requests finish, closes the connections and joins
  // all threads. Idempotent.
  void stop();

  unsigned short port() const;  // the bound port (after start())
  std::size_t active_connections() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace qac
