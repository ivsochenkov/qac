#include "qac/engine.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>


#include "qac/log.hpp"

namespace qac {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

const char* to_string(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::BadInput: return "bad_input";
    case Status::NotFound: return "not_found";
    case Status::Busy: return "busy";
    case Status::Unavailable: return "unavailable";
    case Status::Error: return "error";
  }
  return "?";
}

bool parse_snapshot_mode(const std::string& s, SnapshotMode& out) {
  if (s == "fork") out = SnapshotMode::Fork;
  else if (s == "inline") out = SnapshotMode::Inline;
  else return false;
  return true;
}

// ---------------------------------------------------------------- cache

class ResultCache {
 public:
  struct Entry {
    std::vector<Suggestion> hits;
    bool truncated = false;
  };

  ResultCache(std::size_t capacity, unsigned ttl_ms)
      : cap_(capacity), ttl_(std::chrono::milliseconds(ttl_ms)) {}

  bool get(const std::string& key, Entry& out) {
    if (cap_ == 0) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = map_.find(key);
    if (it == map_.end() || Clock::now() - it->second->at > ttl_) {
      if (it != map_.end()) {
        lru_.erase(it->second);
        map_.erase(it);
      }
      ++misses_;
      return false;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    out = it->second->entry;
    ++hits_;
    return true;
  }

  void put(std::string key, Entry entry) {
    if (cap_ == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = map_.find(key);
    if (it != map_.end()) {
      lru_.erase(it->second);
      map_.erase(it);
    }
    lru_.push_front(Node{key, std::move(entry), Clock::now()});
    map_[std::move(key)] = lru_.begin();
    while (map_.size() > cap_) {
      map_.erase(lru_.back().key);
      lru_.pop_back();
    }
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    lru_.clear();
    map_.clear();
  }

  std::uint64_t hits() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
  }
  std::uint64_t misses() const {
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
  }

 private:
  struct Node {
    std::string key;
    Entry entry;
    Clock::time_point at;
  };
  std::size_t cap_;
  Clock::duration ttl_;
  mutable std::mutex mu_;
  std::list<Node> lru_;
  std::unordered_map<std::string, std::list<Node>::iterator> map_;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
};

// ---------------------------------------------------------------- helpers

namespace {

std::uint32_t now_unix() { return static_cast<std::uint32_t>(std::time(nullptr)); }

struct Applied {
  bool ok = false;
  Index::AddOutcome out;
};

// The single place where a WAL record changes the index; used by live writes,
// crash recovery and optimize's catch-up, so all three behave identically.
Applied apply_record(Index& idx, const WalRecord& r) {
  Applied a;
  switch (r.op) {
    case WalOp::AddGlobal: {
      const std::vector<std::string_view> views(r.words.begin(), r.words.end());
      a.out = idx.add_global(views, r.count);
      a.ok = true;
      break;
    }
    case WalOp::AddUser: {
      const std::vector<std::string_view> views(r.words.begin(), r.words.end());
      a.out = idx.add_user(r.user, views, r.count, r.timestamp, r.also_global);
      a.ok = true;
      break;
    }
    case WalOp::RemoveGlobal:
      a.ok = idx.remove_global(r.id);
      break;
    case WalOp::RemoveUser:
      a.ok = idx.remove_user(r.user, r.id);
      break;
  }
  return a;
}

// In a forked child: drop every inherited descriptor (client sockets, the
// listener, the epoll fd, locks) so the child does not keep connections open
// after the parent closes them.
void close_inherited_fds() {
#ifdef SYS_close_range
  if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
  const long max = std::min<long>(::sysconf(_SC_OPEN_MAX), 65536);
  for (long fd = 3; fd < max; ++fd) ::close(static_cast<int>(fd));
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

Engine::Engine(const Alphabet& alphabet, EngineConfig config)
    : alphabet_(alphabet),
      cfg_(std::move(config)),
      text_(alphabet, cfg_.limits),
      started_(Clock::now()),
      cache_(std::make_unique<ResultCache>(cfg_.cache_entries, cfg_.cache_ttl_ms)) {
  if (cfg_.data_dir.empty()) throw std::runtime_error("data directory is not set");
  recover();
}

Engine::~Engine() {
  stop_background();
  try {
    if (wal_) wal_->sync_if_dirty();
  } catch (...) {
  }
}

std::string Engine::path(const char* name) const {
  return (fs::path(cfg_.data_dir) / name).string();
}

void Engine::recover() {
  dir_lock_ = std::make_unique<DirLock>(cfg_.data_dir);  // throws if owned elsewhere
  open_directory();
}

void Engine::open_directory() {
  std::error_code ec;
  // Leftovers of an interrupted snapshot are never valid.
  fs::remove(path("snapshot.tmp"), ec);
  fs::remove(path("snapshot.opt.tmp"), ec);

  const std::uint64_t fp = alphabet_.fingerprint();
  const std::string snap = path("snapshot.bin");
  if (fs::exists(snap)) {
    LoadedSnapshot loaded = read_snapshot(snap);
    if (loaded.info.alphabet_fingerprint != fp)
      throw std::runtime_error("snapshot " + snap + " was built with a different alphabet");
    idx_ = std::make_unique<Index>(std::move(loaded.index));
    snap_info_ = loaded.info;
    last_seq_ = loaded.info.last_seq;
    const auto s = idx_->stats();
    log_info("loaded snapshot " + snap + ": " + std::to_string(s.global_queries) +
             " global queries, " + std::to_string(s.user_entries) + " user entries, " +
             std::to_string(s.words) + " words, seq " + std::to_string(last_seq_));
  } else {
    idx_ = std::make_unique<Index>();
  }

  WalReader reader(cfg_.data_dir, fp, last_seq_);
  const auto res = reader.read_more([&](const WalRecord& r) { apply_record(*idx_, r); });
  recovered_records_ = res.delivered;
  if (res.delivered > 0)
    log_info("replayed " + std::to_string(res.delivered) + " WAL records");
  if (res.torn_tail) {
    log_warn("WAL " + res.torn_path + " ends with an incomplete record; discarding " +
             "the tail after byte " + std::to_string(res.torn_offset));
    if (res.torn_offset == 0)
      fs::remove(res.torn_path, ec);
    else
      fs::resize_file(res.torn_path, res.torn_offset, ec);
  }
  last_seq_ = reader.last_seq();
  enable_hot_prefixes(*idx_);

  wal_ = std::make_unique<WalWriter>(cfg_.data_dir, fp, cfg_.fsync);
  wal_->open_segment(last_seq_ + 1);
}

void Engine::enable_hot_prefixes(Index& idx) const {
  if (cfg_.hot_prefix_letters == 0) return;
  const auto t0 = Clock::now();
  idx.enable_hot_prefixes({cfg_.hot_prefix_letters, cfg_.hot_prefix_top});
  const auto s = idx.stats();
  log_info("hot-prefix table: " + std::to_string(s.hot_prefixes) + " prefixes, " +
           std::to_string(s.hot_entries) + " entries, " +
           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()) +
           " ms");
}

void Engine::set_spell_fixer(std::shared_ptr<const SpellFixer> fixer) {
  std::lock_guard<std::mutex> lock(fixer_mu_);
  fixer_ = std::move(fixer);
}

bool Engine::has_spell_fixer() const {
  std::lock_guard<std::mutex> lock(fixer_mu_);
  return fixer_ != nullptr;
}

void Engine::start_background() {
  if (sync_thread_.joinable() || snapshot_thread_.joinable()) return;
  stop_ = false;

  sync_thread_ = std::thread([this] {
    if (cfg_.fsync != FsyncPolicy::Interval) return;
    const auto period = std::chrono::milliseconds(std::max(1u, cfg_.fsync_interval_ms));
    std::unique_lock<std::mutex> lock(bg_mu_);
    while (!stop_) {
      bg_cv_.wait_for(lock, period, [this] { return stop_.load(); });
      lock.unlock();
      try {
        wal_->sync_if_dirty();
      } catch (const std::exception& e) {
        log_error(std::string("WAL fsync failed: ") + e.what());
      }
      lock.lock();
    }
  });

  snapshot_thread_ = std::thread([this] {
    if (cfg_.snapshot_interval_s == 0 && cfg_.snapshot_wal_bytes == 0) return;
    auto last = Clock::now();
    std::unique_lock<std::mutex> lock(bg_mu_);
    while (!stop_) {
      bg_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_.load(); });
      if (stop_) break;
      lock.unlock();

      std::uint64_t seq = 0;
      {
        std::shared_lock<RWLock> rl(mu_);
        seq = last_seq_;
      }
      std::uint64_t snap_seq = 0;
      {
        std::lock_guard<std::mutex> il(info_mu_);
        snap_seq = snap_info_.last_seq;
      }
      const bool changed = seq != snap_seq;
      const bool due_time = cfg_.snapshot_interval_s != 0 && changed &&
                            Clock::now() - last >= std::chrono::seconds(cfg_.snapshot_interval_s);
      const bool due_size = cfg_.snapshot_wal_bytes != 0 && changed &&
                            wal_->total_bytes() >= cfg_.snapshot_wal_bytes;
      if (due_time || due_size) {
        const auto r = snapshot();
        if (r.ok())
          log_info("automatic snapshot at seq " + std::to_string(r.last_seq) + " (" +
                   std::to_string(r.file_bytes) + " bytes, " + std::to_string(r.duration_ms) + " ms)");
        else if (r.status != Status::Busy)
          log_error("automatic snapshot failed: " + r.message);
        last = Clock::now();
      }
      lock.lock();
    }
  });
}

void Engine::stop_background() {
  {
    std::lock_guard<std::mutex> lock(bg_mu_);
    stop_ = true;
  }
  bg_cv_.notify_all();
  if (sync_thread_.joinable()) sync_thread_.join();
  if (snapshot_thread_.joinable()) snapshot_thread_.join();
}

// ---------------------------------------------------------------- reads

std::string Engine::render_query(const Index& idx, QueryId id) const {
  std::size_t n = 0;
  const std::uint32_t* w = idx.query_words(id, n);
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    if (i) out.push_back(' ');
    out += alphabet_.decode_word(idx.word(w[i]));
  }
  return out;
}

CompleteResult Engine::complete(const CompleteRequest& req) const {
  const auto t0 = Clock::now();
  ++completes_;
  CompleteResult res;

  // Only the end of an over-long input is used (see TextProcessor::parse_typed).
  std::vector<std::string> words;
  std::string partial, detail;
  bool input_cut = false;
  const ParseError pe = text_.parse_typed(req.query, words, partial, input_cut, &detail);
  if (pe != ParseError::None) {
    res.status = Status::BadInput;
    res.message = std::string(to_string(pe)) + (detail.empty() ? "" : ": " + detail);
    return res;
  }
  res.input_truncated = input_cut;

  const std::size_t tail = req.tail;
  const std::size_t min_tail = tail == 0 ? 0 : std::clamp<std::size_t>(req.min_tail, 1, tail);

  if (req.correct) {
    std::shared_ptr<const SpellFixer> fixer;
    {
      std::lock_guard<std::mutex> lock(fixer_mu_);
      fixer = fixer_;
    }
    if (!fixer) {
      res.status = Status::Unavailable;
      res.message = "spell correction is not configured (start the server with --jamspell-model)";
      return res;
    }
    // Only complete words are corrected: the word being typed is unfinished. With the tail
    // fallback on, only the words a lookup can use are corrected: the last `tail` words of
    // the input, the word being typed among them. Earlier words are left as typed.
    const std::size_t room = tail == 0 ? words.size() : (partial.empty() ? tail : tail - 1);
    const std::size_t first = words.size() > room ? words.size() - room : 0;
    if (first < words.size()) {
      const std::vector<std::string> window(words.begin() + static_cast<std::ptrdiff_t>(first),
                                            words.end());
      try {
        const std::string fixed = fixer->fix(text_.render(window));
        std::vector<std::string> fixed_words;
        std::string fixed_partial;
        // A correction that leaves the alphabet or yields nothing is ignored.
        if (text_.parse_prefix(fixed + " ", fixed_words, fixed_partial) == ParseError::None &&
            fixed_partial.empty() && !fixed_words.empty() && fixed_words != window) {
          words.erase(words.begin() + static_cast<std::ptrdiff_t>(first), words.end());
          words.insert(words.end(), fixed_words.begin(), fixed_words.end());
          std::string shown = text_.render(words);
          if (!partial.empty()) shown += " " + alphabet_.decode_word(partial);
          res.corrected = std::move(shown);
        }
      } catch (const std::exception& e) {
        log_warn(std::string("spell correction failed: ") + e.what());
      }
    }
  }

  std::shared_lock<RWLock> lock(mu_);
  const Index& idx = *idx_;

  std::vector<std::optional<WordId>> word_ids;
  word_ids.reserve(words.size());
  for (const auto& w : words) word_ids.push_back(idx.find_word(w));

  // The lookups, most specific first: the whole input, then its last `tail` words, one word
  // fewer each time, down to `min_tail`. The word being typed counts as the last word. A
  // tail as long as the input would repeat the first lookup and is left out.
  struct Tier {
    std::size_t skip;     // words of the input (the typed one last) left out at the front
    std::size_t matched;  // words used
  };
  const std::size_t total = words.size() + (partial.empty() ? 0 : 1);
  std::vector<Tier> tiers{{0, total}};
  if (tail > 0 && total > 1) {
    for (std::size_t k = std::min(tail, total - 1); k >= min_tail; --k) {
      tiers.push_back({total - k, k});
      if (k == 1) break;
    }
  }

  const auto cache_key = [&](const Tier& t) {
    std::string key;
    for (std::size_t i = t.skip; i < words.size(); ++i) {
      key += words[i];
      key.push_back('\0');
    }
    key.push_back('\1');
    key += partial;
    key.push_back('\0');
    key += std::to_string(req.n);
    key.push_back('\2');
    key.push_back(static_cast<char>('0' + static_cast<int>(req.rank)));
    return key;
  };

  // One scan budget for the whole request, however many lookups it takes.
  std::size_t global_budget = cfg_.scan.max_visited;
  std::size_t user_budget = cfg_.scan.max_user_visited;
  bool global_ran = false, user_ran = false;  // a scan has used the budget (the first always runs)
  bool truncated = false;
  std::unordered_set<QueryId> seen_global, seen_user;
  std::vector<WordId> ids;

  for (const Tier& t : tiers) {
    const bool need_global = req.n > 0 && res.global.size() < req.n;
    const bool need_user = req.user && req.m > 0 && res.user.size() < req.m;
    if (!need_global && !need_user) break;

    // A lookup with a word the index has never seen cannot match anything.
    ids.clear();
    bool known = true;
    for (std::size_t i = t.skip; i < words.size(); ++i) {
      if (!word_ids[i]) {
        known = false;
        break;
      }
      ids.push_back(*word_ids[i]);
    }
    if (!known) continue;

    if (need_global) {
      std::string key = cache_key(t);
      std::vector<Suggestion> hits;
      bool hits_truncated = false;
      ResultCache::Entry entry;
      if (cache_->get(key, entry)) {
        hits = std::move(entry.hits);
        hits_truncated = entry.truncated;
      } else if (global_budget == 0 && global_ran) {
        truncated = true;  // the earlier lookups used it all
      } else {
        const bool whole_budget = global_budget == cfg_.scan.max_visited;
        Index::ScanLimits lim = cfg_.scan;
        lim.max_visited = global_budget;
        Index::ScanStats st;
        const bool avg = req.rank == Index::GlobalRank::AverageWordFrequency;
        for (const auto& h : idx.top_global(ids, partial, req.n, lim, st, req.rank)) {
          Suggestion sg{h.id, render_query(idx, h.id), h.freq, 0, std::nullopt, 0};
          if (avg) sg.score = h.average();
          hits.push_back(std::move(sg));
        }
        global_ran = true;
        global_budget -= std::min(global_budget, st.scanned);
        if (st.hot) ++hot_lookups_;
        hits_truncated = st.truncated;
        // A lookup cut short by a budget that earlier lookups had already eaten into must not
        // stand in for the same lookup with a whole budget.
        if (whole_budget || !st.truncated)
          cache_->put(std::move(key), ResultCache::Entry{hits, st.truncated});
      }
      truncated = truncated || hits_truncated;
      for (auto& h : hits) {
        if (res.global.size() >= req.n) break;
        if (!seen_global.insert(h.id).second) continue;  // already listed by a longer lookup
        h.matched_words = static_cast<std::uint32_t>(t.matched);
        res.global.push_back(std::move(h));
      }
    }

    if (need_user) {
      if (user_budget == 0 && user_ran) {
        truncated = true;
      } else {
        Index::ScanLimits lim = cfg_.scan;
        lim.max_user_visited = user_budget;
        Index::ScanStats st;
        for (const auto& h : idx.top_user(*req.user, ids, partial, req.m, lim, st)) {
          if (res.user.size() >= req.m) break;
          if (!seen_user.insert(h.id).second) continue;
          res.user.push_back({h.id, render_query(idx, h.id), h.freq, h.last_used, std::nullopt,
                              static_cast<std::uint32_t>(t.matched)});
        }
        user_ran = true;
        user_budget -= std::min(user_budget, st.scanned);
        truncated = truncated || st.truncated;
      }
    }
  }

  res.truncated = truncated;
  res.took_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
  return res;
}

// ---------------------------------------------------------------- writes

Result Engine::write_record(WalRecord& rec, AddResult* add) {
  std::unique_lock<RWLock> lock(mu_);

  // Cheap pre-check so that no-op removals do not bloat the log.
  if ((rec.op == WalOp::RemoveGlobal || rec.op == WalOp::RemoveUser) && !idx_->query_alive(rec.id))
    return {Status::NotFound, "no query with id " + std::to_string(rec.id)};

  rec.seq = last_seq_ + 1;
  try {
    wal_->append(rec);  // write-ahead: if this fails the operation did not happen
  } catch (const std::exception& e) {
    return {Status::Error, std::string("write-ahead log failure: ") + e.what()};
  }
  last_seq_ = rec.seq;

  Applied a;
  try {
    a = apply_record(*idx_, rec);
  } catch (const std::exception& e) {
    log_error(std::string("applying WAL record ") + std::to_string(rec.seq) + " failed: " + e.what());
    return {Status::Error, e.what()};
  }
  ++writes_;

  if (rec.op == WalOp::RemoveGlobal || rec.op == WalOp::RemoveUser) {
    cache_->clear();  // a removed query must not be served from the cache
    if (!a.ok) {
      return {Status::NotFound, rec.op == WalOp::RemoveGlobal
                                    ? "query " + std::to_string(rec.id) + " is not in the global pool"
                                    : "query " + std::to_string(rec.id) + " is not in the user's history"};
    }
  }
  if (add != nullptr) {
    add->id = a.out.id;
    add->freq = a.out.freq;
    add->created = a.out.created;
  }

  if (wal_->segment_bytes() >= cfg_.wal_segment_bytes) {
    try {
      wal_->open_segment(last_seq_ + 1);
    } catch (const std::exception& e) {
      log_warn(std::string("WAL rotation failed: ") + e.what());
    }
  }
  return {};
}

AddResult Engine::add_global(std::string_view query, std::uint32_t count) {
  AddResult r;
  if (count == 0) {
    r.status = Status::BadInput;
    r.message = "count must be positive";
    return r;
  }
  WalRecord rec;
  rec.op = WalOp::AddGlobal;
  rec.count = count;
  std::string detail;
  const ParseError pe = text_.parse_query(query, rec.words, &detail);
  if (pe != ParseError::None) {
    r.status = Status::BadInput;
    r.message = std::string(to_string(pe)) + (detail.empty() ? "" : ": " + detail);
    return r;
  }
  r.text = text_.render(rec.words);
  static_cast<Result&>(r) = write_record(rec, &r);
  return r;
}

AddResult Engine::add_user(std::uint32_t user, std::string_view query, std::uint32_t count,
                           bool also_global) {
  AddResult r;
  if (count == 0) {
    r.status = Status::BadInput;
    r.message = "count must be positive";
    return r;
  }
  WalRecord rec;
  rec.op = WalOp::AddUser;
  rec.user = user;
  rec.count = count;
  rec.timestamp = now_unix();
  rec.also_global = also_global;
  std::string detail;
  const ParseError pe = text_.parse_query(query, rec.words, &detail);
  if (pe != ParseError::None) {
    r.status = Status::BadInput;
    r.message = std::string(to_string(pe)) + (detail.empty() ? "" : ": " + detail);
    return r;
  }
  r.text = text_.render(rec.words);
  static_cast<Result&>(r) = write_record(rec, &r);
  return r;
}

Result Engine::remove_global(QueryId id) {
  WalRecord rec;
  rec.op = WalOp::RemoveGlobal;
  rec.id = id;
  return write_record(rec, nullptr);
}

Result Engine::remove_user(std::uint32_t user, QueryId id) {
  WalRecord rec;
  rec.op = WalOp::RemoveUser;
  rec.user = user;
  rec.id = id;
  return write_record(rec, nullptr);
}

// ---------------------------------------------------------------- maintenance

MaintenanceResult Engine::snapshot() { return run_maintenance(false); }
MaintenanceResult Engine::optimize() { return run_maintenance(true); }

MaintenanceResult Engine::run_maintenance(bool compact) {
  MaintenanceResult res;
  std::unique_lock<std::mutex> maint(maint_mu_, std::try_to_lock);
  if (!maint.owns_lock()) {
    res.status = Status::Busy;
    res.message = "another snapshot or optimize is already running";
    return res;
  }
  maint_running_ = true;
  struct Reset {
    std::atomic<bool>& flag;
    ~Reset() { flag = false; }
  } reset{maint_running_};

  const auto t0 = Clock::now();
  const std::uint64_t fp = alphabet_.fingerprint();
  const std::string tmp = path(compact ? "snapshot.opt.tmp" : "snapshot.tmp");
  const std::string final_path = path("snapshot.bin");
  std::error_code ec;

  try {
    std::uint64_t seq = 0;
    SnapshotInfo info;

    if (cfg_.snapshot_mode == SnapshotMode::Inline) {
      // Stop-the-world: simple and portable, but blocks all requests meanwhile.
      std::unique_lock<RWLock> lock(mu_);
      seq = last_seq_;
      res.dead_queries_before = idx_->stats().dead_queries;
      wal_->open_segment(seq + 1);
      if (compact) {
        Index compacted = idx_->compacted();
        info = write_snapshot(tmp, compacted, seq, fp);
        fs::rename(tmp, final_path);
        fsync_directory(cfg_.data_dir);
        enable_hot_prefixes(compacted);
        idx_ = std::make_unique<Index>(std::move(compacted));
        cache_->clear();
      } else {
        info = write_snapshot(tmp, *idx_, seq, fp);
        fs::rename(tmp, final_path);
        fsync_directory(cfg_.data_dir);
      }
    } else {
      // The child sees the index exactly as it is at fork time (copy on write),
      // while the parent keeps serving requests.
      pid_t pid = -1;
      {
        std::unique_lock<RWLock> lock(mu_);
        seq = last_seq_;
        res.dead_queries_before = idx_->stats().dead_queries;
        wal_->open_segment(seq + 1);  // records after `seq` go to a new segment
        pid = ::fork();
        if (pid < 0) throw_errno("fork");
        if (pid == 0) {
          int code = 2;
          try {
            close_inherited_fds();
            if (compact) {
              const Index compacted = idx_->compacted();
              write_snapshot(tmp, compacted, seq, fp);
            } else {
              write_snapshot(tmp, *idx_, seq, fp);
            }
            code = 0;
          } catch (...) {
          }
          ::_exit(code);  // no destructors, no atexit handlers
        }
      }

      int status = 0;
      while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw_errno("waitpid");
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("snapshot child process failed");

      if (!compact) {
        info.alphabet_fingerprint = fp;
        info.last_seq = seq;
        info.created_unix = now_unix();
        info.file_bytes = fs::file_size(tmp);
        fs::rename(tmp, final_path);
        fsync_directory(cfg_.data_dir);
      } else {
        // Load the compacted copy, bring it up to date with the writes that
        // happened meanwhile, then swap it in.
        LoadedSnapshot loaded = read_snapshot(tmp);
        if (loaded.info.last_seq != seq) throw std::runtime_error("optimized snapshot has wrong sequence");
        auto fresh = std::make_unique<Index>(std::move(loaded.index));
        info = loaded.info;
        enable_hot_prefixes(*fresh);  // before the catch-up: the writes below keep it current

        WalReader tail(cfg_.data_dir, fp, seq);
        const auto apply = [&](const WalRecord& r) { apply_record(*fresh, r); };
        for (int pass = 0; pass < 100; ++pass) {
          if (tail.read_more(apply).delivered < 1000) break;  // close enough: finish under the lock
        }
        {
          std::unique_lock<RWLock> lock(mu_);
          const auto last = tail.read_more(apply);  // nothing can be appended now
          if (last.torn_tail || tail.last_seq() != last_seq_)
            throw std::runtime_error("optimize catch-up did not reach the end of the log");
          idx_ = std::move(fresh);
          cache_->clear();
        }
        fs::rename(tmp, final_path);
        fsync_directory(cfg_.data_dir);
      }
    }

    wal_->prune_covered_by(seq);
    {
      std::lock_guard<std::mutex> il(info_mu_);
      snap_info_ = info;
    }
    res.last_seq = seq;
    res.file_bytes = info.file_bytes;
  } catch (const std::exception& e) {
    fs::remove(tmp, ec);
    res.status = Status::Error;
    res.message = e.what();
    log_error(std::string(compact ? "optimize" : "snapshot") + " failed: " + e.what());
  }
  res.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
  return res;
}

// ---------------------------------------------------------------- stats

EngineStats Engine::stats() const {
  EngineStats s;
  {
    std::shared_lock<RWLock> lock(mu_);
    s.index = idx_->stats();
    s.last_seq = last_seq_;
  }
  s.wal_bytes = wal_->total_bytes();
  {
    std::lock_guard<std::mutex> il(info_mu_);
    s.snapshot_seq = snap_info_.last_seq;
    s.snapshot_bytes = snap_info_.file_bytes;
    s.snapshot_unix = snap_info_.created_unix;
    s.recovered_records = recovered_records_;
  }
  s.cache_hits = cache_->hits();
  s.cache_misses = cache_->misses();
  s.completes = completes_.load();
  s.hot_lookups = hot_lookups_.load();
  s.writes = writes_.load();
  s.maintenance_running = maint_running_.load();
  s.uptime_s = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count());
  return s;
}

}  // namespace qac
