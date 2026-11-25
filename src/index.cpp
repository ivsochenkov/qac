#include "qac/index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>

namespace qac {

namespace {
constexpr std::uint64_t kMaxU64 = ~std::uint64_t{0};
}  // namespace

namespace {

constexpr std::uint32_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kBurstThreshold = 1024;  // small buckets keep prefix scans cheap

std::uint32_t sat_add(std::uint32_t a, std::uint32_t b) {
  const std::uint64_t s = static_cast<std::uint64_t>(a) + b;
  return s > kMaxU32 ? kMaxU32 : static_cast<std::uint32_t>(s);
}

// Global ranking: frequency desc, then older (smaller) id first.
struct GlobalBetter {
  bool operator()(const Index::Hit& a, const Index::Hit& b) const {
    if (a.freq != b.freq) return a.freq > b.freq;
    return a.id < b.id;
  }
};

// Mean word frequency desc, then query frequency desc, then id. The means are compared
// as exact fractions (sum / words) in 128-bit arithmetic: no floating point, so the
// order is deterministic.
__extension__ typedef unsigned __int128 u128;
struct AvgBetter {
  bool operator()(const Index::Hit& a, const Index::Hit& b) const {
    const u128 l = static_cast<u128>(a.word_sum) * b.words;
    const u128 r = static_cast<u128>(b.word_sum) * a.words;
    if (l != r) return l > r;
    if (a.freq != b.freq) return a.freq > b.freq;
    return a.id < b.id;
  }
};

// User ranking: frequency desc, then most recently used, then id.
struct UserBetter {
  bool operator()(const Index::Hit& a, const Index::Hit& b) const {
    if (a.freq != b.freq) return a.freq > b.freq;
    if (a.last_used != b.last_used) return a.last_used > b.last_used;
    return a.id < b.id;
  }
};

// Keeps the n best hits. The heap is ordered by `Better`, so the front of the
// max-heap (the element nothing else is "greater" than) is the WORST of the kept
// ones, which is the one to evict when a better hit arrives.
template <class Better>
class TopK {
 public:
  explicit TopK(std::size_t n) : n_(n) { heap_.reserve(std::min<std::size_t>(n, 1024)); }

  void push(const Index::Hit& h) {
    if (n_ == 0) return;
    if (heap_.size() < n_) {
      heap_.push_back(h);
      std::push_heap(heap_.begin(), heap_.end(), Better());
    } else if (Better()(h, heap_.front())) {
      std::pop_heap(heap_.begin(), heap_.end(), Better());
      heap_.back() = h;
      std::push_heap(heap_.begin(), heap_.end(), Better());
    }
  }

  std::vector<Index::Hit> take() {
    std::sort(heap_.begin(), heap_.end(), Better());
    return std::move(heap_);
  }

 private:
  std::size_t n_;
  std::vector<Index::Hit> heap_;
};

// Adapters between hat-trie's (de)serializer concept and a cereal binary archive.
template <class Archive>
struct TrieWriter {
  Archive& ar;
  template <class U>
  void operator()(const U& value) {
    ar(value);
  }
  template <class C>
  void operator()(const C* data, std::size_t n) {
    ar.saveBinary(data, n * sizeof(C));
  }
};

template <class Archive>
struct TrieReader {
  Archive& ar;
  template <class U>
  U operator()() {
    U value{};
    ar(value);
    return value;
  }
  template <class C>
  void operator()(C* out, std::size_t n) {
    ar.loadBinary(out, n * sizeof(C));
  }
};

}  // namespace

Index::Index() {
  words_.burst_threshold(kBurstThreshold);
  queries_.burst_threshold(kBurstThreshold);
  users_.burst_threshold(kBurstThreshold);
  query_off_.push_back(kDead);  // query id 0 is "no query"
}

// ---------------------------------------------------------------- lookups

std::optional<WordId> Index::find_word(std::string_view encoded) const {
  const auto it = words_.find_ks(encoded.data(), encoded.size());
  if (it == words_.end()) return std::nullopt;
  return it.value();
}

std::string_view Index::word(WordId id) const {
  const std::uint64_t off = word_off_.at(id);
  const auto len = static_cast<unsigned char>(word_arena_[off]);
  return std::string_view(word_arena_.data() + off + 1, len);
}

const std::uint32_t* Index::query_words(QueryId id, std::size_t& count) const {
  count = 0;
  if (id == kNoQuery || id >= query_off_.size()) return nullptr;
  const std::uint64_t off = query_off_[id];
  if (off == kDead) return nullptr;
  count = query_arena_[off];
  return query_arena_.data() + off + 1;
}

bool Index::query_alive(QueryId id) const {
  return id != kNoQuery && id < query_off_.size() && query_off_[id] != kDead;
}

// ---------------------------------------------------------------- internals

WordId Index::intern_word(std::string_view encoded) {
  const auto it = words_.find_ks(encoded.data(), encoded.size());
  if (it != words_.end()) return it.value();
  if (word_off_.size() >= kMaxU32) throw std::length_error("word id space exhausted");
  const auto id = static_cast<WordId>(word_off_.size());
  word_off_.push_back(word_arena_.size());
  word_arena_.push_back(static_cast<char>(static_cast<unsigned char>(encoded.size())));
  word_arena_.append(encoded.data(), encoded.size());
  word_weight_.push_back(0);
  words_.insert_ks(encoded.data(), encoded.size(), id);
  return id;
}

std::vector<WordId> Index::intern_all(const std::vector<std::string_view>& words) {
  std::vector<WordId> ids;
  ids.reserve(words.size());
  for (const auto& w : words) ids.push_back(intern_word(w));
  return ids;
}

QueryId Index::new_query(const std::vector<WordId>& ids) {
  if (query_off_.size() >= kMaxU32) throw std::length_error("query id space exhausted");
  const auto id = static_cast<QueryId>(query_off_.size());
  query_off_.push_back(query_arena_.size());
  query_arena_.push_back(static_cast<std::uint32_t>(ids.size()));
  query_arena_.insert(query_arena_.end(), ids.begin(), ids.end());
  return id;
}

void Index::weight_add(const std::uint32_t* w, std::size_t n, std::uint64_t amount) {
  for (std::size_t i = 0; i < n; ++i) word_weight_[w[i]] += amount;
}

void Index::weight_sub(const std::uint32_t* w, std::size_t n, std::uint64_t amount) {
  for (std::size_t i = 0; i < n; ++i) {
    auto& x = word_weight_[w[i]];
    x = x > amount ? x - amount : 0;
  }
}

void Index::bump_global(QueryVal& q, const std::vector<WordId>& ids, std::uint32_t count) {
  const std::uint32_t before = q.freq;
  q.freq = sat_add(before, count);
  if (before == 0 && q.freq > 0) ++global_queries_;
  weight_add(ids.data(), ids.size(), q.freq - before);
  if (hot_cfg_.letters != 0 && !ids.empty()) hot_upsert(ids[0], q.id, q.freq);
}

void Index::drop_if_unused(QueryTrie::iterator it) {
  const QueryVal& q = it.value();
  if (q.freq != 0 || q.refs != 0) return;
  const QueryId id = q.id;
  query_off_[id] = kDead;
  ++dead_queries_;
  queries_.erase(it);
}

std::uint32_t Index::user_slot(std::uint32_t user, bool create) {
  const auto it = user_slots_.find(user);
  if (it != user_slots_.end()) return it->second;
  if (!create) throw std::logic_error("unknown user");
  const auto slot = static_cast<std::uint32_t>(user_slots_.size());
  user_slots_.emplace(user, slot);
  return slot;
}

std::optional<std::uint32_t> Index::find_user_slot(std::uint32_t user) const {
  const auto it = user_slots_.find(user);
  if (it == user_slots_.end()) return std::nullopt;
  return it->second;
}

// ---------------------------------------------------------------- updates

Index::AddOutcome Index::add_global(const std::vector<std::string_view>& words,
                                    std::uint32_t count) {
  if (words.empty()) throw std::invalid_argument("empty query");
  if (count == 0) throw std::invalid_argument("count must be positive");
  const std::vector<WordId> ids = intern_all(words);

  AddOutcome out;
  auto it = queries_.find_ks(ids.data(), ids.size());
  if (it == queries_.end()) {
    const QueryId id = new_query(ids);
    it = queries_.insert_ks(ids.data(), ids.size(), QueryVal{id, 0, 0}).first;
    out.created = true;
  }
  QueryVal& q = it.value();
  bump_global(q, ids, count);
  out.id = q.id;
  out.freq = q.freq;
  return out;
}

Index::AddOutcome Index::add_user(std::uint32_t user, const std::vector<std::string_view>& words,
                                  std::uint32_t count, std::uint32_t timestamp,
                                  bool also_global) {
  if (words.empty()) throw std::invalid_argument("empty query");
  if (count == 0) throw std::invalid_argument("count must be positive");
  const std::vector<WordId> ids = intern_all(words);

  AddOutcome out;
  auto qit = queries_.find_ks(ids.data(), ids.size());
  if (qit == queries_.end()) {
    const QueryId id = new_query(ids);
    qit = queries_.insert_ks(ids.data(), ids.size(), QueryVal{id, 0, 0}).first;
    out.created = true;
  }
  QueryVal& q = qit.value();

  const std::uint32_t slot = user_slot(user, true);
  std::vector<std::uint32_t> ukey;
  ukey.reserve(ids.size() + 1);
  ukey.push_back(slot);
  ukey.insert(ukey.end(), ids.begin(), ids.end());

  const auto uit = users_.find_ks(ukey.data(), ukey.size());
  if (uit != users_.end()) {
    UserVal& u = uit.value();
    u.freq = sat_add(u.freq, count);
    u.last_used = std::max(u.last_used, timestamp);
    out.freq = u.freq;
  } else {
    users_.insert_ks(ukey.data(), ukey.size(), UserVal{q.id, count, timestamp});
    ++q.refs;
    ++user_entries_;
    out.freq = count;
  }
  if (also_global) bump_global(q, ids, count);
  out.id = q.id;
  return out;
}

bool Index::remove_global(QueryId id) {
  std::size_t n = 0;
  const std::uint32_t* w = query_words(id, n);
  if (w == nullptr) return false;
  const auto it = queries_.find_ks(w, n);
  if (it == queries_.end()) return false;
  QueryVal& q = it.value();
  if (q.freq == 0) return false;
  weight_sub(w, n, q.freq);
  q.freq = 0;
  --global_queries_;
  if (hot_cfg_.letters != 0 && n > 0) hot_erase(w[0], id);
  drop_if_unused(it);
  return true;
}

bool Index::remove_user(std::uint32_t user, QueryId id) {
  const auto slot = find_user_slot(user);
  if (!slot) return false;
  std::size_t n = 0;
  const std::uint32_t* w = query_words(id, n);
  if (w == nullptr) return false;

  std::vector<std::uint32_t> ukey;
  ukey.reserve(n + 1);
  ukey.push_back(*slot);
  ukey.insert(ukey.end(), w, w + n);
  const auto uit = users_.find_ks(ukey.data(), ukey.size());
  if (uit == users_.end()) return false;
  users_.erase(uit);
  --user_entries_;

  const auto qit = queries_.find_ks(w, n);
  if (qit != queries_.end()) {
    QueryVal& q = qit.value();
    if (q.refs > 0) --q.refs;
    drop_if_unused(qit);
  }
  return true;
}

// ---------------------------------------------------------------- search

std::vector<WordId> Index::candidate_words(std::string_view partial, const ScanLimits& limits,
                                           ScanStats& st) const {
  struct Cand {
    std::uint64_t weight;
    WordId id;
  };
  // best first: higher weight, then smaller id
  const auto better = [](const Cand& a, const Cand& b) {
    return a.weight != b.weight ? a.weight > b.weight : a.id < b.id;
  };
  // A max-heap ordered by `better` keeps the WORST kept candidate at the front.
  std::vector<Cand> heap;
  const std::size_t cap = std::max<std::size_t>(limits.max_candidates, 1);
  std::size_t visited = 0;
  std::size_t matched = 0;

  auto range = words_.equal_prefix_range_ks(partial.data(), partial.size());
  for (auto it = range.first; it != range.second; ++it) {
    if (++visited > limits.max_dict_visited) {
      st.truncated = true;
      break;
    }
    const WordId id = it.value();
    const std::uint64_t weight = word_weight_[id];
    if (weight == 0) continue;  // not used by any query of the global pool
    ++matched;
    const Cand c{weight, id};
    if (heap.size() < cap) {
      heap.push_back(c);
      std::push_heap(heap.begin(), heap.end(), better);
    } else if (better(c, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), better);
      heap.back() = c;
      std::push_heap(heap.begin(), heap.end(), better);
    }
  }
  if (matched > cap) st.truncated = true;
  st.visited += visited;

  std::sort(heap.begin(), heap.end(), better);
  std::vector<WordId> out;
  out.reserve(heap.size());
  for (const auto& c : heap) out.push_back(c.id);
  return out;
}

std::vector<Index::Hit> Index::top_global(const std::vector<WordId>& prefix,
                                          std::string_view partial, std::size_t n,
                                          const ScanLimits& limits, ScanStats& stats,
                                          GlobalRank rank) const {
  if (n == 0 || (prefix.empty() && partial.empty())) return {};

  // A partial first word of a few letters is the most expensive shape to scan; the
  // hot-prefix table answers it exactly (see enable_hot_prefixes).
  if (rank == GlobalRank::Frequency && prefix.empty() && hot_cfg_.letters != 0 &&
      partial.size() <= hot_cfg_.letters && n <= hot_cfg_.top) {
    std::vector<Hit> hot;
    if (hot_lookup(partial, n, hot)) {
      stats.hot = true;
      return hot;
    }
  }

  // A partial last word is resolved through the dictionary (candidate_words), which is the
  // expensive part of the lookup. If no stored query starts with the complete words in front
  // of it, nothing can match, whatever the word: one probe of the trie saves all of that. (The
  // tail fallback relies on this: its shorter lookups often match nothing.)
  if (!prefix.empty() && !partial.empty()) {
    const auto probe = queries_.equal_prefix_range_ks(prefix.data(), prefix.size());
    if (probe.first == probe.second) return {};
  }

  std::size_t budget = limits.max_visited;

  // Walks the candidates and offers each live global query to `top`, built by `make_hit`.
  // Returns false once the budget is exhausted.
  const auto search = [&](auto& top, auto&& make_hit) {
    const auto scan = [&](const std::vector<std::uint32_t>& key) {
      auto range = queries_.equal_prefix_range_ks(key.data(), key.size());
      for (auto it = range.first; it != range.second; ++it) {
        if (budget == 0) {
          stats.truncated = true;
          return false;
        }
        --budget;
        ++stats.visited;
        ++stats.scanned;
        const QueryVal& v = it.value();
        if (v.freq == 0) continue;  // user-only query
        top.push(make_hit(v));
      }
      return true;
    };

    if (partial.empty()) {
      scan(prefix);
    } else {
      // The word being typed has no id yet: probe the trie once for each of the
      // most frequent dictionary words that start with it.
      const std::vector<WordId> cands = candidate_words(partial, limits, stats);
      std::vector<std::uint32_t> key(prefix);
      key.push_back(0);
      for (WordId c : cands) {
        key.back() = c;
        if (!scan(key)) break;
      }
    }
  };

  if (rank == GlobalRank::AverageWordFrequency) {
    TopK<AvgBetter> top(n);
    search(top, [this](const QueryVal& v) {
      Hit h{v.id, v.freq, 0, 0, 0};
      std::size_t cnt = 0;
      const std::uint32_t* w = query_words(v.id, cnt);
      for (std::size_t i = 0; w != nullptr && i < cnt; ++i) {
        const std::uint64_t x = word_weight_[w[i]];
        h.word_sum = h.word_sum > kMaxU64 - x ? kMaxU64 : h.word_sum + x;  // saturating
      }
      h.words = static_cast<std::uint32_t>(cnt);
      return h;
    });
    return top.take();
  }

  TopK<GlobalBetter> top(n);
  search(top, [](const QueryVal& v) { return Hit{v.id, v.freq, 0, 0, 0}; });
  return top.take();
}

std::vector<Index::Hit> Index::top_user(std::uint32_t user, const std::vector<WordId>& prefix,
                                        std::string_view partial, std::size_t n,
                                        const ScanLimits& limits, ScanStats& stats) const {
  if (n == 0) return {};
  const auto slot = find_user_slot(user);
  if (!slot) return {};

  std::vector<std::uint32_t> key;
  key.reserve(prefix.size() + 1);
  key.push_back(*slot);
  key.insert(key.end(), prefix.begin(), prefix.end());

  TopK<UserBetter> top(n);
  std::size_t budget = limits.max_user_visited;
  auto range = users_.equal_prefix_range_ks(key.data(), key.size());
  for (auto it = range.first; it != range.second; ++it) {
    if (budget == 0) {
      stats.truncated = true;
      break;
    }
    --budget;
    ++stats.visited;
    ++stats.scanned;
    const UserVal& v = it.value();
    if (!partial.empty()) {
      std::size_t cnt = 0;
      const std::uint32_t* w = query_words(v.id, cnt);
      if (w == nullptr || cnt <= prefix.size()) continue;
      const std::string_view next = word(w[prefix.size()]);
      if (next.size() < partial.size() || next.compare(0, partial.size(), partial) != 0) continue;
    }
    top.push(Hit{v.id, v.freq, v.last_used});
  }
  return top.take();
}

// ---------------------------------------------------------------- hot prefixes

namespace {

constexpr unsigned kMaxHotLetters = 4;

// Same order as GlobalBetter, for the hot lists' items.
struct HotBetter {
  template <class Item>
  bool operator()(const Item& a, const Item& b) const {
    if (a.freq != b.freq) return a.freq > b.freq;
    return a.id < b.id;
  }
};

}  // namespace

std::uint64_t Index::hot_key(const char* p, std::size_t len) noexcept {
  std::uint64_t packed = 0;
  for (std::size_t i = 0; i < len; ++i) packed = (packed << 8) | static_cast<unsigned char>(p[i]);
  return (static_cast<std::uint64_t>(len) << 32) | packed;  // len <= 4: packed fits 32 bits
}

void Index::enable_hot_prefixes(const HotPrefixConfig& cfg) {
  hot_.clear();
  if (cfg.letters == 0 || cfg.top == 0) {
    hot_cfg_ = HotPrefixConfig{};
    return;
  }
  hot_cfg_.letters = std::min(cfg.letters, kMaxHotLetters);
  hot_cfg_.top = cfg.top;

  const std::size_t cap = hot_capacity();
  const HotBetter better;
  std::unordered_map<std::uint64_t, std::size_t> total;  // members seen per prefix

  for (auto it = queries_.begin(); it != queries_.end(); ++it) {
    const QueryVal& v = it.value();
    if (v.freq == 0) continue;  // user-only query
    std::size_t n = 0;
    const std::uint32_t* w = query_words(v.id, n);
    if (w == nullptr || n == 0) continue;
    const std::string_view first = word(w[0]);
    const HotItem item{v.freq, v.id};
    const std::size_t longest = std::min<std::size_t>(hot_cfg_.letters, first.size());
    for (std::size_t len = 1; len <= longest; ++len) {
      const std::uint64_t key = hot_key(first.data(), len);
      ++total[key];
      auto& items = hot_[key].items;
      // A bounded heap whose front is the worst kept item (see TopK).
      if (items.size() < cap) {
        items.push_back(item);
        std::push_heap(items.begin(), items.end(), better);
      } else if (better(item, items.front())) {
        std::pop_heap(items.begin(), items.end(), better);
        items.back() = item;
        std::push_heap(items.begin(), items.end(), better);
      }
    }
  }
  for (auto& kv : hot_) {
    std::sort(kv.second.items.begin(), kv.second.items.end(), better);
    kv.second.complete = total[kv.first] <= cap;
  }
}

void Index::hot_upsert(WordId first_word, QueryId id, std::uint32_t freq) {
  if (hot_cfg_.letters == 0 || freq == 0) return;
  const std::string_view first = word(first_word);
  const std::size_t cap = hot_capacity();
  const HotBetter better;
  const HotItem item{freq, id};
  const std::size_t longest = std::min<std::size_t>(hot_cfg_.letters, first.size());
  for (std::size_t len = 1; len <= longest; ++len) {
    HotList& list = hot_[hot_key(first.data(), len)];  // a missing prefix is an empty, complete set
    auto& items = list.items;
    const auto cur = std::find_if(items.begin(), items.end(),
                                  [id](const HotItem& x) { return x.id == id; });
    if (cur != items.end()) {
      items.erase(cur);  // a member whose frequency rose: put it back at its new place
    } else if (!list.complete && (items.empty() || !better(item, items.back()))) {
      continue;  // not among the best items.size() queries: the list stays exact as it is
    }
    items.insert(std::lower_bound(items.begin(), items.end(), item, better), item);
    if (items.size() > cap) {
      items.pop_back();
      list.complete = false;
    }
  }
}

void Index::hot_erase(WordId first_word, QueryId id) {
  if (hot_cfg_.letters == 0) return;
  const std::string_view first = word(first_word);
  const std::size_t longest = std::min<std::size_t>(hot_cfg_.letters, first.size());
  for (std::size_t len = 1; len <= longest; ++len) {
    const auto it = hot_.find(hot_key(first.data(), len));
    if (it == hot_.end()) continue;
    auto& items = it->second.items;
    const auto cur = std::find_if(items.begin(), items.end(),
                                  [id](const HotItem& x) { return x.id == id; });
    if (cur != items.end()) items.erase(cur);  // the rest is still the exact top of what is left
    if (it->second.complete && items.empty()) hot_.erase(it);
  }
}

bool Index::hot_lookup(std::string_view partial, std::size_t n, std::vector<Hit>& out) const {
  out.clear();
  const auto it = hot_.find(hot_key(partial.data(), partial.size()));
  if (it == hot_.end()) return true;  // no query starts with it
  const HotList& list = it->second;
  if (!list.complete && list.items.size() < n) return false;  // lost members: scan instead
  const std::size_t take = std::min(n, list.items.size());
  out.reserve(take);
  for (std::size_t i = 0; i < take; ++i)
    out.push_back(Hit{list.items[i].id, list.items[i].freq, 0, 0, 0});
  return true;
}

// ---------------------------------------------------------------- maintenance

Index Index::compacted() const {
  Index out;
  out.user_slots_ = user_slots_;
  out.query_off_.assign(query_off_.size(), kDead);
  out.global_queries_ = global_queries_;
  out.user_entries_ = user_entries_;
  out.dead_queries_ = 0;

  // Renumber the words that live queries use (in query-id order) and copy the
  // query table, keeping query ids unchanged.
  std::vector<WordId> remap(word_off_.size(), kMaxU32);
  std::vector<WordId> mapped;
  for (QueryId id = 1; id < query_off_.size(); ++id) {
    std::size_t n = 0;
    const std::uint32_t* w = query_words(id, n);
    if (w == nullptr) continue;
    mapped.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      WordId& r = remap[w[i]];
      if (r == kMaxU32) r = out.intern_word(word(w[i]));
      mapped[i] = r;
    }
    out.query_off_[id] = out.query_arena_.size();
    out.query_arena_.push_back(static_cast<std::uint32_t>(n));
    out.query_arena_.insert(out.query_arena_.end(), mapped.begin(), mapped.end());
  }

  for (auto it = queries_.begin(); it != queries_.end(); ++it) {
    const QueryVal& v = it.value();
    std::size_t n = 0;
    const std::uint32_t* w = out.query_words(v.id, n);
    if (w == nullptr) continue;
    out.queries_.insert_ks(w, n, v);
    if (v.freq != 0) out.weight_add(w, n, v.freq);
  }

  UserTrie::key_buffer_type old_key;
  std::vector<std::uint32_t> new_key;
  for (auto it = users_.begin(); it != users_.end(); ++it) {
    const UserVal& v = it.value();
    std::size_t n = 0;
    const std::uint32_t* w = out.query_words(v.id, n);
    if (w == nullptr) continue;
    it.key(old_key);
    new_key.clear();
    new_key.push_back(old_key[0]);  // user slot
    new_key.insert(new_key.end(), w, w + n);
    out.users_.insert_ks(new_key.data(), new_key.size(), v);
  }
  return out;
}

Index::Stats Index::stats() const {
  Stats s;
  s.words = word_off_.size();
  s.query_ids_allocated = query_off_.size() - 1;
  s.live_queries = queries_.size();
  s.global_queries = global_queries_;
  s.user_entries = user_entries_;
  s.users = user_slots_.size();
  s.dead_queries = dead_queries_;
  s.arena_bytes = word_arena_.size() + word_off_.size() * 16 + query_arena_.size() * 4 +
                  query_off_.size() * 8;
  s.hot_prefixes = hot_.size();
  for (const auto& kv : hot_) s.hot_entries += kv.second.items.size();
  return s;
}

// ---------------------------------------------------------------- persistence

void Index::save(std::ostream& os) const {
  cereal::BinaryOutputArchive ar(os);
  const std::uint32_t version = kFormatVersion;
  const std::uint64_t hash_id = kHashId;
  ar(version, hash_id);
  ar(global_queries_, user_entries_, dead_queries_);
  ar(word_arena_, word_off_, word_weight_, query_off_, query_arena_, user_slots_);
  TrieWriter<cereal::BinaryOutputArchive> w{ar};
  words_.serialize(w);
  queries_.serialize(w);
  users_.serialize(w);
}

Index Index::load(std::istream& is) {
  cereal::BinaryInputArchive ar(is);
  std::uint32_t version = 0;
  std::uint64_t hash_id = 0;
  ar(version, hash_id);
  if (version != kFormatVersion)
    throw std::runtime_error("unsupported index format version " + std::to_string(version));

  Index idx;
  ar(idx.global_queries_, idx.user_entries_, idx.dead_queries_);
  ar(idx.word_arena_, idx.word_off_, idx.word_weight_, idx.query_off_, idx.query_arena_,
     idx.user_slots_);

  // Tries can be loaded without rehashing only if the hash function is the same.
  const bool compatible = hash_id == kHashId;
  TrieReader<cereal::BinaryInputArchive> r{ar};
  idx.words_ = WordTrie::deserialize(r, compatible);
  idx.queries_ = QueryTrie::deserialize(r, compatible);
  idx.users_ = UserTrie::deserialize(r, compatible);

  if (idx.word_off_.size() != idx.word_weight_.size() || idx.query_off_.empty() ||
      idx.query_off_[0] != kDead)
    throw std::runtime_error("corrupt index: inconsistent tables");
  for (std::uint64_t off : idx.query_off_)
    if (off != kDead && off >= idx.query_arena_.size())
      throw std::runtime_error("corrupt index: query offset out of range");
  return idx;
}

}  // namespace qac
