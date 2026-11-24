#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "test_util.hpp"

using namespace qac;
using namespace qac::test;

TEST(Index, AddAndRankByFrequency) {
  Index idx;
  add(idx, "new york", 5);
  add(idx, "new york weather", 9);
  add(idx, "new zealand", 2);
  add(idx, "newcastle", 1);

  const auto r = global_texts(idx, "new ");
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[0], "new york weather");
  EXPECT_EQ(r[1], "new york");
  EXPECT_EQ(r[2], "new zealand");
}

TEST(Index, ExactQueryIsPartOfItsOwnCompletions) {
  Index idx;
  add(idx, "new york", 3);
  const auto r = global_texts(idx, "new york ");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], "new york");
}

TEST(Index, PartialLastWord) {
  Index idx;
  add(idx, "new york", 5);
  add(idx, "new york weather", 9);
  add(idx, "new yorker magazine", 7);
  add(idx, "new zealand", 2);
  add(idx, "you tube", 100);

  auto r = global_texts(idx, "new yo");
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[0], "new york weather");
  EXPECT_EQ(r[1], "new yorker magazine");
  EXPECT_EQ(r[2], "new york");

  // a one-word partial: candidates are ranked by how much the word is used
  r = global_texts(idx, "yo");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], "you tube");

  r = global_texts(idx, "new q");
  EXPECT_TRUE(r.empty());
}

TEST(Index, UnknownCompleteWordGivesNothing) {
  Index idx;
  add(idx, "new york");
  EXPECT_TRUE(global_texts(idx, "old ").empty());
  EXPECT_TRUE(global_texts(idx, "").empty());
}

TEST(Index, FrequencyAccumulatesAndIdsAreStable) {
  Index idx;
  const auto a = add(idx, "hello world", 2);
  const auto b = add(idx, "hello world", 3);
  EXPECT_TRUE(a.created);
  EXPECT_FALSE(b.created);
  EXPECT_EQ(a.id, b.id);
  EXPECT_EQ(b.freq, 5u);
  EXPECT_NE(a.id, kNoQuery);
}

TEST(Index, FrequencySaturates) {
  Index idx;
  add(idx, "big", 0xFFFFFFF0u);
  const auto r = add(idx, "big", 100);
  EXPECT_EQ(r.freq, 0xFFFFFFFFu);
}

TEST(Index, RemoveGlobal) {
  Index idx;
  const auto a = add(idx, "alpha beta", 4);
  add(idx, "alpha gamma", 3);
  EXPECT_TRUE(idx.remove_global(a.id));
  EXPECT_FALSE(idx.remove_global(a.id));  // already gone
  EXPECT_FALSE(idx.query_alive(a.id));

  const auto r = global_texts(idx, "alpha ");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], "alpha gamma");

  EXPECT_FALSE(idx.remove_global(999));
  EXPECT_FALSE(idx.remove_global(kNoQuery));
}

TEST(Index, ReAddAfterRemoveGetsNewId) {
  Index idx;
  const auto a = add(idx, "foo bar");
  ASSERT_TRUE(idx.remove_global(a.id));
  const auto b = add(idx, "foo bar");
  EXPECT_TRUE(b.created);
  EXPECT_NE(a.id, b.id);
  EXPECT_EQ(b.freq, 1u);
}

TEST(Index, UserHistoryBasics) {
  Index idx;
  add_user(idx, 7, "my secret query", 1, 100);
  add_user(idx, 7, "my other query", 3, 200);
  add_user(idx, 8, "my private thing", 9, 300);

  auto r = user_texts(idx, 7, "my ");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "my other query");  // higher frequency first
  EXPECT_EQ(r[1], "my secret query");

  r = user_texts(idx, 7, "my o");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], "my other query");

  // another user's data is never visible
  EXPECT_EQ(user_texts(idx, 8, "my ").size(), 1u);
  EXPECT_TRUE(user_texts(idx, 9, "my ").empty());

  // user-only queries are not part of the global pool
  EXPECT_TRUE(global_texts(idx, "my ").empty());
}

TEST(Index, UserTieBreakByRecency) {
  Index idx;
  add_user(idx, 1, "abc one", 2, 10);
  add_user(idx, 1, "abc two", 2, 50);
  add_user(idx, 1, "abc three", 2, 30);
  const auto r = user_texts(idx, 1, "abc ");
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[0], "abc two");
  EXPECT_EQ(r[1], "abc three");
  EXPECT_EQ(r[2], "abc one");
}

TEST(Index, UserEmptyInputReturnsHistory) {
  Index idx;
  add_user(idx, 1, "first query", 1, 1);
  add_user(idx, 1, "second query", 5, 2);
  const auto r = user_texts(idx, 1, "");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "second query");
}

TEST(Index, UserAddAlsoGlobal) {
  Index idx;
  const auto r = add_user(idx, 5, "shared words", 2, 10, true);
  EXPECT_EQ(r.freq, 2u);
  const auto g = global_texts(idx, "shared ");
  ASSERT_EQ(g.size(), 1u);
}

TEST(Index, UserAddKeepsIdOfExistingGlobalQuery) {
  Index idx;
  const auto g = add(idx, "common query", 10);
  const auto u = add_user(idx, 3, "common query", 1, 5);
  EXPECT_EQ(g.id, u.id);
  EXPECT_FALSE(u.created);
}

TEST(Index, RemoveUserKeepsGlobalAndOtherUsers) {
  Index idx;
  const auto g = add(idx, "x y", 4);
  add_user(idx, 1, "x y", 1, 1);
  add_user(idx, 2, "x y", 1, 1);

  EXPECT_TRUE(idx.remove_user(1, g.id));
  EXPECT_FALSE(idx.remove_user(1, g.id));  // not in the history any more
  EXPECT_TRUE(user_texts(idx, 1, "x ").empty());
  EXPECT_EQ(user_texts(idx, 2, "x ").size(), 1u);
  EXPECT_EQ(global_texts(idx, "x ").size(), 1u);
  EXPECT_TRUE(idx.query_alive(g.id));
}

TEST(Index, QueryDiesWhenNeitherGlobalNorUserReferenceIt) {
  Index idx;
  const auto g = add(idx, "left over", 1);
  add_user(idx, 1, "left over", 1, 1);

  ASSERT_TRUE(idx.remove_global(g.id));
  EXPECT_TRUE(idx.query_alive(g.id));  // still referenced by the user
  EXPECT_TRUE(global_texts(idx, "left ").empty());
  EXPECT_EQ(user_texts(idx, 1, "left ").size(), 1u);

  ASSERT_TRUE(idx.remove_user(1, g.id));
  EXPECT_FALSE(idx.query_alive(g.id));
  EXPECT_EQ(idx.stats().dead_queries, 1u);
}

TEST(Index, RemoveUserOnlyQueryKillsIt) {
  Index idx;
  const auto u = add_user(idx, 1, "only mine", 1, 1);
  EXPECT_TRUE(idx.remove_user(1, u.id));
  EXPECT_FALSE(idx.query_alive(u.id));
  EXPECT_FALSE(idx.remove_user(1, u.id));
  EXPECT_FALSE(idx.remove_global(u.id));
}

TEST(Index, RemoveUserOfUnknownUserOrQuery) {
  Index idx;
  const auto g = add(idx, "a b");
  EXPECT_FALSE(idx.remove_user(42, g.id));  // unknown user
  add_user(idx, 42, "c d", 1, 1);
  EXPECT_FALSE(idx.remove_user(42, g.id));  // not in this user's history
  EXPECT_FALSE(idx.remove_user(42, 12345));
}

TEST(Index, SearchBudgetTruncates) {
  Index idx;
  for (int i = 0; i < 500; ++i) add(idx, "common w" + std::to_string(i), 1 + i % 7);

  Index::ScanLimits lim;
  lim.max_visited = 100;
  Index::ScanStats st;
  const auto common = *idx.find_word("common");
  const auto hits = idx.top_global({common}, "", 5, lim, st);
  EXPECT_EQ(hits.size(), 5u);
  EXPECT_TRUE(st.truncated);

  Index::ScanStats st2;
  idx.top_global({common}, "", 5, Index::ScanLimits{}, st2);
  EXPECT_FALSE(st2.truncated);
}

TEST(Index, TooManyCandidatesMarksTruncated) {
  Index idx;
  for (int i = 0; i < 20; ++i) add(idx, "pre" + std::to_string(i) + "x", 1 + i);
  Index::ScanLimits lim;
  lim.max_candidates = 5;
  Index::ScanStats st;
  const auto hits = idx.top_global({}, "pre", 3, lim, st);
  EXPECT_EQ(hits.size(), 3u);
  EXPECT_TRUE(st.truncated);
  EXPECT_EQ(text_of(idx, hits[0].id), "pre19x");  // highest weight candidate first
}

TEST(Index, ManyUsersAndQueries) {
  Index idx;
  for (std::uint32_t u = 1000; u > 0; --u) add_user(idx, u * 7919u, "history " + std::to_string(u), u, u);
  EXPECT_EQ(idx.stats().users, 1000u);
  const auto r = user_texts(idx, 500u * 7919u, "history ");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], "history 500");
}

namespace {
// Records every observable property of the index relevant for equivalence checks.
std::vector<std::string> probe(const Index& idx) {
  std::vector<std::string> out;
  for (const char* typed : {"new ", "new y", "n", "alpha ", "al", "zz", "ma", "my "}) {
    for (const auto& t : global_texts(idx, typed, 20)) out.push_back(std::string(typed) + "=>" + t);
  }
  for (std::uint32_t u : {1u, 2u, 3u})
    for (const char* typed : {"", "my ", "my s", "q"})
      for (const auto& t : user_texts(idx, u, typed, 20))
        out.push_back("u" + std::to_string(u) + ":" + typed + "=>" + t);
  return out;
}

Index make_sample() {
  Index idx;
  add(idx, "new york", 5);
  add(idx, "new york weather", 9);
  add(idx, "new zealand", 2);
  add(idx, "alpha beta", 4);
  add(idx, "alpha gamma", 6);
  add(idx, "mango", 1);
  const auto doomed = add(idx, "to be removed", 3);
  add_user(idx, 1, "my secret query", 2, 10);
  add_user(idx, 1, "mango", 4, 20, true);
  add_user(idx, 2, "my other query", 1, 30);
  add_user(idx, 3, "q", 1, 1);
  idx.remove_global(doomed.id);
  const auto gone = add_user(idx, 2, "short lived", 1, 1);
  idx.remove_user(2, gone.id);
  return idx;
}
}  // namespace

TEST(Index, SaveLoadRoundTrip) {
  const Index a = make_sample();
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  a.save(ss);
  Index b = Index::load(ss);

  EXPECT_EQ(probe(a), probe(b));
  const auto sa = a.stats();
  const auto sb = b.stats();
  EXPECT_EQ(sa.words, sb.words);
  EXPECT_EQ(sa.live_queries, sb.live_queries);
  EXPECT_EQ(sa.user_entries, sb.user_entries);
  EXPECT_EQ(sa.dead_queries, sb.dead_queries);

  // the loaded index keeps working, and keeps allocating the same ids
  const auto x = add(const_cast<Index&>(a), "brand new query");
  const auto y = add(b, "brand new query");
  EXPECT_EQ(x.id, y.id);
}

TEST(Index, LoadRejectsTruncatedData) {
  const Index a = make_sample();
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  a.save(ss);
  const std::string full = ss.str();
  for (std::size_t cut : {std::size_t{3}, full.size() / 3, full.size() - 5}) {
    std::stringstream part(full.substr(0, cut), std::ios::in | std::ios::binary);
    EXPECT_ANY_THROW(Index::load(part)) << "cut at " << cut;
  }
}

TEST(Index, CompactionPreservesBehaviourAndIds) {
  Index a = make_sample();
  const auto before = probe(a);
  const auto before_stats = a.stats();
  ASSERT_GT(before_stats.dead_queries, 0u);

  Index c = a.compacted();
  EXPECT_EQ(probe(c), before);
  EXPECT_EQ(c.stats().dead_queries, 0u);
  EXPECT_EQ(c.stats().live_queries, before_stats.live_queries);
  EXPECT_LE(c.stats().words, before_stats.words);
  EXPECT_LT(c.stats().arena_bytes, before_stats.arena_bytes);

  // ids of surviving queries are identical, dead ones stay dead
  const auto mango = add(a, "mango", 0 + 1);
  const auto mango_c = add(c, "mango", 1);
  EXPECT_EQ(mango.id, mango_c.id);
  EXPECT_FALSE(c.query_alive(7));  // "to be removed"

  // and both allocate the same next id
  EXPECT_EQ(add(a, "fresh one").id, add(c, "fresh one").id);
}

TEST(Index, CompactionDropsUnusedWords) {
  Index a;
  const auto q = add(a, "unique words only here", 1);
  add(a, "kept", 1);
  a.remove_global(q.id);
  EXPECT_EQ(a.stats().words, 5u);
  const Index c = a.compacted();
  EXPECT_EQ(c.stats().words, 1u);
  EXPECT_FALSE(c.find_word("unique").has_value());
  EXPECT_TRUE(c.find_word("kept").has_value());
}

TEST(Index, CompactedIndexSurvivesSaveLoad) {
  const Index a = make_sample();
  const Index c = a.compacted();
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  c.save(ss);
  const Index d = Index::load(ss);
  EXPECT_EQ(probe(c), probe(d));
}

// ================================================ ranking by mean word frequency

namespace {

using Rank = Index::GlobalRank;

struct Ranked {
  std::string text;
  double score;
  bool operator==(const Ranked& o) const { return text == o.text && score == o.score; }
};
std::ostream& operator<<(std::ostream& os, const Ranked& r) { return os << r.text << "@" << r.score; }

std::vector<Ranked> ranked(const Index& idx, const std::string& typed, Rank rank, std::size_t n = 10,
                           Index::ScanStats* stats_out = nullptr,
                           const Index::ScanLimits& lim = Index::ScanLimits{}) {
  auto words = split(typed);
  std::string partial;
  if (!typed.empty() && typed.back() != ' ' && !words.empty()) {
    partial = words.back();
    words.pop_back();
  }
  std::vector<WordId> ids;
  std::vector<Ranked> out;
  if (!resolve(idx, words, ids)) return out;
  Index::ScanStats st;
  for (const auto& h : idx.top_global(ids, partial, n, lim, st, rank))
    out.push_back({text_of(idx, h.id), h.average()});
  if (stats_out) *stats_out = st;
  return out;
}

std::vector<std::string> names(const std::vector<Ranked>& v) {
  std::vector<std::string> out;
  for (const auto& r : v) out.push_back(r.text);
  return out;
}

// Word frequencies: apple 112 (100+10+1+1), pie 10, tart 1, juice 1001 (1+1000).
Index apple_index() {
  Index idx;
  add(idx, "apple", 100);
  add(idx, "apple pie", 10);
  add(idx, "apple tart", 1);
  add(idx, "apple juice", 1);
  add(idx, "juice", 1000);
  return idx;
}

}  // namespace

TEST(IndexRank, MeanWordFrequencyReordersTheCandidates) {
  const Index idx = apple_index();
  // By query frequency the rare "apple juice" is last (ties by id) ...
  EXPECT_EQ(names(ranked(idx, "apple ", Rank::Frequency)),
            (std::vector<std::string>{"apple", "apple pie", "apple tart", "apple juice"}));
  // ... but its words are very frequent: (112 + 1001) / 2 = 556.5.
  const auto avg = ranked(idx, "apple ", Rank::AverageWordFrequency);
  EXPECT_EQ(avg, (std::vector<Ranked>{{"apple juice", 556.5},
                                      {"apple", 112.0},
                                      {"apple pie", 61.0},
                                      {"apple tart", 56.5}}));
}

TEST(IndexRank, TopNKeepsTheBestByTheChosenRank) {
  const Index idx = apple_index();
  EXPECT_EQ(names(ranked(idx, "apple ", Rank::AverageWordFrequency, 2)),
            (std::vector<std::string>{"apple juice", "apple"}));
  EXPECT_EQ(names(ranked(idx, "apple ", Rank::Frequency, 2)),
            (std::vector<std::string>{"apple", "apple pie"}));
  EXPECT_TRUE(ranked(idx, "apple ", Rank::AverageWordFrequency, 0).empty());
}

TEST(IndexRank, FrequencyRankingDoesNotCarryScores) {
  const Index idx = apple_index();
  for (const auto& r : ranked(idx, "apple ", Rank::Frequency)) EXPECT_EQ(r.score, 0.0);
}

TEST(IndexRank, WorksForAPartialLastWord) {
  Index idx = apple_index();
  add(idx, "pear", 5);
  add(idx, "peach juice", 2);  // peach 2, juice now 1003
  // "ap" -> the dictionary word "apple"; "apple juice" now (112 + 1003) / 2.
  EXPECT_EQ(ranked(idx, "ap", Rank::AverageWordFrequency).front(), (Ranked{"apple juice", 557.5}));
  // Several candidate words: "pe" -> pear (5), peach (2); whole queries are ranked together.
  EXPECT_EQ(ranked(idx, "pe", Rank::AverageWordFrequency),
            (std::vector<Ranked>{{"peach juice", 502.5}, {"pear", 5.0}}));
  // a complete word before the partial one
  EXPECT_EQ(names(ranked(idx, "apple j", Rank::AverageWordFrequency)), (std::vector<std::string>{"apple juice"}));
}

TEST(IndexRank, TiesFallBackToQueryFrequencyThenId) {
  Index idx;
  add(idx, "go a b", 5);
  add(idx, "go b a", 3);
  add(idx, "go c d", 4);
  add(idx, "go d c", 4);
  // go=16, a=b=c=d=8: all four means are (16+8+8)/3.
  const auto r = ranked(idx, "go ", Rank::AverageWordFrequency);
  ASSERT_EQ(r.size(), 4u);
  for (const auto& x : r) EXPECT_DOUBLE_EQ(x.score, 32.0 / 3.0);
  EXPECT_EQ(names(r), (std::vector<std::string>{"go a b", "go c d", "go d c", "go b a"}));
}

TEST(IndexRank, FollowsUpdatesAndRemovals) {
  Index idx = apple_index();
  EXPECT_EQ(ranked(idx, "apple ", Rank::AverageWordFrequency).front().text, "apple juice");

  // The frequency of a word changes with every query that contains it.
  add(idx, "apple pie", 2000);  // pie 2010, apple 2112: apple 2112, apple pie 2061, apple juice 1556.5
  EXPECT_EQ(names(ranked(idx, "apple ", Rank::AverageWordFrequency)),
            (std::vector<std::string>{"apple", "apple pie", "apple juice", "apple tart"}));

  // Removing the query that made "juice" frequent lowers the score of "apple juice"
  // to (2112 + 1) / 2, which ties "apple tart"; the older query wins the tie.
  const auto juice = idx.find_word("juice");
  ASSERT_TRUE(juice);
  Index::ScanStats st;
  const auto juice_hits = idx.top_global({*juice}, "", 5, Index::ScanLimits{}, st);
  ASSERT_EQ(juice_hits.size(), 1u);  // only the query "juice" itself starts with it
  ASSERT_TRUE(idx.remove_global(juice_hits[0].id));
  const auto r = ranked(idx, "apple ", Rank::AverageWordFrequency);
  ASSERT_EQ(r.size(), 4u);
  EXPECT_EQ(r[3].text, "apple juice");
  EXPECT_EQ(r[2].text, "apple tart");
  EXPECT_DOUBLE_EQ(r[2].score, r[3].score);
}

TEST(IndexRank, UserOnlyQueriesDoNotCountAsCandidatesOrWords) {
  Index idx;
  add(idx, "red car", 10);
  add_user(idx, 7, "red car chase", 500, 1, false);  // user history only
  const auto r = ranked(idx, "red ", Rank::AverageWordFrequency);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], (Ranked{"red car", 10.0}));  // the user's query adds nothing to the word weights
}

TEST(IndexRank, SurvivesCompaction) {
  Index idx = apple_index();
  add(idx, "scratch me", 3);
  Index::ScanStats st;
  const auto id = idx.top_global({*idx.find_word("scratch")}, "", 1, Index::ScanLimits{}, st);
  ASSERT_EQ(id.size(), 1u);
  ASSERT_TRUE(idx.remove_global(id[0].id));
  const Index packed = idx.compacted();
  for (const char* typed : {"apple ", "ap", "apple j", "ju"})
    EXPECT_EQ(ranked(packed, typed, Rank::AverageWordFrequency), ranked(idx, typed, Rank::AverageWordFrequency)) << typed;
}

TEST(IndexRank, SurvivesSaveAndLoad) {
  const Index idx = apple_index();
  std::stringstream ss;
  idx.save(ss);
  const Index loaded = Index::load(ss);
  EXPECT_EQ(ranked(loaded, "apple ", Rank::AverageWordFrequency), ranked(idx, "apple ", Rank::AverageWordFrequency));
}

TEST(IndexRank, ScansTheSameEntriesAsTheFrequencyRanking) {
  Index idx;
  for (int i = 0; i < 300; ++i) add(idx, "common w" + std::to_string(i), 1 + i % 7);
  Index::ScanStats a, b;
  ranked(idx, "common ", Rank::Frequency, 5, &a);
  ranked(idx, "common ", Rank::AverageWordFrequency, 5, &b);
  EXPECT_EQ(a.visited, b.visited);
  EXPECT_FALSE(b.truncated);

  Index::ScanLimits tight;
  tight.max_visited = 50;
  ranked(idx, "common ", Rank::Frequency, 5, &a, tight);
  ranked(idx, "common ", Rank::AverageWordFrequency, 5, &b, tight);
  EXPECT_TRUE(a.truncated);
  EXPECT_TRUE(b.truncated);  // the budget applies to both
  EXPECT_EQ(a.visited, b.visited);
}

TEST(IndexRank, LargeFrequenciesDoNotOverflow) {
  Index idx;
  const std::uint32_t big = 4294967295u;
  add(idx, "top a b c d e f g h", big);
  add(idx, "top a b c d e f g h", big);  // saturates the query, not the arithmetic
  add(idx, "top z", 1);
  const auto r = ranked(idx, "top ", Rank::AverageWordFrequency);
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0].text, "top a b c d e f g h");
  EXPECT_GT(r[0].score, 4.0e9);
}

// ====================================================== brute-force oracle (property tests)
//
// The result of a search must equal "take every candidate, sort, keep the first N". This
// matters most when there are more candidates than N, which is when the selection has
// to evict the right element.

namespace {

__extension__ typedef unsigned __int128 OracleU128;

struct ModelQuery {
  std::vector<std::string> words;
  std::uint32_t count;
  QueryId id;
};

struct Model {
  std::vector<ModelQuery> queries;                  // global pool, id = insertion order + 1
  std::vector<std::string> word_order;              // WordId = first appearance
  std::map<std::string, std::uint64_t> weight;      // word -> total frequency in the global pool

  void add(const std::vector<std::string>& words, std::uint32_t count) {
    for (const auto& w : words) {
      if (std::find(word_order.begin(), word_order.end(), w) == word_order.end()) word_order.push_back(w);
      weight[w] += count;
    }
    queries.push_back({words, count, static_cast<QueryId>(queries.size() + 1)});
  }
  std::size_t word_id(const std::string& w) const {
    return static_cast<std::size_t>(std::find(word_order.begin(), word_order.end(), w) - word_order.begin());
  }
  static bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

  // Candidates for "complete words + partial last word", as the engine defines them: with a
  // partial word, only its `cap` most frequent dictionary words are considered.
  std::vector<const ModelQuery*> candidates(const std::vector<std::string>& prefix, const std::string& partial,
                                            std::size_t cap) const {
    std::vector<const ModelQuery*> out;
    std::vector<std::string> chosen;
    if (!partial.empty()) {
      std::vector<std::string> dict;
      for (const auto& w : word_order)
        if (starts_with(w, partial) && weight.at(w) > 0) dict.push_back(w);
      std::sort(dict.begin(), dict.end(), [&](const std::string& a, const std::string& b) {
        return weight.at(a) != weight.at(b) ? weight.at(a) > weight.at(b) : word_id(a) < word_id(b);
      });
      if (dict.size() > cap) dict.resize(cap);
      chosen = dict;
    }
    for (const auto& q : queries) {
      if (q.words.size() < prefix.size() + (partial.empty() ? 0 : 1)) continue;
      if (!std::equal(prefix.begin(), prefix.end(), q.words.begin())) continue;
      if (!partial.empty() &&
          std::find(chosen.begin(), chosen.end(), q.words[prefix.size()]) == chosen.end())
        continue;
      out.push_back(&q);
    }
    return out;
  }

  std::uint64_t word_sum(const ModelQuery& q) const {
    std::uint64_t s = 0;
    for (const auto& w : q.words) s += weight.at(w);
    return s;
  }
};

std::vector<QueryId> oracle_global(const Model& m, const std::vector<std::string>& prefix,
                                   const std::string& partial, std::size_t n, Rank rank, std::size_t cap) {
  auto c = m.candidates(prefix, partial, cap);
  std::sort(c.begin(), c.end(), [&](const ModelQuery* a, const ModelQuery* b) {
    if (rank == Rank::AverageWordFrequency) {
      const OracleU128 l = static_cast<OracleU128>(m.word_sum(*a)) * b->words.size();
      const OracleU128 r = static_cast<OracleU128>(m.word_sum(*b)) * a->words.size();
      if (l != r) return l > r;
    }
    if (a->count != b->count) return a->count > b->count;
    return a->id < b->id;
  });
  std::vector<QueryId> ids;
  for (std::size_t i = 0; i < c.size() && i < n; ++i) ids.push_back(c[i]->id);
  return ids;
}

}  // namespace

TEST(IndexOracle, GlobalTopNEqualsFullSort) {
  for (unsigned seed = 1; seed <= 6; ++seed) {
    std::mt19937 rng(seed);
    Index idx;
    Model model;
    // 150 words that all start with "wa" (more than the 64 dictionary candidates), counts small so
    // that ties are frequent.
    std::vector<std::string> vocab;
    for (int i = 0; i < 150; ++i) vocab.push_back("wa" + std::to_string(i));
    vocab.push_back("zz");
    std::set<std::string> seen;
    for (int i = 0; i < 700; ++i) {
      std::vector<std::string> words;
      const int len = 1 + static_cast<int>(rng() % 3);
      for (int k = 0; k < len; ++k)
        words.push_back(vocab[(rng() % 3 == 0) ? rng() % 12 : rng() % vocab.size()]);  // skewed: a few hot words
      std::string key;
      for (const auto& w : words) key += w + " ";
      if (!seen.insert(key).second) continue;  // distinct texts only: model id == index id
      const std::uint32_t count = 1 + static_cast<std::uint32_t>(rng() % 20);
      std::vector<std::string_view> v(words.begin(), words.end());
      idx.add_global(v, count);
      model.add(words, count);
    }

    const Index::ScanLimits lim;  // defaults: budgets far above this data size
    struct Typed { std::vector<std::string> prefix; std::string partial; };
    const std::vector<Typed> cases = {
        {{}, "wa"}, {{}, "wa1"}, {{}, "wa12"}, {{"wa3"}, ""}, {{"wa3"}, "wa"}, {{"wa0"}, "wa1"},
        {{"wa5", "wa2"}, ""}, {{"wa7"}, "z"}, {{}, "w"}};
    for (const auto& t : cases) {
      std::vector<WordId> ids;
      bool known = true;
      for (const auto& w : t.prefix) {
        const auto id = idx.find_word(w);
        if (!id) { known = false; break; }
        ids.push_back(*id);
      }
      if (!known) continue;
      for (Rank rank : {Rank::Frequency, Rank::AverageWordFrequency})
        for (std::size_t n : {1u, 2u, 3u, 10u, 37u, 5000u}) {
          Index::ScanStats st;
          std::vector<QueryId> got;
          for (const auto& h : idx.top_global(ids, t.partial, n, lim, st, rank)) got.push_back(h.id);
          EXPECT_EQ(got, oracle_global(model, t.prefix, t.partial, n, rank, lim.max_candidates))
              << "seed " << seed << " partial '" << t.partial << "' prefix " << t.prefix.size() << " words, n=" << n
              << (rank == Rank::Frequency ? " frequency" : " avg_word_freq");
        }
    }
  }
}

TEST(IndexOracle, UserTopMEqualsFullSort) {
  for (unsigned seed = 1; seed <= 4; ++seed) {
    std::mt19937 rng(100 + seed);
    Index idx;
    struct U { std::vector<std::string> words; std::uint32_t count, ts; QueryId id; };
    std::vector<U> mine;
    std::set<std::string> seen;
    for (int i = 0; i < 400; ++i) {
      std::vector<std::string> words{"my"};
      const int len = 1 + static_cast<int>(rng() % 2);
      for (int k = 0; k < len; ++k) words.push_back("u" + std::to_string(rng() % 60));
      std::string key;
      for (const auto& w : words) key += w + " ";
      if (!seen.insert(key).second) continue;
      const std::uint32_t count = 1 + static_cast<std::uint32_t>(rng() % 6);  // many ties
      const std::uint32_t ts = 1 + static_cast<std::uint32_t>(rng() % 5);     // ties on recency too
      std::vector<std::string_view> v(words.begin(), words.end());
      const auto out = idx.add_user(9, v, count, ts, false);
      mine.push_back({words, count, ts, out.id});
      // another user's data must never leak in
      idx.add_user(10, v, 1000, 99, false);
    }
    const Index::ScanLimits lim;
    for (const std::string& partial : {std::string(), std::string("u"), std::string("u1"), std::string("u5")}) {
      const std::vector<std::string> prefix = {"my"};
      std::vector<const U*> c;
      for (const auto& q : mine) {
        if (q.words.size() < 1 + (partial.empty() ? 0 : 1)) continue;
        if (!partial.empty() && q.words[1].compare(0, partial.size(), partial) != 0) continue;
        c.push_back(&q);
      }
      std::sort(c.begin(), c.end(), [](const U* a, const U* b) {
        if (a->count != b->count) return a->count > b->count;
        if (a->ts != b->ts) return a->ts > b->ts;
        return a->id < b->id;
      });
      for (std::size_t m : {1u, 2u, 5u, 20u, 5000u}) {
        Index::ScanStats st;
        std::vector<QueryId> got, want;
        for (const auto& h : idx.top_user(9, {*idx.find_word("my")}, partial, m, lim, st)) got.push_back(h.id);
        for (std::size_t i = 0; i < c.size() && i < m; ++i) want.push_back(c[i]->id);
        EXPECT_EQ(got, want) << "seed " << seed << " partial '" << partial << "' m=" << m;
      }
    }
  }
}
