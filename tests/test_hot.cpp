// Tests of the hot-prefix table (Index::enable_hot_prefixes).
//
// The table is derived data kept up to date inside the index mutators, so the tests compare it
// with a brute-force model of "the live global queries whose first word starts with P, best
// first" after long random sequences of adds, raises and removals.
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <sstream>

#include "qac/index.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;

namespace {

// What the global pool contains, tracked by the test itself.
struct Model {
  struct Entry {
    std::string text;
    std::uint32_t freq = 0;
  };
  std::map<QueryId, Entry> queries;

  std::vector<std::string> top(const std::string& partial, std::size_t n) const {
    std::vector<std::pair<std::uint32_t, QueryId>> hits;
    for (const auto& [id, e] : queries) {
      if (e.freq == 0) continue;
      const std::string first = e.text.substr(0, e.text.find(' '));
      if (first.compare(0, partial.size(), partial) != 0) continue;
      hits.emplace_back(e.freq, id);
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<std::string> out;
    for (std::size_t i = 0; i < std::min(n, hits.size()); ++i) out.push_back(queries.at(hits[i].second).text);
    return out;
  }

  std::vector<QueryId> live() const {
    std::vector<QueryId> out;
    for (const auto& [id, e] : queries)
      if (e.freq != 0) out.push_back(id);
    return out;
  }
};

// Every string over {a, b, c} of 1..3 letters: first words are drawn from the same set, so
// prefixes collide a lot and lists overflow.
const std::vector<std::string>& all_words() {
  static const std::vector<std::string> w = [] {
    std::vector<std::string> out;
    const std::string letters = "abc";
    for (char x : letters) {
      out.push_back(std::string(1, x));
      for (char y : letters) {
        out.push_back(std::string{x, y});
        for (char z : letters) out.push_back(std::string{x, y, z});
      }
    }
    return out;
  }();
  return w;
}

class Driver {
 public:
  explicit Driver(unsigned seed) : rng_(seed) {}

  Index idx;
  Model model;

  void step() {
    const unsigned r = rng_() % 100;
    if (r < 55) {
      add_global();
    } else if (r < 65) {
      add_user(true);
    } else if (r < 72) {
      add_user(false);
    } else if (r < 85) {
      remove_random();
    } else {
      remove_best_of_a_prefix();
    }
  }

  // Compares every lookup of the hot shapes (and one longer) with the model.
  void check(const std::string& when, unsigned* hot_hits = nullptr, unsigned* fallbacks = nullptr) {
    const auto& cfg = idx.hot_prefix_config();
    for (const auto& w : all_words()) {
      if (w.size() > 3) continue;
      for (std::size_t n : {1u, 3u, 5u, 10u}) {
        Index::ScanStats st;
        const auto got = global_texts(idx, w, n, &st);
        const auto want = model.top(w, n);
        ASSERT_EQ(got, want) << when << ": partial '" << w << "' n=" << n << " hot=" << st.hot;
        const bool shape = cfg.letters != 0 && w.size() <= cfg.letters && n <= cfg.top;
        if (st.hot) {
          ASSERT_TRUE(shape) << when << ": answered from the table outside its shape: " << w;
          ASSERT_FALSE(st.truncated);
          ASSERT_EQ(st.scanned, 0u);
          if (hot_hits) ++*hot_hits;
        } else if (shape && fallbacks) {
          ++*fallbacks;
        }
      }
    }
  }

 private:
  std::string random_query() {
    const auto& words = all_words();
    const std::size_t len = 1 + rng_() % 3;
    std::string text;
    for (std::size_t i = 0; i < len; ++i) {
      if (i) text += ' ';
      text += words[rng_() % words.size()];
    }
    return text;
  }

  void add_global() {
    const std::string text = random_query();
    const std::uint32_t count = 1 + rng_() % 6;
    const auto out = add(idx, text, count);
    auto& e = model.queries[out.id];
    e.text = text;
    e.freq += count;
    ASSERT_EQ(e.freq, out.freq);
  }

  void add_user(bool also_global) {
    const std::string text = random_query();
    const std::uint32_t count = 1 + rng_() % 4;
    const auto out = qac::test::add_user(idx, rng_() % 3, text, count, 1, also_global);
    auto& e = model.queries[out.id];
    e.text = text;
    if (also_global) e.freq += count;
  }

  void remove_random() {
    const auto live = model.live();
    if (live.empty()) return;
    const QueryId id = live[rng_() % live.size()];
    ASSERT_TRUE(idx.remove_global(id));
    model.queries[id].freq = 0;
  }

  // The query most likely to be in a hot list: the best one under some prefix.
  void remove_best_of_a_prefix() {
    const auto& words = all_words();
    const std::string& p = words[rng_() % words.size()];
    const auto best = model.top(p, 1);
    if (best.empty()) return;
    for (auto& [id, e] : model.queries) {
      if (e.freq != 0 && e.text == best[0]) {
        ASSERT_TRUE(idx.remove_global(id));
        e.freq = 0;
        return;
      }
    }
  }

  std::mt19937 rng_;
};

}  // namespace

// The table is built from existing data and then kept current through every kind of write.
TEST(HotPrefixes, RandomWritesKeepTheTableExact) {
  struct Case {
    unsigned letters;
    std::size_t top;
    unsigned seed;
  };
  for (const Case c : {Case{1, 3, 1}, Case{2, 3, 2}, Case{2, 1, 3}, Case{3, 5, 4}, Case{2, 100, 5}}) {
    Driver d(c.seed);
    unsigned hot_hits = 0, fallbacks = 0;
    for (int i = 0; i < 150; ++i) d.step();  // data first, then switch the table on
    d.idx.enable_hot_prefixes({c.letters, c.top});
    d.check("after enabling", &hot_hits, &fallbacks);
    for (int round = 0; round < 12; ++round) {
      for (int i = 0; i < 40; ++i) d.step();
      d.check("round " + std::to_string(round) + " letters=" + std::to_string(c.letters) +
                  " top=" + std::to_string(c.top),
              &hot_hits, &fallbacks);
      if (::testing::Test::HasFatalFailure()) return;
    }
    EXPECT_GT(hot_hits, 0u) << "letters=" << c.letters << " top=" << c.top;
    if (c.top <= 3) {
      EXPECT_GT(fallbacks, 0u) << "lists that lost members must have been scanned";
    }
  }
}

TEST(HotPrefixes, RebuildingGivesTheSameAnswersAsMaintaining) {
  Driver d(77);
  d.idx.enable_hot_prefixes({2, 4});
  for (int i = 0; i < 500; ++i) d.step();
  d.check("maintained");
  d.idx.enable_hot_prefixes({2, 4});  // rebuilt from scratch
  d.check("rebuilt");
}

TEST(HotPrefixes, OnlyTheCoveredShapesUseTheTable) {
  Index idx;
  for (const char* q : {"apple", "apple pie", "apple juice", "apricot", "banana", "ab"}) add(idx, q, 3);
  idx.enable_hot_prefixes({2, 10});

  Index::ScanStats st;
  EXPECT_EQ(global_texts(idx, "a", 10, &st).size(), 5u);
  EXPECT_TRUE(st.hot);
  EXPECT_EQ(global_texts(idx, "ap", 10, &st).size(), 4u);
  EXPECT_TRUE(st.hot);
  EXPECT_EQ(global_texts(idx, "zz", 10, &st).size(), 0u);  // no query starts with it: exact, empty
  EXPECT_TRUE(st.hot);

  global_texts(idx, "app", 10, &st);  // more letters than the table covers
  EXPECT_FALSE(st.hot);
  global_texts(idx, "a", 11, &st);  // more results than it keeps
  EXPECT_FALSE(st.hot);
  global_texts(idx, "apple p", 10, &st);  // something typed before the partial word
  EXPECT_FALSE(st.hot);
  global_texts(idx, "apple ", 10, &st);  // no partial word at all
  EXPECT_FALSE(st.hot);

  // the ranking by mean word frequency is not served from it
  const auto ap = idx.find_word("apple");
  ASSERT_TRUE(ap.has_value());
  Index::ScanStats st2;
  idx.top_global({}, "a", 5, Index::ScanLimits{}, st2, Index::GlobalRank::AverageWordFrequency);
  EXPECT_FALSE(st2.hot);
  Index::ScanStats st3;
  idx.top_global({}, "a", 5, Index::ScanLimits{}, st3, Index::GlobalRank::Frequency);
  EXPECT_TRUE(st3.hot);
}

TEST(HotPrefixes, SwitchedOffIndexNeverUsesIt) {
  Index idx;
  for (const char* q : {"apple", "apple pie", "banana"}) add(idx, q, 2);
  Index::ScanStats st;
  EXPECT_EQ(global_texts(idx, "a", 10, &st).size(), 2u);
  EXPECT_FALSE(st.hot);

  idx.enable_hot_prefixes({2, 10});
  global_texts(idx, "a", 10, &st);
  EXPECT_TRUE(st.hot);
  idx.enable_hot_prefixes({0, 10});  // off again, and the table is gone
  global_texts(idx, "a", 10, &st);
  EXPECT_FALSE(st.hot);
  EXPECT_EQ(idx.stats().hot_prefixes, 0u);
  EXPECT_EQ(idx.hot_prefix_config().letters, 0u);
  idx.enable_hot_prefixes({2, 0});  // nothing to serve either
  EXPECT_EQ(idx.hot_prefix_config().letters, 0u);
}

TEST(HotPrefixes, StatsAndLetterLimit) {
  Index idx;
  add(idx, "a b", 1);
  add(idx, "ab c", 2);
  add(idx, "b", 3);
  add_user(idx, 1, "bz only the user", 1, 0, false);  // never in the global pool: not listed

  idx.enable_hot_prefixes({2, 10});
  EXPECT_EQ(idx.stats().hot_prefixes, 3u);  // a, ab, b
  EXPECT_EQ(idx.stats().hot_entries, 4u);   // "a b" and "ab c" under a; "ab c" under ab; "b" under b
  idx.enable_hot_prefixes({1, 10});
  EXPECT_EQ(idx.stats().hot_prefixes, 2u);  // a, b
  EXPECT_EQ(idx.stats().hot_entries, 3u);
  idx.enable_hot_prefixes({9, 10});
  EXPECT_EQ(idx.hot_prefix_config().letters, 4u);  // at most four letters
}

TEST(HotPrefixes, AOneLetterFirstWordBelongsOnlyToItsOwnPrefix) {
  Index idx;
  add(idx, "a x", 5);
  add(idx, "ab y", 4);
  idx.enable_hot_prefixes({2, 10});
  EXPECT_EQ(global_texts(idx, "a", 10), (std::vector<std::string>{"a x", "ab y"}));
  EXPECT_EQ(global_texts(idx, "ab", 10), (std::vector<std::string>{"ab y"}));
  EXPECT_EQ(global_texts(idx, "a", 1), (std::vector<std::string>{"a x"}));
}

TEST(HotPrefixes, AListThatLostMembersFallsBackToTheScanAndRecovers) {
  Index idx;
  std::vector<QueryId> ids;
  for (int i = 0; i < 10; ++i)
    ids.push_back(add(idx, "a" + std::string(1, static_cast<char>('a' + i)) + " q", 100 - i).id);
  idx.enable_hot_prefixes({1, 2});  // serves n <= 2, keeps 4

  Index::ScanStats st;
  EXPECT_EQ(global_texts(idx, "a", 2, &st), (std::vector<std::string>{"aa q", "ab q"}));
  EXPECT_TRUE(st.hot);

  // three of the four kept ones go: one member is left, fewer than the 2 asked for
  ASSERT_TRUE(idx.remove_global(ids[0]));
  ASSERT_TRUE(idx.remove_global(ids[1]));
  EXPECT_EQ(global_texts(idx, "a", 2, &st), (std::vector<std::string>{"ac q", "ad q"}));
  EXPECT_TRUE(st.hot);  // two members still, so the list is enough
  ASSERT_TRUE(idx.remove_global(ids[2]));
  EXPECT_EQ(global_texts(idx, "a", 2, &st), (std::vector<std::string>{"ad q", "ae q"}));
  EXPECT_FALSE(st.hot);  // one member left, but the 2 best of the rest are not known: scanned
  EXPECT_EQ(global_texts(idx, "a", 1, &st), (std::vector<std::string>{"ad q"}));
  EXPECT_TRUE(st.hot);  // one is enough for n = 1

  // a better query than the worst member is taken in again
  add(idx, "az q", 1000);
  EXPECT_EQ(global_texts(idx, "a", 2, &st), (std::vector<std::string>{"az q", "ad q"}));
  EXPECT_TRUE(st.hot);
}

TEST(HotPrefixes, EmptyListsDisappearAndNewPrefixesAppear) {
  Index idx;
  const auto a = add(idx, "apple", 2);
  idx.enable_hot_prefixes({2, 5});
  EXPECT_EQ(idx.stats().hot_prefixes, 2u);  // a, ap
  ASSERT_TRUE(idx.remove_global(a.id));
  EXPECT_EQ(idx.stats().hot_prefixes, 0u);  // the set is empty again: no list for it

  Index::ScanStats st;
  EXPECT_TRUE(global_texts(idx, "ap", 5, &st).empty());
  EXPECT_TRUE(st.hot);
  add(idx, "apricot", 1);  // a prefix that had no list gets one
  EXPECT_EQ(global_texts(idx, "ap", 5, &st), (std::vector<std::string>{"apricot"}));
  EXPECT_TRUE(st.hot);
  EXPECT_EQ(idx.stats().hot_prefixes, 2u);
}

TEST(HotPrefixes, ARaisedQueryMovesUp) {
  Index idx;
  add(idx, "alpha", 5);
  const auto beta = add(idx, "alps", 3);
  idx.enable_hot_prefixes({2, 5});
  EXPECT_EQ(global_texts(idx, "al", 2), (std::vector<std::string>{"alpha", "alps"}));
  add(idx, "alps", 10);  // 13 now
  Index::ScanStats st;
  EXPECT_EQ(global_texts(idx, "al", 2, &st), (std::vector<std::string>{"alps", "alpha"}));
  EXPECT_TRUE(st.hot);
  // a query that exists only for a user joins the pool through also_global
  add_user(idx, 9, "almond", 1, 0, false);
  EXPECT_EQ(global_texts(idx, "al", 5).size(), 2u);
  add_user(idx, 9, "almond", 20, 0, true);
  EXPECT_EQ(global_texts(idx, "al", 5), (std::vector<std::string>{"almond", "alps", "alpha"}));
  (void)beta;
}

// The table is derived data: it is neither saved nor carried into a compacted copy, and
// whoever loads or compacts an index switches it on again.
TEST(HotPrefixes, SaveLoadAndCompactionDoNotCarryTheTable) {
  Driver d(5);
  d.idx.enable_hot_prefixes({2, 4});
  for (int i = 0; i < 400; ++i) d.step();
  d.check("before");

  Index compact = d.idx.compacted();
  EXPECT_EQ(compact.hot_prefix_config().letters, 0u);
  compact.enable_hot_prefixes({2, 4});

  std::stringstream ss;
  d.idx.save(ss);
  Index loaded = Index::load(ss);
  EXPECT_EQ(loaded.hot_prefix_config().letters, 0u);
  loaded.enable_hot_prefixes({2, 4});

  for (const std::string p : {"a", "b", "c", "ab", "ca", "cc"}) {
    for (std::size_t n : {1u, 4u}) {
      const auto want = d.model.top(p, n);
      EXPECT_EQ(global_texts(compact, p, n), want) << p;
      EXPECT_EQ(global_texts(loaded, p, n), want) << p;
    }
  }
  // and writes after the switch-on are followed by the copies as well
  add(compact, "a zz", 999);
  add(loaded, "a zz", 999);
  EXPECT_EQ(global_texts(compact, "a", 1), (std::vector<std::string>{"a zz"}));
  EXPECT_EQ(global_texts(loaded, "a", 1), (std::vector<std::string>{"a zz"}));
}

// A list that was complete when the table was built, outgrows its capacity through live
// writes (so it no longer holds everything) and is then shortened by removals must not be
// mistaken for a complete one: it has to hand over to the scan.
TEST(HotPrefixes, OverflowingTheCapacityMakesAListIncomplete) {
  Index idx;
  add(idx, "aa q", 60);
  add(idx, "ab q", 50);
  idx.enable_hot_prefixes({1, 2});  // serves n <= 2, keeps 4

  std::vector<QueryId> ids;
  for (const auto& [text, freq] : std::vector<std::pair<std::string, int>>{
           {"ac q", 40}, {"ad q", 30}, {"ae q", 20}, {"af q", 10}})
    ids.push_back(add(idx, text, freq).id);

  Index::ScanStats st;
  EXPECT_EQ(global_texts(idx, "a", 2, &st), (std::vector<std::string>{"aa q", "ab q"}));
  EXPECT_TRUE(st.hot);

  // take out the three best: the list holds one query, but five were ever beneath it
  ASSERT_TRUE(idx.remove_global(1));  // "aa q" is query id 1, "ab q" id 2
  ASSERT_TRUE(idx.remove_global(2));
  ASSERT_TRUE(idx.remove_global(ids[0]));  // "ac q"
  EXPECT_EQ(global_texts(idx, "a", 2, &st), (std::vector<std::string>{"ad q", "ae q"}));
  EXPECT_FALSE(st.hot) << "an incomplete list that is too short must not be served";
  EXPECT_EQ(global_texts(idx, "a", 1, &st), (std::vector<std::string>{"ad q"}));
  EXPECT_TRUE(st.hot);
}

// A partial word after complete words that no stored query starts with is answered without
// touching the dictionary, and the answer does not change.
TEST(PartialWordLookup, SkipsTheDictionaryWhenTheCompleteWordsMatchNothing) {
  Index idx;
  for (const char* q : {"red fox jumps", "red fish", "blue car", "blue cart wheel"}) add(idx, q, 2);

  Index::ScanStats st;
  EXPECT_TRUE(global_texts(idx, "red car ca", 10, &st).empty());  // both words exist, not in this order
  EXPECT_EQ(st.visited, 0u);
  EXPECT_FALSE(st.truncated);

  // lookups that do match are as before
  EXPECT_EQ(global_texts(idx, "blue ca", 10, &st), (std::vector<std::string>{"blue car", "blue cart wheel"}));
  EXPECT_GT(st.visited, 0u);
  EXPECT_EQ(global_texts(idx, "red f", 10, &st), (std::vector<std::string>{"red fox jumps", "red fish"}));  // equal counts: older first
  // a prefix that exists only as the start of a longer stored query still matches through it
  EXPECT_EQ(global_texts(idx, "red fox ju", 10), (std::vector<std::string>{"red fox jumps"}));
}
