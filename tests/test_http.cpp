#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <sys/socket.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "qac/http_server.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
using tcp = net::ip::tcp;

namespace {

const Alphabet& alphabet() {
  static const Alphabet a = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  return a;
}

struct Client {
  net::io_context ioc;
  beast::tcp_stream stream{ioc};
  beast::flat_buffer buf;

  explicit Client(unsigned short port) {
    stream.expires_after(std::chrono::seconds(10));
    stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    // Blocking reads ignore expires_after(): a stuck server must fail a test, not hang it.
    timeval tv{10, 0};
    ::setsockopt(stream.socket().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }

  http::response<http::string_body> send(http::verb method, const std::string& target,
                                         const std::string& body = "", bool keep_alive = true) {
    http::request<http::string_body> req{method, target, 11};
    req.set(http::field::host, "localhost");
    req.keep_alive(keep_alive);
    if (!body.empty() || method == http::verb::post) {
      req.set(http::field::content_type, "application/json");
      req.body() = body;
      req.prepare_payload();
    }
    stream.expires_after(std::chrono::seconds(10));
    http::write(stream, req);
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    return res;
  }

  // Sends raw bytes and returns everything the server answers until it closes.
  std::string raw(const std::string& bytes) {
    stream.expires_after(std::chrono::seconds(10));
    net::write(stream.socket(), net::buffer(bytes));
    std::string out;
    beast::error_code ec;
    char tmp[4096];
    for (;;) {
      const auto n = stream.socket().read_some(net::buffer(tmp), ec);
      if (ec) break;
      out.append(tmp, n);
    }
    return out;
  }
};

json::value parse(const std::string& s) {
  boost::system::error_code ec;
  json::value v = json::parse(s, ec);
  EXPECT_FALSE(ec) << s;
  return v;
}

struct HttpFixture : ::testing::Test {
  TempDir dir;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<Api> api;
  std::unique_ptr<HttpServer> server;

  void start(HttpServerConfig cfg = {}) {
    EngineConfig c;
    c.data_dir = dir.sub("data");
    c.fsync = FsyncPolicy::Never;
    c.snapshot_interval_s = 0;
    c.snapshot_wal_bytes = 0;
    c.cache_entries = 0;
    engine = std::make_unique<Engine>(alphabet(), c);
    ApiConfig ac;
    ac.demo_page = "<html>demo</html>";
    api = std::make_unique<Api>(*engine, ac);
    cfg.address = "127.0.0.1";
    cfg.port = 0;
    cfg.io_threads = 2;
    cfg.worker_threads = 4;
    server = std::make_unique<HttpServer>(*api, cfg);
    server->start();
  }
  void TearDown() override {
    if (server) server->stop();
  }
  unsigned short port() const { return server->port(); }
};

}  // namespace

TEST_F(HttpFixture, ServesTheApi) {
  start();
  Client c(port());
  auto res = c.send(http::verb::post, "/v1/queries", R"({"query":"hello world","count":2})");
  EXPECT_EQ(res.result_int(), 201);
  EXPECT_EQ(res[http::field::content_type], "application/json");
  EXPECT_EQ(res[http::field::cache_control], "no-store");
  EXPECT_EQ(res[http::field::server], "qac");

  res = c.send(http::verb::get, "/v1/complete?q=hel");  // same connection: keep-alive
  ASSERT_EQ(res.result_int(), 200);
  const auto v = parse(res.body());
  ASSERT_EQ(v.at("global").as_array().size(), 1u);
  EXPECT_EQ(v.at("global").as_array()[0].at("text").as_string(), "hello world");

  res = c.send(http::verb::get, "/");
  EXPECT_EQ(res.body(), "<html>demo</html>");
  EXPECT_EQ(res[http::field::content_type], "text/html; charset=utf-8");
  res = c.send(http::verb::get, "/nope");
  EXPECT_EQ(res.result_int(), 404);
  res = c.send(http::verb::delete_, "/v1/queries/1");
  EXPECT_EQ(res.result_int(), 200);
}

TEST_F(HttpFixture, HeadHasHeadersButNoBody) {
  start();
  Client c(port());
  const auto get = c.send(http::verb::get, "/healthz");
  http::request<http::empty_body> req{http::verb::head, "/healthz", 11};
  req.set(http::field::host, "localhost");
  http::write(c.stream, req);
  http::response_parser<http::empty_body> p;
  p.skip(true);  // a response to HEAD has no body
  http::read(c.stream, c.buf, p);
  EXPECT_EQ(p.get().result_int(), 200);
  EXPECT_EQ(p.get()[http::field::content_length], std::to_string(get.body().size()));
  // the connection is still usable
  EXPECT_EQ(c.send(http::verb::get, "/healthz").body(), get.body());
}

TEST_F(HttpFixture, ConnectionCloseIsHonoured) {
  start();
  Client c(port());
  const auto res = c.send(http::verb::get, "/healthz", "", false);
  EXPECT_FALSE(res.keep_alive());
  http::response<http::string_body> next;
  beast::error_code ec;
  http::read(c.stream, c.buf, next, ec);
  EXPECT_EQ(ec, http::error::end_of_stream);
}

TEST_F(HttpFixture, OversizedBodyGets413) {
  HttpServerConfig cfg;
  cfg.max_body_bytes = 256;
  start(cfg);
  Client c(port());
  const std::string big = R"({"query":")" + std::string(1000, 'a') + R"("})";
  const auto out = c.raw("POST /v1/queries HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                         "Content-Length: " + std::to_string(big.size()) + "\r\n\r\n" + big);
  EXPECT_NE(out.find("HTTP/1.1 413"), std::string::npos) << out;
  EXPECT_NE(out.find("payload_too_large"), std::string::npos);
  EXPECT_EQ(engine->stats().index.live_queries, 0u);
}

TEST_F(HttpFixture, MalformedRequestsGet400AndDoNotKillTheServer) {
  start();
  {
    Client c(port());
    const auto out = c.raw("THIS IS NOT HTTP\r\n\r\n");
    EXPECT_NE(out.find("HTTP/1.1 400"), std::string::npos) << out;
  }
  {
    Client c(port());
    const auto out = c.raw("GET /v1/complete?q=a HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n");
    EXPECT_NE(out.find("HTTP/1.1 400"), std::string::npos) << out;
  }
  {
    Client c(port());
    const auto out = c.raw("GET /" + std::string(40000, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(out.find("HTTP/1.1 431") != std::string::npos || out.find("HTTP/1.1 400") != std::string::npos) << out;
  }
  Client c(port());
  EXPECT_EQ(c.send(http::verb::get, "/healthz").result_int(), 200);
}

TEST_F(HttpFixture, Expect100Continue) {
  start();
  Client c(port());
  const std::string body = R"({"query":"continue me"})";
  http::request<http::string_body> req{http::verb::post, "/v1/queries", 11};
  req.set(http::field::host, "localhost");
  req.set(http::field::expect, "100-continue");
  req.body() = body;
  req.prepare_payload();
  http::request_serializer<http::string_body> sr{req};
  http::write_header(c.stream, sr);
  http::response<http::string_body> interim;
  http::response_parser<http::string_body> p;
  http::read_header(c.stream, c.buf, p);
  EXPECT_EQ(p.get().result(), http::status::continue_);
  http::write(c.stream, sr);
  http::response<http::string_body> res;
  http::read(c.stream, c.buf, res);
  EXPECT_EQ(res.result_int(), 201) << res.body();
}

TEST_F(HttpFixture, ChunkedRequestBody) {
  start();
  Client c(port());
  const auto out = c.raw("POST /v1/queries HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                         "Connection: close\r\n\r\n"
                         "8\r\n{\"query\"\r\n"
                         "f\r\n:\"chunked one\"}\r\n"
                         "0\r\n\r\n");
  EXPECT_NE(out.find("HTTP/1.1 201"), std::string::npos) << out;
  EXPECT_EQ(engine->stats().index.global_queries, 1u);
}

TEST_F(HttpFixture, PipelinedRequestsAreAnsweredInOrder) {
  start();
  Client c(port());
  const std::string one = "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n";
  const std::string two = "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  const auto out = c.raw(one + two);
  const auto first = out.find("HTTP/1.1 200");
  const auto second = out.find("HTTP/1.1 404");
  ASSERT_NE(first, std::string::npos) << out;
  ASSERT_NE(second, std::string::npos) << out;
  EXPECT_LT(first, second);
}

TEST_F(HttpFixture, ManyConcurrentClientsReadingAndWriting) {
  start();
  constexpr int kClients = 16, kRounds = 40;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t)
    threads.emplace_back([&, t] {
      try {
        Client c(port());
        for (int i = 0; i < kRounds; ++i) {
          const std::string q = "client" + std::string(1, static_cast<char>('a' + t)) + " round " + std::to_string(i);
          if (c.send(http::verb::post, "/v1/queries", R"({"query":")" + q + R"("})").result_int() >= 300) ++failures;
          if (c.send(http::verb::post, "/v1/users/" + std::to_string(t) + "/queries", R"({"query":")" + q + R"("})")
                  .result_int() >= 300)
            ++failures;
          const auto r = c.send(http::verb::get, "/v1/complete?q=client&user=" + std::to_string(t));
          if (r.result_int() != 200) ++failures;
        }
      } catch (const std::exception&) {
        ++failures;
      }
    });
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(engine->stats().index.global_queries, static_cast<std::uint64_t>(kClients * kRounds));
  EXPECT_EQ(engine->stats().index.users, static_cast<std::uint64_t>(kClients));
}

TEST_F(HttpFixture, ConnectionLimit) {
  HttpServerConfig cfg;
  cfg.max_connections = 2;
  start(cfg);
  Client a(port()), b(port());
  EXPECT_EQ(a.send(http::verb::get, "/healthz").result_int(), 200);
  EXPECT_EQ(b.send(http::verb::get, "/healthz").result_int(), 200);
  Client over(port());  // accepted by the kernel, refused by the server
  beast::error_code ec;
  http::request<http::empty_body> req{http::verb::get, "/healthz", 11};
  req.set(http::field::host, "x");
  http::write(over.stream, req, ec);
  http::response<http::string_body> res;
  if (!ec) http::read(over.stream, over.buf, res, ec);
  EXPECT_TRUE(ec) << "the third connection must be refused";
  EXPECT_EQ(a.send(http::verb::get, "/healthz").result_int(), 200);  // others unaffected
}

TEST_F(HttpFixture, IdleConnectionsAreClosed) {
  HttpServerConfig cfg;
  cfg.idle_timeout_s = 1;
  start(cfg);
  Client c(port());
  EXPECT_EQ(c.send(http::verb::get, "/healthz").result_int(), 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  beast::error_code ec;
  http::response<http::string_body> res;
  http::read(c.stream, c.buf, res, ec);
  EXPECT_EQ(ec, http::error::end_of_stream);
}

TEST_F(HttpFixture, StopClosesIdleConnectionsAndIsIdempotent) {
  start();
  Client idle(port());
  EXPECT_EQ(idle.send(http::verb::get, "/healthz").result_int(), 200);
  const auto t0 = std::chrono::steady_clock::now();
  server->stop();
  server->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
  EXPECT_EQ(server->active_connections(), 0u);
  beast::error_code ec;
  http::response<http::string_body> res;
  http::read(idle.stream, idle.buf, res, ec);
  EXPECT_TRUE(ec);
  // The port is free again.
  net::io_context ioc;
  beast::tcp_stream s(ioc);
  EXPECT_THROW(s.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port())), boost::system::system_error);
}

TEST_F(HttpFixture, StopWhileRequestsAreInFlight) {
  start();
  std::atomic<bool> go{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      try {
        Client c(port());
        for (int i = 0; go; ++i)
          c.send(http::verb::post, "/v1/queries", R"({"query":"load )" + std::to_string(t) + " " + std::to_string(i) + R"("})");
      } catch (const std::exception&) {
        // expected once the server goes away
      }
    });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  server->stop();
  go = false;
  for (auto& th : threads) th.join();
  // Whatever was acknowledged is in the engine and consistent.
  EXPECT_GT(engine->stats().index.global_queries, 0u);
}

TEST(HttpServer, RejectsBadAddressesAndBusyPorts) {
  TempDir dir;
  EngineConfig c;
  c.data_dir = dir.sub("data");
  c.fsync = FsyncPolicy::Never;
  c.snapshot_interval_s = 0;
  c.snapshot_wal_bytes = 0;
  Engine e(alphabet(), c);
  Api api(e);
  HttpServerConfig cfg;
  cfg.address = "not-an-address";
  HttpServer bad(api, cfg);
  EXPECT_THROW(bad.start(), std::runtime_error);

  cfg.address = "127.0.0.1";
  cfg.port = 0;
  HttpServer first(api, cfg);
  first.start();
  cfg.port = first.port();
  HttpServer second(api, cfg);
  EXPECT_THROW(second.start(), std::runtime_error);
}
