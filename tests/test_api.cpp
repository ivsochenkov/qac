#include <gtest/gtest.h>

#include <boost/json.hpp>

#include "qac/api.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;
namespace json = boost::json;

namespace {

const Alphabet& alphabet() {
  static const Alphabet a = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  return a;
}

struct ApiFixture : ::testing::Test {
  TempDir dir;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<Api> api;

  void SetUp() override {
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
  }

  ApiResponse call(const std::string& method, const std::string& target, const std::string& body = "") {
    return api->handle({method, target, body});
  }
  static json::value parse(const ApiResponse& r) {
    boost::system::error_code ec;
    json::value v = json::parse(r.body, ec);
    EXPECT_FALSE(ec) << "response is not valid JSON: " << r.body;
    return v;
  }
  std::uint32_t add(const std::string& q, int count = 1) {
    const auto r = call("POST", "/v1/queries", R"({"query":")" + q + R"(","count":)" + std::to_string(count) + "}");
    EXPECT_TRUE(r.status == 200 || r.status == 201) << r.body;
    return static_cast<std::uint32_t>(parse(r).at("id").as_int64());
  }
  static std::vector<std::string> texts(const json::value& v, const char* pool) {
    std::vector<std::string> out;
    for (const auto& e : v.at(pool).as_array()) out.emplace_back(e.at("text").as_string());
    return out;
  }
};

}  // namespace

TEST(ApiHelpers, PercentDecoding) {
  std::string out;
  EXPECT_TRUE(percent_decode("a%20b+c%D0%BF", out, true));
  EXPECT_EQ(out, "a b cп");
  EXPECT_TRUE(percent_decode("a+b", out, false));
  EXPECT_EQ(out, "a+b");
  EXPECT_FALSE(percent_decode("%", out, true));
  EXPECT_FALSE(percent_decode("%4", out, true));
  EXPECT_FALSE(percent_decode("%zz", out, true));
  EXPECT_TRUE(percent_decode("%41", out, true));
  EXPECT_EQ(out, "A");
}

TEST(ApiHelpers, ParseU32IsStrict) {
  std::uint32_t v = 0;
  EXPECT_TRUE(parse_u32("0", v));
  EXPECT_TRUE(parse_u32("4294967295", v));
  EXPECT_EQ(v, 4294967295u);
  for (const char* bad : {"", "-1", "+1", "4294967296", "12a", " 1", "99999999999", "1.5"})
    EXPECT_FALSE(parse_u32(bad, v)) << bad;
}

TEST_F(ApiFixture, AddAndCompleteGlobal) {
  auto r = call("POST", "/v1/queries", R"({"query":"New York weather","count":3})");
  EXPECT_EQ(r.status, 201);
  auto v = parse(r);
  EXPECT_EQ(v.at("text").as_string(), "new york weather");
  EXPECT_EQ(v.at("freq").as_int64(), 3);
  EXPECT_TRUE(v.at("created").as_bool());

  r = call("POST", "/v1/queries", R"({"query":"new york weather"})");
  EXPECT_EQ(r.status, 200);  // existing query
  EXPECT_EQ(parse(r).at("freq").as_int64(), 4);
  add("new york times");

  r = call("GET", "/v1/complete?q=new%20york%20");
  ASSERT_EQ(r.status, 200) << r.body;
  v = parse(r);
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"new york weather", "new york times"}));
  EXPECT_EQ(v.at("global").as_array()[0].at("freq").as_int64(), 4);
  EXPECT_TRUE(v.at("user").as_array().empty());
  EXPECT_FALSE(v.as_object().contains("corrected"));
  EXPECT_FALSE(v.at("truncated").as_bool());

  // '+' is a space in a query string, a partial last word is completed, n limits.
  r = call("GET", "/v1/complete?q=new+york+w&n=1");
  EXPECT_EQ(texts(parse(r), "global"), (std::vector<std::string>{"new york weather"}));
  r = call("GET", "/v1/complete?q=new+york+&n=1");
  EXPECT_EQ(parse(r).at("global").as_array().size(), 1u);
}

TEST_F(ApiFixture, UserHistoryAndRemoval) {
  auto r = call("POST", "/v1/users/42/queries", R"({"query":"my secret plan","count":2})");
  ASSERT_EQ(r.status, 201) << r.body;
  const auto id = static_cast<std::uint32_t>(parse(r).at("id").as_int64());
  call("POST", "/v1/users/7/queries", R"({"query":"my other plan","also_global":true})");

  r = call("GET", "/v1/complete?q=my+&user=42&n=10&m=5");
  ASSERT_EQ(r.status, 200);
  auto v = parse(r);
  EXPECT_EQ(texts(v, "user"), (std::vector<std::string>{"my secret plan"}));
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"my other plan"}));  // also_global only for user 7
  EXPECT_EQ(v.at("user").as_array()[0].at("freq").as_int64(), 2);
  EXPECT_TRUE(v.at("user").as_array()[0].as_object().contains("last_used"));

  r = call("GET", "/v1/complete?q=&user=42");  // empty input returns the user's history
  EXPECT_EQ(texts(parse(r), "user"), (std::vector<std::string>{"my secret plan"}));

  EXPECT_EQ(call("DELETE", "/v1/users/42/queries/" + std::to_string(id)).status, 200);
  EXPECT_EQ(call("DELETE", "/v1/users/42/queries/" + std::to_string(id)).status, 404);
  EXPECT_TRUE(texts(parse(call("GET", "/v1/complete?q=my+&user=42")), "user").empty());
}

TEST_F(ApiFixture, RemoveFromGlobalPool) {
  const auto id = add("remove me");
  add("remove other");
  EXPECT_EQ(call("DELETE", "/v1/queries/" + std::to_string(id)).status, 200);
  EXPECT_EQ(call("DELETE", "/v1/queries/" + std::to_string(id)).status, 404);
  EXPECT_EQ(texts(parse(call("GET", "/v1/complete?q=remove+")), "global"),
            (std::vector<std::string>{"remove other"}));
  EXPECT_EQ(call("DELETE", "/v1/queries/abc").status, 400);
  EXPECT_EQ(call("DELETE", "/v1/queries/99999999999").status, 400);
}

TEST_F(ApiFixture, RejectsBadAdds) {
  for (const char* body : {"", "not json", "[]", R"({"query":5})", R"({})", R"({"query":"x","count":0})",
                           R"({"query":"x","count":-3})", R"({"query":"x","count":1.5})",
                           R"({"query":"x","count":4294967296})", R"({"query":"x","extra":1})",
                           R"({"query":"x","also_global":true})",  // not allowed on the global endpoint
                           R"({"query":"日本語"})", R"({"query":"   "})"}) {
    const auto r = call("POST", "/v1/queries", body);
    EXPECT_EQ(r.status, 400) << body << " -> " << r.body;
    EXPECT_EQ(parse(r).at("error").as_string(), "bad_request");
  }
  EXPECT_EQ(call("POST", "/v1/users/abc/queries", R"({"query":"x"})").status, 400);
  EXPECT_EQ(call("POST", "/v1/users/-1/queries", R"({"query":"x"})").status, 400);
  EXPECT_EQ(call("POST", "/v1/users/1/queries", R"({"query":"x","also_global":"yes"})").status, 400);
  EXPECT_EQ(engine->stats().index.live_queries, 0u);  // nothing leaked in
}

TEST_F(ApiFixture, RejectsBadCompleteParameters) {
  EXPECT_EQ(call("GET", "/v1/complete").status, 400);  // q is required
  for (const char* t : {"/v1/complete?q=a&n=-1", "/v1/complete?q=a&n=101", "/v1/complete?q=a&n=x",
                        "/v1/complete?q=a&m=1000", "/v1/complete?q=a&user=-2", "/v1/complete?q=a&correct=maybe",
                        "/v1/complete?q=%zz", "/v1/complete?q=%FF%FE", "/v1/complete?q=%E6%97%A5"}) {
    const auto r = call("GET", t);
    EXPECT_EQ(r.status, 400) << t << " -> " << r.body;
    parse(r);  // the error document is valid JSON even for undecodable input
  }
  EXPECT_EQ(call("GET", "/v1/complete?q=a&correct=1").status, 501);  // no model attached
  EXPECT_EQ(call("GET", "/v1/complete?q=a&correct=0").status, 200);
}

TEST_F(ApiFixture, SpellCorrectionIsOptIn) {
  struct Fixer : SpellFixer {
    std::string fix(const std::string& s) const override {
      return s == "helo" ? "hello" : s;
    }
  };
  engine->set_spell_fixer(std::make_shared<Fixer>());
  add("hello world");
  auto v = parse(call("GET", "/v1/complete?q=helo+w&correct=true"));
  EXPECT_EQ(v.at("corrected").as_string(), "hello w");
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"hello world"}));
  v = parse(call("GET", "/v1/complete?q=helo+w"));
  EXPECT_FALSE(v.as_object().contains("corrected"));
  EXPECT_TRUE(v.at("global").as_array().empty());
}

TEST_F(ApiFixture, RoutingAndMethods) {
  auto r = call("GET", "/v1/queries");
  EXPECT_EQ(r.status, 405);
  ASSERT_FALSE(r.headers.empty());
  EXPECT_EQ(r.headers[0].first, "Allow");
  EXPECT_EQ(r.headers[0].second, "POST");
  EXPECT_EQ(call("POST", "/v1/complete").status, 405);
  EXPECT_EQ(call("PUT", "/v1/queries/1").status, 405);
  EXPECT_EQ(call("GET", "/nope").status, 404);
  EXPECT_EQ(call("GET", "/v2/complete").status, 404);
  EXPECT_EQ(call("GET", "/v1/users/1/other").status, 404);
  EXPECT_EQ(call("GET", "/v1/complete/?q=a").status, 200);  // trailing slash tolerated

  r = call("GET", "/");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.content_type, "text/html; charset=utf-8");
  EXPECT_EQ(r.body, "<html>demo</html>");
  EXPECT_EQ(parse(call("GET", "/healthz")).at("status").as_string(), "ok");
}

TEST_F(ApiFixture, StatsSnapshotAndOptimize) {
  add("alpha beta", 2);
  add("alpha gamma");
  auto v = parse(call("GET", "/v1/stats"));
  EXPECT_EQ(v.at("global_queries").as_int64(), 2);
  EXPECT_EQ(v.at("writes").as_int64(), 2);
  EXPECT_FALSE(v.at("spell_correction").as_bool());

  auto r = call("POST", "/v1/snapshot");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_GT(parse(r).at("file_bytes").as_int64(), 0);

  const auto id = add("to be removed");
  EXPECT_EQ(call("DELETE", "/v1/queries/" + std::to_string(id)).status, 200);
  r = call("POST", "/v1/optimize");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(parse(r).at("dead_queries_before").as_int64(), 1);
  EXPECT_EQ(parse(call("GET", "/v1/stats")).at("dead_queries").as_int64(), 0);
  EXPECT_EQ(call("GET", "/v1/snapshot").status, 405);
}

TEST_F(ApiFixture, AdminEndpointsCanBeDisabled) {
  ApiConfig ac;
  ac.allow_admin = false;
  Api locked(*engine, ac);
  EXPECT_EQ(locked.handle({"POST", "/v1/snapshot", ""}).status, 403);
  EXPECT_EQ(locked.handle({"POST", "/v1/optimize", ""}).status, 403);
  EXPECT_EQ(locked.handle({"GET", "/v1/stats", ""}).status, 200);
}

TEST_F(ApiFixture, CorsHeaders) {
  ApiConfig ac;
  ac.cors = true;
  Api cors(*engine, ac);
  auto r = cors.handle({"GET", "/healthz", ""});
  ASSERT_FALSE(r.headers.empty());
  EXPECT_EQ(r.headers.back().first, "Access-Control-Allow-Origin");
  r = cors.handle({"OPTIONS", "/v1/queries", ""});
  EXPECT_EQ(r.status, 204);
  EXPECT_TRUE(r.body.empty());
  EXPECT_TRUE(call("GET", "/healthz").headers.empty());  // off by default
}

TEST_F(ApiFixture, Utf8RoundTrip) {
  EXPECT_EQ(call("POST", "/v1/queries", R"({"query":"Привет, МИР","count":2})").status, 201);
  const auto v = parse(call("GET", "/v1/complete?q=%D0%BF%D1%80%D0%B8"));  // "при"
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"привет мир"}));
}

TEST_F(ApiFixture, RankByAverageWordFrequency) {
  add("apple", 100);
  add("apple pie", 10);
  add("apple tart", 1);
  add("apple juice", 1);
  add("juice", 1000);

  // default: by query frequency, no score in the response
  auto v = parse(call("GET", "/v1/complete?q=apple+"));
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"apple", "apple pie", "apple tart", "apple juice"}));
  EXPECT_FALSE(v.at("global").as_array()[0].as_object().contains("score"));
  v = parse(call("GET", "/v1/complete?q=apple+&rank=frequency"));
  EXPECT_EQ(texts(v, "global").front(), "apple");
  EXPECT_FALSE(v.at("global").as_array()[0].as_object().contains("score"));

  // by the mean frequency of the words, with the score
  auto r = call("GET", "/v1/complete?q=apple+&rank=avg_word_freq");
  ASSERT_EQ(r.status, 200) << r.body;
  v = parse(r);
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"apple juice", "apple", "apple pie", "apple tart"}));
  const auto& top = v.at("global").as_array()[0];
  EXPECT_DOUBLE_EQ(top.at("score").to_number<double>(), 556.5);
  EXPECT_EQ(top.at("freq").as_int64(), 1);  // the query's own frequency is still there

  // n applies after ranking; a partial last word works too
  v = parse(call("GET", "/v1/complete?q=app&n=2&rank=avg_word_freq"));
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"apple juice", "apple"}));

  // "rank=" is the default; anything else is an error
  EXPECT_EQ(texts(parse(call("GET", "/v1/complete?q=apple+&rank=")), "global").front(), "apple");
  for (const char* bad : {"rank=avg", "rank=AVG_WORD_FREQ", "rank=frequency,avg_word_freq", "rank=1"}) {
    const auto e = call("GET", std::string("/v1/complete?q=apple+&") + bad);
    EXPECT_EQ(e.status, 400) << bad;
    EXPECT_EQ(parse(e).at("error").as_string(), "bad_request");
  }
}

TEST_F(ApiFixture, RankDoesNotChangeTheUserList) {
  add("apple", 100);
  add("apple juice", 1);
  add("juice", 1000);
  call("POST", "/v1/users/3/queries", R"({"query":"apple crumble","count":4})");
  const auto plain = parse(call("GET", "/v1/complete?q=apple+&user=3"));
  const auto avg = parse(call("GET", "/v1/complete?q=apple+&user=3&rank=avg_word_freq"));
  EXPECT_EQ(texts(plain, "user"), texts(avg, "user"));
  EXPECT_FALSE(avg.at("user").as_array()[0].as_object().contains("score"));
  EXPECT_NE(texts(plain, "global"), texts(avg, "global"));
}

// ================================================================== tail fallback over HTTP

TEST_F(ApiFixture, TailFallbackIsOnByDefaultAndReportsMatchedWords) {
  add("red foxes jump across the trees", 5);
  add("foxes juggernaut", 2);
  const std::string q = "/v1/complete?q=Long+time+ago++far+in+the+Galaxy+red+foxes+ju";

  auto r = call("GET", q);
  ASSERT_EQ(r.status, 200) << r.body;
  auto v = parse(r);
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"red foxes jump across the trees", "foxes juggernaut"}));
  EXPECT_EQ(v.at("global").as_array()[0].at("matched_words").as_int64(), 3);
  EXPECT_EQ(v.at("global").as_array()[1].at("matched_words").as_int64(), 2);
  EXPECT_FALSE(v.as_object().contains("input_truncated"));

  EXPECT_TRUE(texts(parse(call("GET", q + "&tail=0")), "global").empty());
  EXPECT_EQ(texts(parse(call("GET", q + "&tail=2")), "global"), (std::vector<std::string>{"foxes juggernaut"}));
  EXPECT_EQ(texts(parse(call("GET", q + "&tail=3&min_tail=3")), "global"),
            (std::vector<std::string>{"red foxes jump across the trees"}));
  EXPECT_EQ(call("GET", q + "&tail=1").status, 200);  // the default floor yields to a short tail

  // an ordinary lookup reports the whole input
  v = parse(call("GET", "/v1/complete?q=foxes+ju"));
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"foxes juggernaut"}));
  EXPECT_EQ(v.at("global").as_array()[0].at("matched_words").as_int64(), 2);
}

TEST_F(ApiFixture, RejectsBadTailParameters) {
  for (const char* t : {"/v1/complete?q=a&tail=33", "/v1/complete?q=a&tail=-1", "/v1/complete?q=a&tail=x",
                        "/v1/complete?q=a&min_tail=0", "/v1/complete?q=a&min_tail=x",
                        "/v1/complete?q=a&tail=2&min_tail=3", "/v1/complete?q=a&min_tail=40"}) {
    const auto r = call("GET", t);
    EXPECT_EQ(r.status, 400) << t << " -> " << r.body;
    EXPECT_EQ(parse(r).at("error").as_string(), "bad_request");
  }
  EXPECT_EQ(call("GET", "/v1/complete?q=a&tail=0&min_tail=3").status, 200);  // the floor is moot when off
}

TEST_F(ApiFixture, OverLongInputIsUsedFromItsEndAndFlagged) {
  add("red foxes jump", 3);
  std::string q;
  for (int i = 0; i < 40; ++i) q += "bla+";
  const auto r = call("GET", "/v1/complete?q=" + q + "red+foxes+ju");
  ASSERT_EQ(r.status, 200) << r.body;
  const auto v = parse(r);
  EXPECT_EQ(texts(v, "global"), (std::vector<std::string>{"red foxes jump"}));
  ASSERT_TRUE(v.as_object().contains("input_truncated"));
  EXPECT_TRUE(v.at("input_truncated").as_bool());
  // still an error when what is kept is not text
  EXPECT_EQ(call("GET", "/v1/complete?q=" + q + "%E6%97%A5").status, 400);
}

TEST_F(ApiFixture, ServerWideTailDefaultsCanBeSet) {
  add("red foxes jump", 3);
  ApiConfig ac;
  ac.default_tail = 0;  // the operator switched the fallback off
  Api off(*engine, ac);
  const std::string q = "/v1/complete?q=long+ago+red+foxes+ju";
  EXPECT_TRUE(texts(parse(off.handle({"GET", q, ""})), "global").empty());
  EXPECT_EQ(texts(parse(off.handle({"GET", q + "&tail=3", ""})), "global"),
            (std::vector<std::string>{"red foxes jump"}));  // a request can still ask for it
  EXPECT_EQ(texts(parse(call("GET", q)), "global"), (std::vector<std::string>{"red foxes jump"}));
}

TEST_F(ApiFixture, StatsReportTheHotPrefixTable) {
  add("apple", 2);
  add("apricot", 1);
  EXPECT_EQ(texts(parse(call("GET", "/v1/complete?q=ap")), "global"), (std::vector<std::string>{"apple", "apricot"}));
  const auto s = parse(call("GET", "/v1/stats"));
  EXPECT_GE(s.at("hot_lookups").as_int64(), 1);
  EXPECT_GE(s.at("hot_prefixes").as_int64(), 2);
  EXPECT_GE(s.at("hot_entries").as_int64(), 4);
}
