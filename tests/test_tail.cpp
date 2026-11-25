// Engine-level tests of the tail fallback (CompleteRequest::tail / min_tail), the handling of
// over-long input, spell correction of the tail only, and the hot-prefix table.
#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "qac/engine.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;

namespace {

const Alphabet& alphabet() {
  static const Alphabet a = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  return a;
}

EngineConfig make_config(const TempDir& dir, SnapshotMode mode = SnapshotMode::Inline,
                         const std::string& sub = "data") {
  EngineConfig c;
  c.data_dir = dir.sub(sub);
  c.fsync = FsyncPolicy::Never;
  c.snapshot_mode = mode;
  c.snapshot_interval_s = 0;
  c.snapshot_wal_bytes = 0;
  c.cache_entries = 0;
  return c;
}

CompleteResult run(const Engine& e, const std::string& q, std::size_t tail = 3,
                   std::size_t min_tail = 2, std::size_t n = 10) {
  CompleteRequest r;
  r.query = q;
  r.n = n;
  r.m = 0;
  r.tail = tail;
  r.min_tail = min_tail;
  return e.complete(r);
}

CompleteResult run_user(const Engine& e, std::uint32_t user, const std::string& q,
                        std::size_t tail = 3, std::size_t min_tail = 2, std::size_t m = 10) {
  CompleteRequest r;
  r.query = q;
  r.n = 0;
  r.m = m;
  r.user = user;
  r.tail = tail;
  r.min_tail = min_tail;
  return e.complete(r);
}

std::vector<std::string> texts(const std::vector<Suggestion>& v) {
  std::vector<std::string> out;
  for (const auto& s : v) out.push_back(s.text);
  return out;
}

std::vector<std::uint32_t> matched(const std::vector<Suggestion>& v) {
  std::vector<std::uint32_t> out;
  for (const auto& s : v) out.push_back(s.matched_words);
  return out;
}

using Strings = std::vector<std::string>;
using Numbers = std::vector<std::uint32_t>;

void fill_foxes(Engine& e) {
  ASSERT_TRUE(e.add_global("red foxes jump across the trees", 5).ok());
  ASSERT_TRUE(e.add_global("foxes juggernaut", 2).ok());
}

}  // namespace

// ================================================================== tail fallback

TEST(TailFallback, FindsWhatTheWholeInputCannot) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  fill_foxes(e);
  const std::string typed = "Long time ago  far in the Galaxy red foxes ju";

  auto r = run(e, typed);  // defaults: tail 3, down to 2
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(texts(r.global), (Strings{"red foxes jump across the trees", "foxes juggernaut"}));
  EXPECT_EQ(matched(r.global), (Numbers{3, 2}));
  EXPECT_FALSE(r.truncated);
  EXPECT_FALSE(r.input_truncated);

  EXPECT_TRUE(run(e, typed, 0).global.empty());  // the fallback off: nothing, as before

  r = run(e, typed, 3, 3);  // never shorter than 3 words
  EXPECT_EQ(texts(r.global), (Strings{"red foxes jump across the trees"}));
  EXPECT_EQ(matched(r.global), (Numbers{3}));

  r = run(e, typed, 2);  // only the last two words
  EXPECT_EQ(texts(r.global), (Strings{"foxes juggernaut"}));
  EXPECT_EQ(matched(r.global), (Numbers{2}));
}

TEST(TailFallback, AnUnchangedLookupReportsTheWholeInput) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_global("new york", 5).ok());
  ASSERT_TRUE(e.add_global("new york weather", 9).ok());
  ASSERT_TRUE(e.add_global("new zealand", 2).ok());

  for (const char* typed : {"new yo", "new ", "ne", "new york "}) {
    const auto with = run(e, typed);
    const auto without = run(e, typed, 0);
    ASSERT_TRUE(with.ok());
    EXPECT_EQ(texts(with.global), texts(without.global)) << typed;  // same answers as before
    for (const auto& s : with.global) EXPECT_GE(s.matched_words, 1u) << typed;
  }
  EXPECT_EQ(matched(run(e, "new yo").global), (Numbers{2, 2}));
  EXPECT_EQ(matched(run(e, "ne").global), (Numbers{1, 1, 1}));
}

TEST(TailFallback, ShorterTailsFillTheRemainingPlacesSpecificFirst) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_global("big red foxes jump high", 1).ok());
  ASSERT_TRUE(e.add_global("red foxes jump", 10).ok());
  ASSERT_TRUE(e.add_global("red foxes jumpy", 9).ok());
  ASSERT_TRUE(e.add_global("foxes juggle", 20).ok());

  // n = 3: the whole input gives one, the last 3 words give two more: enough
  auto r = run(e, "big red foxes ju", 3, 2, 3);
  EXPECT_EQ(texts(r.global), (Strings{"big red foxes jump high", "red foxes jump", "red foxes jumpy"}));
  EXPECT_EQ(matched(r.global), (Numbers{4, 3, 3}));

  // n = 10: also the last 2 words, listed after the more specific matches even though the
  // query is far more frequent
  r = run(e, "big red foxes ju", 3, 2, 10);
  EXPECT_EQ(texts(r.global), (Strings{"big red foxes jump high", "red foxes jump", "red foxes jumpy", "foxes juggle"}));
  EXPECT_EQ(matched(r.global), (Numbers{4, 3, 3, 2}));
}

TEST(TailFallback, AQueryIsListedOnceByTheMostSpecificLookup) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_global("a b a b c", 5).ok());
  // "a b a b " finds it as a whole and again through its last two words "a b "
  const auto r = run(e, "a b a b ");
  EXPECT_EQ(texts(r.global), (Strings{"a b a b c"}));
  EXPECT_EQ(matched(r.global), (Numbers{4}));
}

TEST(TailFallback, EnoughResultsStopTheFallback) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_global("red foxes jump", 1).ok());
  ASSERT_TRUE(e.add_global("big red foxes jump", 1).ok());
  const auto r = run(e, "big red foxes ju", 3, 2, 1);  // one place, taken by the whole input
  EXPECT_EQ(texts(r.global), (Strings{"big red foxes jump"}));
  EXPECT_EQ(matched(r.global), (Numbers{4}));
}

TEST(TailFallback, UnknownWordsOnlyDisableTheLookupsContainingThem) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  fill_foxes(e);
  // before the fallback one unknown word anywhere meant no answer at all
  EXPECT_FALSE(run(e, "zzzz red foxes ju").global.empty());
  EXPECT_FALSE(run(e, "qqqq zzzz red foxes ju").global.empty());
  // an unknown word inside the tail window takes the lookups that contain it with it
  EXPECT_TRUE(run(e, "red qqqq ju").global.empty());
  EXPECT_TRUE(run(e, "red foxes qqqq ").global.empty());
  // a partial word nothing starts with matches nothing, with or without the fallback
  EXPECT_TRUE(run(e, "red foxes qzx").global.empty());
}

TEST(TailFallback, MinTailSetsTheFloor) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_global("juggernaut x", 4).ok());
  ASSERT_TRUE(e.add_global("jumbo y", 2).ok());

  EXPECT_TRUE(run(e, "zzz yyy ju", 3, 2).global.empty());  // one word is below the floor
  const auto r = run(e, "zzz yyy ju", 3, 1);               // down to the word being typed alone
  EXPECT_EQ(texts(r.global), (Strings{"juggernaut x", "jumbo y"}));
  EXPECT_EQ(matched(r.global), (Numbers{1, 1}));

  // a floor above the tail is clamped to it; 0 is clamped to 1
  EXPECT_EQ(texts(run(e, "zzz yyy ju", 2, 5).global), texts(run(e, "zzz yyy ju", 2, 2).global));
  EXPECT_EQ(texts(run(e, "zzz yyy ju", 2, 0).global), texts(run(e, "zzz yyy ju", 2, 1).global));
  // a two-word input has no shorter lookup above the default floor
  EXPECT_TRUE(run(e, "zzz ju").global.empty());
}

TEST(TailFallback, WorksWhenTheInputEndsInACompleteWord) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  fill_foxes(e);
  const auto r = run(e, "blah blah red foxes ");  // trailing space: nothing is being typed
  EXPECT_EQ(texts(r.global), (Strings{"red foxes jump across the trees"}));
  EXPECT_EQ(matched(r.global), (Numbers{2}));
}

TEST(TailFallback, TheUserHistoryGetsTheSameFallback) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_user(7, "red foxes jump high", 3).ok());
  ASSERT_TRUE(e.add_user(7, "foxes juggle", 1).ok());
  ASSERT_TRUE(e.add_user(8, "red foxes jump low", 1).ok());

  auto r = run_user(e, 7, "far far away red foxes ju");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(texts(r.user), (Strings{"red foxes jump high", "foxes juggle"}));
  EXPECT_EQ(matched(r.user), (Numbers{3, 2}));

  EXPECT_TRUE(run_user(e, 7, "far far away red foxes ju", 0).user.empty());
  EXPECT_EQ(texts(run_user(e, 8, "far far away red foxes ju").user), (Strings{"red foxes jump low"}));
  EXPECT_TRUE(run_user(e, 9, "far far away red foxes ju").user.empty());  // someone else's history
}

TEST(TailFallback, GlobalAndUserListsFallBackIndependently) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  ASSERT_TRUE(e.add_global("red foxes jump", 4).ok());
  ASSERT_TRUE(e.add_user(7, "foxes juggle", 1).ok());
  CompleteRequest req;
  req.query = "long ago red foxes ju";
  req.n = 5;
  req.m = 5;
  req.user = 7;
  const auto r = e.complete(req);
  EXPECT_EQ(texts(r.global), (Strings{"red foxes jump"}));
  EXPECT_EQ(matched(r.global), (Numbers{3}));
  EXPECT_EQ(texts(r.user), (Strings{"foxes juggle"}));
  EXPECT_EQ(matched(r.user), (Numbers{2}));
}

// ================================================================== over-long input

TEST(LongInput, OnlyTheEndIsUsedInsteadOfAnError) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  fill_foxes(e);

  std::string many_words;
  for (int i = 0; i < 40; ++i) many_words += "bla ";  // more than the 32 words a lookup takes
  auto r = run(e, many_words + "red foxes ju");
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_TRUE(r.input_truncated);
  EXPECT_EQ(texts(r.global), (Strings{"red foxes jump across the trees", "foxes juggernaut"}));
  EXPECT_EQ(matched(r.global), (Numbers{3, 2}));

  std::string many_bytes;
  for (int i = 0; i < 600; ++i) many_bytes += "x ";  // 1200 bytes, over the 1024 allowed
  r = run(e, many_bytes + "red foxes ju");
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_TRUE(r.input_truncated);
  EXPECT_EQ(texts(r.global), (Strings{"red foxes jump across the trees", "foxes juggernaut"}));

  EXPECT_FALSE(run(e, "red foxes ju").input_truncated);
}

TEST(LongInput, MalformedInputIsStillRejected) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  EXPECT_EQ(run(e, std::string(100, 'a')).status, Status::BadInput);  // a word that long cannot exist
  EXPECT_EQ(run(e, "abc \xFF\xFE").status, Status::BadInput);         // not UTF-8
  EXPECT_EQ(run(e, "abc 日本").status, Status::BadInput);              // outside the alphabet
  // outside the alphabet but cut off with the head of a long input: not looked at
  std::string head = "日本 ";
  for (int i = 0; i < 40; ++i) head += "bla ";
  EXPECT_TRUE(run(e, head + "ab").ok());
  // writes stay strict
  EXPECT_EQ(e.add_global(std::string(2000, 'a') + " b", 1).status, Status::BadInput);
}

// ================================================================== scan budget and cache

TEST(TailBudget, OneBudgetServesAllTheLookupsOfARequest) {
  TempDir dir;
  EngineConfig cfg = make_config(dir);
  cfg.scan.max_visited = 3;
  Engine e(alphabet(), cfg);
  for (int i = 0; i < 6; ++i)
    ASSERT_TRUE(e.add_global("red foxes jump " + std::string(1, static_cast<char>('a' + i)), 10 + i).ok());
  ASSERT_TRUE(e.add_global("foxes jump rope", 1).ok());

  // the whole input uses the budget on its 6 matches; no budget is left for the last 2 words
  auto r = run(e, "red foxes jump ", 3, 2, 10);
  EXPECT_EQ(r.global.size(), 3u);
  EXPECT_TRUE(r.truncated);
  for (const auto& s : r.global) EXPECT_EQ(s.matched_words, 3u);

  EngineConfig roomy = make_config(dir, SnapshotMode::Inline, "roomy");
  Engine e2(alphabet(), roomy);
  for (int i = 0; i < 6; ++i)
    ASSERT_TRUE(e2.add_global("red foxes jump " + std::string(1, static_cast<char>('a' + i)), 10 + i).ok());
  ASSERT_TRUE(e2.add_global("foxes jump rope", 1).ok());
  r = run(e2, "red foxes jump ", 3, 2, 10);
  EXPECT_EQ(r.global.size(), 7u);  // with room for all: the 6 and then "foxes jump rope"
  EXPECT_EQ(r.global.back().text, "foxes jump rope");
  EXPECT_EQ(r.global.back().matched_words, 2u);
  EXPECT_FALSE(r.truncated);
}

TEST(TailCache, EveryLookupIsCachedOnItsOwn) {
  TempDir dir;
  EngineConfig cfg = make_config(dir);
  cfg.cache_entries = 100;
  cfg.cache_ttl_ms = 60000;
  Engine e(alphabet(), cfg);
  fill_foxes(e);
  ASSERT_TRUE(e.add_global("new york", 1).ok());
  ASSERT_TRUE(e.add_global("old york", 1).ok());

  const auto first = run(e, "new red foxes ju");
  EXPECT_EQ(texts(first.global), (Strings{"red foxes jump across the trees", "foxes juggernaut"}));
  const auto hits_before = e.stats().cache_hits;
  // a different front, the same last three words: that lookup comes from the cache
  const auto second = run(e, "old red foxes ju");
  EXPECT_EQ(texts(second.global), texts(first.global));
  EXPECT_EQ(matched(second.global), matched(first.global));
  EXPECT_GE(e.stats().cache_hits, hits_before + 2);  // the 3-word and the 2-word lookup

  // and the cache never changes an answer
  TempDir dir2;
  Engine plain(alphabet(), make_config(dir2));
  fill_foxes(plain);
  EXPECT_EQ(texts(run(plain, "old red foxes ju").global), texts(second.global));
}

// ================================================================== spell correction

namespace {

class RecordingFixer : public SpellFixer {
 public:
  std::string fix(const std::string& text) const override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      calls_.push_back(text);
    }
    std::string out, word;
    const std::map<std::string, std::string> fixes{{"teh", "the"}, {"wrold", "world"}};
    for (std::size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == ' ') {
        const auto it = fixes.find(word);
        if (!out.empty()) out += ' ';
        out += it == fixes.end() ? word : it->second;
        word.clear();
      } else {
        word += text[i];
      }
    }
    return out;
  }
  std::vector<std::string> calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return calls_;
  }

 private:
  mutable std::mutex mu_;
  mutable std::vector<std::string> calls_;
};

}  // namespace

TEST(TailCorrection, OnlyTheWordsTheLookupCanUseAreCorrected) {
  TempDir dir;
  Engine e(alphabet(), make_config(dir));
  auto fixer = std::make_shared<RecordingFixer>();
  e.set_spell_fixer(fixer);
  ASSERT_TRUE(e.add_global("quick world record", 3).ok());

  CompleteRequest req;
  req.query = "teh quick teh wrold ";  // all complete: the last 3 words are corrected
  req.correct = true;
  auto r = e.complete(req);
  ASSERT_TRUE(r.ok()) << r.message;
  ASSERT_EQ(fixer->calls().size(), 1u);
  EXPECT_EQ(fixer->calls()[0], "quick teh wrold");
  ASSERT_TRUE(r.corrected.has_value());
  EXPECT_EQ(*r.corrected, "teh quick the world");  // the first word stays as it was typed

  // with a word being typed it takes one of the three places: 2 complete words are corrected
  req.query = "teh quick wrold re";
  r = e.complete(req);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(fixer->calls().back(), "quick wrold");
  ASSERT_TRUE(r.corrected.has_value());
  EXPECT_EQ(*r.corrected, "teh quick world re");
  // and the lookups use the corrected words: "teh" is unknown, so only the tail finds anything
  EXPECT_EQ(texts(r.global), (Strings{"quick world record"}));
  EXPECT_EQ(matched(r.global), (Numbers{3}));

  // tail = 0: no fallback, and everything is corrected as before
  req.query = "teh quick wrold re";
  req.tail = 0;
  r = e.complete(req);
  EXPECT_EQ(fixer->calls().back(), "teh quick wrold");
  ASSERT_TRUE(r.corrected.has_value());
  EXPECT_EQ(*r.corrected, "the quick world re");

  // a tail of one word leaves no complete word to correct next to the one being typed
  const std::size_t calls = fixer->calls().size();
  req.tail = 1;
  req.min_tail = 1;
  r = e.complete(req);
  EXPECT_EQ(fixer->calls().size(), calls);
  EXPECT_FALSE(r.corrected.has_value());
}

// ================================================================== hot-prefix table (engine level)

namespace {

std::vector<std::string> small_words() {
  std::vector<std::string> out;
  for (char x : std::string("abc")) {
    out.push_back(std::string(1, x));
    for (char y : std::string("abc")) {
      out.push_back(std::string{x, y});
      for (char z : std::string("abc")) out.push_back(std::string{x, y, z});
    }
  }
  return out;
}

// What a client could observe of the single-letter and two-letter lookups.
std::vector<std::string> hot_dump(const Engine& e) {
  std::vector<std::string> out;
  for (char x : std::string("abc")) {
    std::vector<std::string> prefixes{std::string(1, x)};
    for (char y : std::string("abc")) prefixes.push_back(std::string{x, y});
    for (const auto& p : prefixes) {
      for (std::size_t n : {1u, 5u, 100u}) {
        const auto r = run(e, p, 0, 1, n);
        EXPECT_TRUE(r.ok());
        for (const auto& s : r.global)
          out.push_back(p + "|" + std::to_string(n) + "|" + std::to_string(s.id) + "|" + s.text + "|" +
                        std::to_string(s.freq));
        out.push_back(p + "|" + std::to_string(n) + "|end");
      }
    }
  }
  return out;
}

class HotEngineTest : public ::testing::TestWithParam<SnapshotMode> {};

INSTANTIATE_TEST_SUITE_P(Modes, HotEngineTest,
                         ::testing::Values(SnapshotMode::Fork, SnapshotMode::Inline),
                         [](const auto& info) {
                           return info.param == SnapshotMode::Fork ? "Fork" : "Inline";
                         });

}  // namespace

// The same operations on an engine with the table and on one without must be
// indistinguishable, across restart, snapshot and optimize (which swaps in a rebuilt index).
TEST_P(HotEngineTest, SameAnswersAsAnEngineThatScans) {
  TempDir dir;
  EngineConfig hot_cfg = make_config(dir, GetParam(), "hot");
  hot_cfg.hot_prefix_letters = 2;
  hot_cfg.hot_prefix_top = 100;
  EngineConfig scan_cfg = make_config(dir, GetParam(), "scan");
  scan_cfg.hot_prefix_letters = 0;

  auto hot = std::make_unique<Engine>(alphabet(), hot_cfg);
  auto scan = std::make_unique<Engine>(alphabet(), scan_cfg);

  const auto words = small_words();
  std::mt19937 rng(2024);
  std::vector<QueryId> live;
  const auto random_query = [&] {
    std::string q;
    for (std::size_t i = 0, len = 1 + rng() % 3; i < len; ++i) {
      if (i) q += ' ';
      q += words[rng() % words.size()];
    }
    return q;
  };
  const auto write_some = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      const unsigned r = rng() % 100;
      if (r < 65) {
        const std::string q = random_query();
        const std::uint32_t c = 1 + rng() % 5;
        const auto a = hot->add_global(q, c);
        const auto b = scan->add_global(q, c);
        ASSERT_TRUE(a.ok() && b.ok());
        ASSERT_EQ(a.id, b.id);
        live.push_back(a.id);
      } else if (r < 80) {
        const std::string q = random_query();
        const bool also = rng() % 2;
        const auto a = hot->add_user(1 + rng() % 3, q, 2, also);
        const auto b = scan->add_user(1, q, 2, also);  // the user does not matter to the global pool
        ASSERT_TRUE(a.ok() && b.ok());
      } else if (!live.empty()) {
        const QueryId id = live[rng() % live.size()];
        const auto a = hot->remove_global(id);
        const auto b = scan->remove_global(id);
        ASSERT_EQ(a.status, b.status);
      }
    }
  };
  const auto same = [&](const char* when) {
    EXPECT_EQ(hot_dump(*hot), hot_dump(*scan)) << when;
  };
  const auto restart = [&] {
    hot.reset();  // the data directory is locked until the old engine is gone
    scan.reset();
    hot = std::make_unique<Engine>(alphabet(), hot_cfg);
    scan = std::make_unique<Engine>(alphabet(), scan_cfg);
  };

  write_some(400);
  same("after the writes");
  EXPECT_GT(hot->stats().hot_lookups, 0u);
  EXPECT_EQ(scan->stats().hot_lookups, 0u);
  EXPECT_GT(hot->stats().index.hot_prefixes, 0u);
  EXPECT_EQ(scan->stats().index.hot_prefixes, 0u);

  ASSERT_TRUE(hot->snapshot().ok());
  ASSERT_TRUE(scan->snapshot().ok());
  restart();  // snapshot + WAL, table rebuilt
  same("after a restart");
  EXPECT_GT(hot->stats().index.hot_prefixes, 0u);

  write_some(200);
  ASSERT_TRUE(hot->optimize().ok());
  ASSERT_TRUE(scan->optimize().ok());
  EXPECT_GT(hot->stats().index.hot_prefixes, 0u);  // the swapped-in index has its table
  same("after optimize");

  write_some(300);  // writes after the swap keep the new table current
  same("after more writes");
  restart();
  same("after a second restart");
}

// Where the plain scan is approximate (too few dictionary words tried) the table is exact.
TEST(HotEngine, TheTableIsExactWhereTheScanIsApproximate) {
  TempDir dir;
  EngineConfig hot_cfg = make_config(dir, SnapshotMode::Inline, "hot");
  hot_cfg.hot_prefix_letters = 1;
  hot_cfg.scan.max_candidates = 2;
  EngineConfig scan_cfg = make_config(dir, SnapshotMode::Inline, "scan");
  scan_cfg.hot_prefix_letters = 0;
  scan_cfg.scan.max_candidates = 2;
  Engine hot(alphabet(), hot_cfg);
  Engine scan(alphabet(), scan_cfg);
  for (Engine* e : {&hot, &scan}) {
    ASSERT_TRUE(e->add_global("ab x", 5).ok());
    ASSERT_TRUE(e->add_global("ac y", 4).ok());
    ASSERT_TRUE(e->add_global("ad z", 3).ok());
  }
  const auto a = run(hot, "a", 0);
  EXPECT_EQ(texts(a.global), (Strings{"ab x", "ac y", "ad z"}));
  EXPECT_FALSE(a.truncated);
  const auto b = run(scan, "a", 0);
  EXPECT_EQ(b.global.size(), 2u);  // only two dictionary words are tried
  EXPECT_TRUE(b.truncated);
}

TEST(HotEngine, TheTailFallbackUsesItForALonePartialWord) {
  TempDir dir;
  EngineConfig cfg = make_config(dir);
  cfg.hot_prefix_letters = 2;
  Engine e(alphabet(), cfg);
  ASSERT_TRUE(e.add_global("juggernaut x", 4).ok());
  const auto before = e.stats().hot_lookups;
  // down to the word being typed alone, which is what the table is for
  const auto r = run(e, "zzz yyy ju", 3, 1);
  EXPECT_EQ(texts(r.global), (Strings{"juggernaut x"}));
  EXPECT_EQ(e.stats().hot_lookups, before + 1);
}

// Lookups served from the hot-prefix table run concurrently with writers that update it. Every
// answer must be a consistent, correctly ordered list, and at the end the table that the writes
// have maintained must equal one rebuilt from scratch by a restart.
TEST(HotEngine, ConcurrentWritersAndReadersThenARestartRebuildsTheSameTable) {
  TempDir dir;
  EngineConfig cfg = make_config(dir);
  cfg.hot_prefix_letters = 2;
  cfg.hot_prefix_top = 3;  // small, so that lists overflow and lose members all the time
  const auto words = small_words();
  std::vector<std::string> short_words;  // the prefixes the table serves
  for (const auto& w : words)
    if (w.size() <= 2) short_words.push_back(w);

  // Everything a client can see of the 1- and 2-letter lookups the table serves (n <= top).
  const auto dump = [&](const Engine& e) {
    std::vector<std::string> out;
    for (char x : std::string("abc")) {
      std::vector<std::string> prefixes{std::string(1, x)};
      for (char y : std::string("abc")) prefixes.push_back(std::string{x, y});
      for (const auto& p : prefixes)
        for (std::size_t n : {1u, 2u, 3u})
          for (const auto& s : run(e, p, 0, 1, n).global)
            out.push_back(p + "|" + std::to_string(n) + "|" + std::to_string(s.id) + "|" +
                          std::to_string(s.freq));
    }
    return out;
  };

  std::vector<std::string> maintained;
  std::atomic<int> bad_lists{0};
  {
    Engine e(alphabet(), cfg);
    std::atomic<bool> writers_done{false};
    std::atomic<int> running_writers{2};
    std::atomic<int> readers_started{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 2; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937 rng(100 + t);
        std::vector<QueryId> mine;
        while (readers_started < 3) std::this_thread::yield();  // the writes overlap the lookups
        for (int i = 0; i < 3000; ++i) {
          if (i % 8 == 0) std::this_thread::yield();  // interleave even on a single core
          if (rng() % 10 < 7 || mine.empty()) {
            std::string q;
            for (std::size_t k = 0, len = 1 + rng() % 3; k < len; ++k) {
              if (k) q += ' ';
              q += words[rng() % words.size()];
            }
            const auto r = e.add_global(q, 1 + rng() % 5);
            if (r.ok()) mine.push_back(r.id);
          } else {
            e.remove_global(mine[rng() % mine.size()]);  // may be gone already: ignored
          }
        }
        if (--running_writers == 0) writers_done = true;
      });
    }
    for (unsigned t = 0; t < 3; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937 rng(200 + t);
        ++readers_started;
        for (int done = 0; !writers_done || done < 300; ++done) {
          std::this_thread::yield();
          const std::string p = short_words[rng() % short_words.size()];
          const auto r = run(e, p, 0, 1, 1 + rng() % 3);
          std::set<QueryId> seen;
          for (std::size_t i = 0; i < r.global.size(); ++i) {
            const auto& s = r.global[i];
            const bool ordered = i == 0 || r.global[i - 1].freq > s.freq ||
                                 (r.global[i - 1].freq == s.freq && r.global[i - 1].id < s.id);
            if (!ordered || !seen.insert(s.id).second || s.freq == 0 ||
                s.text.compare(0, p.size(), p) != 0)
              ++bad_lists;
          }
        }
      });
    }
    for (auto& th : threads) th.join();
    EXPECT_GT(e.stats().hot_lookups, 0u);
    maintained = dump(e);
  }
  EXPECT_EQ(bad_lists.load(), 0);
  ASSERT_FALSE(maintained.empty());

  Engine reopened(alphabet(), cfg);  // snapshot-less restart: WAL replay, table built from scratch
  EXPECT_EQ(dump(reopened), maintained);
}
