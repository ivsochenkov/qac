#include "qac/api.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

#include <boost/json.hpp>

#include "qac/log.hpp"
#include "qac/utf8.hpp"

namespace qac {

namespace json = boost::json;

bool parse_u32(std::string_view s, std::uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  if (v > std::numeric_limits<std::uint32_t>::max()) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}

bool percent_decode(std::string_view in, std::string& out, bool plus_is_space) {
  out.clear();
  out.reserve(in.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) return false;  // needs two hex digits after '%'
      const int h = hex(in[i + 1]), l = hex(in[i + 2]);
      if (h < 0 || l < 0) return false;
      out.push_back(static_cast<char>(h * 16 + l));
      i += 2;
    } else if (c == '+' && plus_is_space) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return true;
}

namespace {

// Text echoed into JSON must be valid UTF-8, or the document would be invalid.
std::string safe_text(const std::string& s) {
  std::u32string tmp;
  return utf8::decode(s, tmp) ? s : std::string("(not valid UTF-8)");
}

ApiResponse json_response(unsigned status, const json::value& v) {
  ApiResponse r;
  r.status = status;
  r.body = json::serialize(v);
  return r;
}

ApiResponse error(unsigned status, const char* code, const std::string& message) {
  return json_response(status, json::object{{"error", code}, {"message", safe_text(message)}});
}

ApiResponse from_result(const Result& r) {
  switch (r.status) {
    case Status::Ok: return json_response(200, json::object{{"ok", true}});
    case Status::BadInput: return error(400, "bad_request", r.message);
    case Status::NotFound: return error(404, "not_found", r.message);
    case Status::Busy: return error(409, "busy", r.message);
    case Status::Unavailable: return error(501, "unavailable", r.message);
    case Status::Error: break;
  }
  return error(500, "internal_error", r.message);
}

struct Target {
  std::vector<std::string> segments;
  std::vector<std::pair<std::string, std::string>> params;
  bool ok = true;
};

Target parse_target(std::string_view target) {
  Target t;
  std::string_view path = target, query;
  if (const auto q = target.find('?'); q != std::string_view::npos) {
    path = target.substr(0, q);
    query = target.substr(q + 1);
  }
  if (const auto h = query.find('#'); h != std::string_view::npos) query = query.substr(0, h);

  std::size_t pos = 0;
  while (pos <= path.size()) {
    const auto slash = path.find('/', pos);
    const auto end = slash == std::string_view::npos ? path.size() : slash;
    if (end > pos) {
      std::string seg;
      if (!percent_decode(path.substr(pos, end - pos), seg, false)) t.ok = false;
      t.segments.push_back(std::move(seg));
    }
    if (slash == std::string_view::npos) break;
    pos = slash + 1;
  }

  pos = 0;
  while (pos < query.size()) {
    auto amp = query.find('&', pos);
    if (amp == std::string_view::npos) amp = query.size();
    std::string_view kv = query.substr(pos, amp - pos);
    pos = amp + 1;
    if (kv.empty()) continue;
    const auto eq = kv.find('=');
    std::string k, v;
    if (!percent_decode(kv.substr(0, eq), k, true) ||
        (eq != std::string_view::npos && !percent_decode(kv.substr(eq + 1), v, true)))
      t.ok = false;
    t.params.emplace_back(std::move(k), std::move(v));
  }
  return t;
}

const std::string* find_param(const Target& t, std::string_view key) {
  for (auto it = t.params.rbegin(); it != t.params.rend(); ++it)
    if (it->first == key) return &it->second;
  return nullptr;
}

bool parse_bool(const std::string& s, bool& out) {
  if (s == "1" || s == "true") return out = true, true;
  if (s == "0" || s == "false" || s.empty()) return out = false, true;
  return false;
}

// ---- body parsing -----------------------------------------------------------------

struct AddBody {
  std::string query;
  std::uint32_t count = 1;
  bool also_global = false;
};

// Returns an empty string on success, otherwise the reason.
std::string parse_add_body(const std::string& body, bool allow_also_global, AddBody& out) {
  boost::system::error_code ec;
  const json::value v = json::parse(body, ec);
  if (ec || !v.is_object()) return "the body must be a JSON object";
  const json::object& o = v.get_object();
  for (const auto& kv : o) {
    const std::string_view k = kv.key();
    if (k != "query" && k != "count" && !(allow_also_global && k == "also_global"))
      return "unknown field \"" + std::string(k) + "\"";
  }
  const auto* q = o.if_contains("query");
  if (!q || !q->is_string()) return "\"query\" must be a string";
  out.query = std::string(q->get_string());
  if (const auto* c = o.if_contains("count")) {
    std::uint64_t n = 0;
    if (c->is_uint64())
      n = c->get_uint64();
    else if (c->is_int64() && c->get_int64() > 0)
      n = static_cast<std::uint64_t>(c->get_int64());
    else
      return "\"count\" must be a positive integer";
    if (n == 0 || n > std::numeric_limits<std::uint32_t>::max())
      return "\"count\" must be between 1 and 4294967295";
    out.count = static_cast<std::uint32_t>(n);
  }
  if (const auto* a = o.if_contains("also_global")) {
    if (!a->is_bool()) return "\"also_global\" must be a boolean";
    out.also_global = a->get_bool();
  }
  return {};
}

json::object suggestion_json(const Suggestion& s, bool user) {
  json::object o{{"id", s.id}, {"text", s.text}, {"freq", s.freq}};
  if (user) o["last_used"] = s.last_used;
  if (s.score) o["score"] = *s.score;
  o["matched_words"] = s.matched_words;
  return o;
}

ApiResponse add_response(const AddResult& r) {
  if (!r.ok()) return from_result(r);
  return json_response(r.created ? 201 : 200,
                       json::object{{"id", r.id}, {"text", r.text}, {"freq", r.freq}, {"created", r.created}});
}

ApiResponse maintenance_response(const MaintenanceResult& r) {
  if (!r.ok()) return from_result(r);
  return json_response(200, json::object{{"last_seq", r.last_seq},
                                         {"file_bytes", r.file_bytes},
                                         {"duration_ms", r.duration_ms},
                                         {"dead_queries_before", r.dead_queries_before}});
}

}  // namespace

ApiResponse Api::handle(const ApiRequest& req) const {
  ApiResponse resp;
  try {
    const Target t = parse_target(req.target);
    const auto& seg = t.segments;
    const std::string& m = req.method;

    auto method_not_allowed = [&](const char* allow) {
      ApiResponse r = error(405, "method_not_allowed", "use " + std::string(allow));
      r.headers.emplace_back("Allow", allow);
      return r;
    };

    if (!t.ok) {
      resp = error(400, "bad_request", "malformed percent-encoding in the request target");
    } else if (m == "OPTIONS") {
      resp.status = 204;
      resp.body.clear();
      resp.headers.emplace_back("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
      resp.headers.emplace_back("Access-Control-Allow-Headers", "Content-Type");
    } else if (seg.empty()) {
      if (m != "GET" && m != "HEAD") {
        resp = method_not_allowed("GET");
      } else if (cfg_.demo_page.empty()) {
        resp = error(404, "not_found", "no such resource");
      } else {
        resp.content_type = "text/html; charset=utf-8";
        resp.body = std::string(cfg_.demo_page);
      }
    } else if (seg.size() == 1 && seg[0] == "healthz") {
      resp = m == "GET" || m == "HEAD" ? json_response(200, json::object{{"status", "ok"}})
                                       : method_not_allowed("GET");
    } else if (seg[0] != "v1") {
      resp = error(404, "not_found", "no such resource");
    } else if (seg.size() == 2 && seg[1] == "complete") {
      if (m != "GET") {
        resp = method_not_allowed("GET");
      } else {
        CompleteRequest cr;
        cr.tail = cfg_.default_tail;
        cr.min_tail = cfg_.default_min_tail;
        std::string bad;
        const std::string* q = find_param(t, "q");
        if (!q) {
          bad = "missing parameter \"q\"";
        } else {
          cr.query = *q;
        }
        auto number = [&](const char* name, std::size_t cap, std::size_t& dst) {
          if (const auto* s = find_param(t, name)) {
            std::uint32_t v = 0;
            if (!parse_u32(*s, v) || v > cap)
              bad = std::string("\"") + name + "\" must be an integer between 0 and " + std::to_string(cap);
            else
              dst = v;
          }
        };
        number("n", cfg_.max_n, cr.n);
        number("m", cfg_.max_m, cr.m);
        const std::size_t max_words = engine_.text().limits().max_words;
        number("tail", max_words, cr.tail);
        const bool min_tail_given = find_param(t, "min_tail") != nullptr;
        number("min_tail", max_words, cr.min_tail);
        if (bad.empty() && min_tail_given && cr.min_tail == 0)
          bad = "\"min_tail\" must be at least 1";
        else if (bad.empty() && min_tail_given && cr.tail > 0 && cr.min_tail > cr.tail)
          bad = "\"min_tail\" must not be larger than \"tail\"";
        else if (!min_tail_given && cr.tail > 0 && cr.min_tail > cr.tail)
          cr.min_tail = cr.tail;  // a short tail with the default floor
        if (const auto* u = find_param(t, "user")) {
          std::uint32_t v = 0;
          if (!parse_u32(*u, v))
            bad = "\"user\" must be an unsigned 32-bit integer";
          else
            cr.user = v;
        }
        if (const auto* r = find_param(t, "rank")) {
          if (*r == "frequency" || r->empty())
            cr.rank = Index::GlobalRank::Frequency;
          else if (*r == "avg_word_freq")
            cr.rank = Index::GlobalRank::AverageWordFrequency;
          else
            bad = "\"rank\" must be frequency or avg_word_freq";
        }
        if (const auto* c = find_param(t, "correct"))
          if (!parse_bool(*c, cr.correct)) bad = "\"correct\" must be 0, 1, true or false";
        if (!bad.empty()) {
          resp = error(400, "bad_request", bad);
        } else {
          const CompleteResult r = engine_.complete(cr);
          if (!r.ok()) {
            resp = from_result(r);
          } else {
            json::array g, u;
            for (const auto& s : r.global) g.push_back(suggestion_json(s, false));
            for (const auto& s : r.user) u.push_back(suggestion_json(s, true));
            json::object o{{"global", std::move(g)}, {"user", std::move(u)},
                           {"truncated", r.truncated}, {"took_us", r.took_us}};
            if (r.corrected) o["corrected"] = *r.corrected;
            if (r.input_truncated) o["input_truncated"] = true;
            resp = json_response(200, o);
          }
        }
      }
    } else if (seg.size() == 2 && seg[1] == "queries") {
      if (m != "POST") {
        resp = method_not_allowed("POST");
      } else {
        AddBody b;
        const std::string why = parse_add_body(req.body, false, b);
        resp = why.empty() ? add_response(engine_.add_global(b.query, b.count))
                           : error(400, "bad_request", why);
      }
    } else if (seg.size() == 3 && seg[1] == "queries") {
      std::uint32_t id = 0;
      if (m != "DELETE")
        resp = method_not_allowed("DELETE");
      else if (!parse_u32(seg[2], id))
        resp = error(400, "bad_request", "the query id must be an unsigned 32-bit integer");
      else
        resp = from_result(engine_.remove_global(id));
    } else if (seg.size() >= 4 && seg[1] == "users" && seg[3] == "queries" && seg.size() <= 5) {
      std::uint32_t user = 0;
      if (!parse_u32(seg[2], user)) {
        resp = error(400, "bad_request", "the user id must be an unsigned 32-bit integer");
      } else if (seg.size() == 4) {
        if (m != "POST") {
          resp = method_not_allowed("POST");
        } else {
          AddBody b;
          const std::string why = parse_add_body(req.body, true, b);
          resp = why.empty() ? add_response(engine_.add_user(user, b.query, b.count, b.also_global))
                             : error(400, "bad_request", why);
        }
      } else {
        std::uint32_t id = 0;
        if (m != "DELETE")
          resp = method_not_allowed("DELETE");
        else if (!parse_u32(seg[4], id))
          resp = error(400, "bad_request", "the query id must be an unsigned 32-bit integer");
        else
          resp = from_result(engine_.remove_user(user, id));
      }
    } else if (seg.size() == 2 && (seg[1] == "snapshot" || seg[1] == "optimize")) {
      if (m != "POST")
        resp = method_not_allowed("POST");
      else if (!cfg_.allow_admin)
        resp = error(403, "forbidden", "maintenance endpoints are disabled");
      else
        resp = maintenance_response(seg[1] == "snapshot" ? engine_.snapshot() : engine_.optimize());
    } else if (seg.size() == 2 && seg[1] == "stats") {
      if (m != "GET") {
        resp = method_not_allowed("GET");
      } else {
        const EngineStats s = engine_.stats();
        resp = json_response(
            200, json::object{{"words", s.index.words},
                              {"query_ids_allocated", s.index.query_ids_allocated},
                              {"live_queries", s.index.live_queries},
                              {"global_queries", s.index.global_queries},
                              {"user_entries", s.index.user_entries},
                              {"users", s.index.users},
                              {"dead_queries", s.index.dead_queries},
                              {"arena_bytes", s.index.arena_bytes},
                              {"last_seq", s.last_seq},
                              {"wal_bytes", s.wal_bytes},
                              {"snapshot_seq", s.snapshot_seq},
                              {"snapshot_bytes", s.snapshot_bytes},
                              {"snapshot_unix", s.snapshot_unix},
                              {"cache_hits", s.cache_hits},
                              {"cache_misses", s.cache_misses},
                              {"completes", s.completes},
                              {"hot_lookups", s.hot_lookups},
                              {"hot_prefixes", s.index.hot_prefixes},
                              {"hot_entries", s.index.hot_entries},
                              {"writes", s.writes},
                              {"recovered_records", s.recovered_records},
                              {"maintenance_running", s.maintenance_running},
                              {"uptime_s", s.uptime_s},
                              {"spell_correction", engine_.has_spell_fixer()}});
      }
    } else {
      resp = error(404, "not_found", "no such resource");
    }
  } catch (const std::exception& e) {
    log_error(std::string("request failed: ") + e.what());
    resp = error(500, "internal_error", "internal error");
  } catch (...) {
    resp = error(500, "internal_error", "internal error");
  }
  if (cfg_.cors) resp.headers.emplace_back("Access-Control-Allow-Origin", "*");
  return resp;
}

}  // namespace qac
