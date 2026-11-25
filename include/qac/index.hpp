// The in-memory search index.
//
//   words_      HAT-trie  alphabet-encoded word bytes -> WordId (u32)
//   queries_    HAT-trie  key = sequence of WordIds (u32 letters) -> {QueryId, freq, refs}
//   users_      HAT-trie  key = [user slot, WordIds...]           -> {QueryId, freq, last_used}
//   query table QueryId -> WordId sequence (needed to remove a query by id and to
//               render suggestions without reconstructing trie keys)
//
// A query is therefore a sequence of 4-byte word ids instead of its text, and
// the HAT-trie compresses the shared prefixes of those sequences.
//
// Query-id lifecycle (this makes compaction a purely physical operation):
//   * one id per distinct query text, shared by the global pool and all users;
//   * ids are allocated sequentially and never reused;
//   * a query has an entry in `queries_` exactly while it is in the global pool
//     (freq > 0) or in at least one user history (refs > 0). When both stop
//     being true the entry and the id die, and re-adding the text yields a new id.
//
// Index is NOT thread-safe: callers (Engine) provide a reader/writer lock.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <tsl/htrie_map.h>

#include "qac/common.hpp"

namespace qac {

// Stateless, deterministic hash. Tries are deserialized with the "hash
// compatible" fast path, so this function must never change without bumping
// Index::kHashId.
template <class CharT>
struct KeyHash {
  std::size_t operator()(const CharT* key, std::size_t n) const noexcept {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(n) * 0xFF51AFD7ED558CCDull);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<CharT>>(key[i])) +
           0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
      h ^= h >> 32;
    }
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return static_cast<std::size_t>(h);
  }
};

struct QueryVal {
  QueryId id = kNoQuery;
  std::uint32_t freq = 0;  // global frequency, 0 = not in the global pool
  std::uint32_t refs = 0;  // number of user histories containing the query
  template <class Archive>
  void serialize(Archive& ar) {
    ar(id, freq, refs);
  }
};

struct UserVal {
  QueryId id = kNoQuery;
  std::uint32_t freq = 0;
  std::uint32_t last_used = 0;  // unix seconds
  template <class Archive>
  void serialize(Archive& ar) {
    ar(id, freq, last_used);
  }
};

using WordTrie = tsl::htrie_map<char, WordId, KeyHash<char>, std::uint16_t,
                                tsl::htrie_policy::bitmap_children>;
using QueryTrie = tsl::htrie_map<std::uint32_t, QueryVal, KeyHash<std::uint32_t>, std::uint16_t,
                                 tsl::htrie_policy::flat_map_children>;
using UserTrie = tsl::htrie_map<std::uint32_t, UserVal, KeyHash<std::uint32_t>, std::uint16_t,
                                tsl::htrie_policy::flat_map_children>;

class Index {
 public:
  static constexpr std::uint32_t kFormatVersion = 1;
  static constexpr std::uint64_t kHashId = 0x5143484153480001ull;

  // How the global candidates are ranked.
  enum class GlobalRank : std::uint8_t {
    Frequency,             // by the frequency of the whole query
    AverageWordFrequency,  // by the mean frequency of the words the query consists of
  };

  struct Hit {
    QueryId id = kNoQuery;
    std::uint32_t freq = 0;
    std::uint32_t last_used = 0;  // user hits only
    // AverageWordFrequency only: the sum of the frequencies of the query's words and
    // their number. The mean is word_sum / words; ordering uses the exact fraction.
    std::uint64_t word_sum = 0;
    std::uint32_t words = 0;
    double average() const { return words ? static_cast<double>(word_sum) / words : 0.0; }
  };

  struct ScanLimits {
    std::size_t max_visited = 1'000'000;     // trie entries examined per global search
    std::size_t max_user_visited = 200'000;  // trie entries examined per user search
    std::size_t max_candidates = 64;         // words considered for a partially typed word
    std::size_t max_dict_visited = 200'000;  // dictionary entries examined to find them
  };

  struct ScanStats {
    std::size_t visited = 0;  // trie entries looked at, dictionary entries included
    // Entries of the query / user trie that were examined: exactly what
    // ScanLimits::max_visited and max_user_visited bound. The engine uses it to share
    // one budget between the tiers of a tail-fallback lookup.
    std::size_t scanned = 0;
    bool truncated = false;  // a budget ran out: results may be approximate
    bool hot = false;        // answered from the hot-prefix table (exact, nothing scanned)
  };

  // Precomputed top-K lists for the partial first words that cost the most to scan: one
  // to a few letters typed, nothing before them ("a", "we", ...). See enable_hot_prefixes().
  struct HotPrefixConfig {
    unsigned letters = 0;    // longest partial first word served from the table; 0 = off (max 4)
    std::size_t top = 100;   // largest n the table can answer; bigger requests are scanned
  };

  struct AddOutcome {
    QueryId id = kNoQuery;
    std::uint32_t freq = 0;  // frequency after the operation in the pool that was addressed
    bool created = false;    // a new query id was allocated
  };

  struct Stats {
    std::uint64_t words = 0;
    std::uint64_t query_ids_allocated = 0;
    std::uint64_t live_queries = 0;     // entries in the query registry
    std::uint64_t global_queries = 0;   // of which in the global pool
    std::uint64_t user_entries = 0;
    std::uint64_t users = 0;
    std::uint64_t dead_queries = 0;     // ids whose table space is waiting for optimize()
    std::uint64_t arena_bytes = 0;      // word + query tables
    std::uint64_t hot_prefixes = 0;     // prefixes with a precomputed list
    std::uint64_t hot_entries = 0;      // queries held in those lists
  };

  Index();
  Index(Index&&) noexcept = default;
  Index& operator=(Index&&) noexcept = default;
  Index(const Index&) = delete;
  Index& operator=(const Index&) = delete;

  // ---- lookups ----
  std::optional<WordId> find_word(std::string_view encoded) const;
  std::string_view word(WordId id) const;  // encoded bytes
  // Word ids of a live query (nullptr when the id is unknown or dead).
  const std::uint32_t* query_words(QueryId id, std::size_t& count) const;
  bool query_alive(QueryId id) const;

  // ---- updates; `words` are alphabet-encoded tokens, at least one ----
  AddOutcome add_global(const std::vector<std::string_view>& words, std::uint32_t count);
  AddOutcome add_user(std::uint32_t user, const std::vector<std::string_view>& words,
                      std::uint32_t count, std::uint32_t timestamp, bool also_global);
  bool remove_global(QueryId id);                    // false: unknown / not in the global pool
  bool remove_user(std::uint32_t user, QueryId id);  // false: not in that user's history

  // ---- search: `prefix` = ids of the complete words, `partial` = encoded word being typed ----
  // `rank` decides the order of the result; either way the scan visits the same
  // entries (and honours the same budget).
  std::vector<Hit> top_global(const std::vector<WordId>& prefix, std::string_view partial,
                              std::size_t n, const ScanLimits& limits, ScanStats& stats,
                              GlobalRank rank = GlobalRank::Frequency) const;
  std::vector<Hit> top_user(std::uint32_t user, const std::vector<WordId>& prefix,
                            std::string_view partial, std::size_t n, const ScanLimits& limits,
                            ScanStats& stats) const;

  // ---- hot prefixes ----
  // Builds (or rebuilds) the hot-prefix table with one pass over the query trie, and from
  // then on keeps it up to date inside add_global / add_user(also_global) / remove_global,
  // so it needs no lock of its own: readers only ever see it between writes. letters == 0
  // switches it off. The table is not persisted (it is derived data); whoever loads an index
  // calls this again. compacted() does not build it for the copy it returns.
  //
  // top_global() answers from it when `prefix` is empty, the partial word has 1..letters
  // letters, n <= top and the ranking is by frequency. The answer is exact (the plain scan
  // is only approximate for such shapes: 64 dictionary words, a visit budget). Every other
  // shape, and a list that has lost too many members to removals, is scanned as before.
  void enable_hot_prefixes(const HotPrefixConfig& cfg);
  const HotPrefixConfig& hot_prefix_config() const noexcept { return hot_cfg_; }

  // ---- maintenance ----
  // Copy without dead queries and unused words. Observable behaviour (ids,
  // frequencies, search results) is unchanged.
  Index compacted() const;
  Stats stats() const;

  void save(std::ostream& os) const;
  static Index load(std::istream& is);

 private:
  static constexpr std::uint64_t kDead = ~std::uint64_t{0};

  WordId intern_word(std::string_view encoded);
  std::vector<WordId> intern_all(const std::vector<std::string_view>& words);
  QueryId new_query(const std::vector<WordId>& ids);
  void bump_global(QueryVal& q, const std::vector<WordId>& ids, std::uint32_t count);
  void weight_add(const std::uint32_t* w, std::size_t n, std::uint64_t amount);
  void weight_sub(const std::uint32_t* w, std::size_t n, std::uint64_t amount);
  void drop_if_unused(QueryTrie::iterator it);
  std::uint32_t user_slot(std::uint32_t user, bool create);
  std::optional<std::uint32_t> find_user_slot(std::uint32_t user) const;
  std::vector<WordId> candidate_words(std::string_view partial, const ScanLimits& limits,
                                      ScanStats& st) const;

  // ---- hot-prefix table ----
  // For every prefix P of 1..letters letters, a list of live global queries whose FIRST word
  // starts with P, best first (frequency desc, id asc). Invariant: `items` is exactly the best
  // items.size() queries of that set, and `complete` says it holds the whole set. Removal of a
  // member only shortens the list (still exact); a new or raised query is inserted when it
  // beats the worst member (or the list is complete); the list is cut at `capacity`. A prefix
  // without a list is an empty set.
  struct HotItem {
    std::uint32_t freq = 0;
    QueryId id = kNoQuery;
  };
  struct HotList {
    std::vector<HotItem> items;
    bool complete = true;
  };
  std::size_t hot_capacity() const noexcept { return hot_cfg_.top * 2; }
  static std::uint64_t hot_key(const char* p, std::size_t len) noexcept;
  void hot_upsert(WordId first_word, QueryId id, std::uint32_t freq);
  void hot_erase(WordId first_word, QueryId id);
  bool hot_lookup(std::string_view partial, std::size_t n, std::vector<Hit>& out) const;

  WordTrie words_;
  std::string word_arena_;                  // [len:u8][bytes] per word
  std::vector<std::uint64_t> word_off_;     // WordId -> offset into word_arena_
  std::vector<std::uint64_t> word_weight_;  // WordId -> sum of global freq of queries using it

  std::vector<std::uint64_t> query_off_;    // QueryId -> offset into query_arena_, kDead if gone
  std::vector<std::uint32_t> query_arena_;  // [count][WordIds...]

  QueryTrie queries_;
  UserTrie users_;
  // user id -> dense slot (assigned in order of first appearance, which keeps the
  // first-level children of `users_` append-only instead of sorted-insert).
  std::unordered_map<std::uint32_t, std::uint32_t> user_slots_;

  HotPrefixConfig hot_cfg_;
  std::unordered_map<std::uint64_t, HotList> hot_;

  std::uint64_t global_queries_ = 0;
  std::uint64_t user_entries_ = 0;
  std::uint64_t dead_queries_ = 0;
};

}  // namespace qac
