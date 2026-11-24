// REST API on top of the Engine, independent of any transport.
//
//   GET    /v1/complete?q=<text>[&n=10][&user=<id>&m=5][&correct=1][&rank=frequency|avg_word_freq]
//   POST   /v1/queries                       {"query": "...", "count": 1}
//   POST   /v1/users/{user}/queries          {"query": "...", "count": 1, "also_global": false}
//   DELETE /v1/queries/{id}
//   DELETE /v1/users/{user}/queries/{id}
//   POST   /v1/snapshot        persist now and truncate the WAL
//   POST   /v1/optimize        compact the dictionary and replace the snapshot
//   GET    /v1/stats           GET /healthz           GET /   (demo page)
//
// Errors are JSON: {"error": "<code>", "message": "..."}.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "qac/engine.hpp"

namespace qac {

struct ApiRequest {
  std::string method;  // upper case
  std::string target;  // path and optional query string, as sent
  std::string body;
};

struct ApiResponse {
  unsigned status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct ApiConfig {
  std::size_t max_n = 100;  // upper bound for n and m
  std::size_t max_m = 100;
  bool cors = false;        // Access-Control-Allow-Origin: *
  bool allow_admin = true;  // /v1/snapshot and /v1/optimize
  std::string_view demo_page;  // served at "/" when not empty
};

class Api {
 public:
  Api(Engine& engine, ApiConfig config = {}) : engine_(engine), cfg_(std::move(config)) {}

  // Never throws.
  ApiResponse handle(const ApiRequest& req) const;

 private:
  Engine& engine_;
  ApiConfig cfg_;
};

// Exposed for tests.
bool percent_decode(std::string_view in, std::string& out, bool plus_is_space);
bool parse_u32(std::string_view s, std::uint32_t& out);

}  // namespace qac
