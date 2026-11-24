// Engine: the thread-safe facade the server uses.
//
// Owns the Index, the write-ahead log and the snapshot machinery:
//   * reads (complete) take a shared lock, writes an exclusive one;
//   * every write is appended to the WAL first, then applied;
//   * snapshot() persists the index and lets the WAL be truncated;
//   * optimize() additionally rebuilds the index without dead queries/words and
//     swaps it in.
// With SnapshotMode::Fork the heavy lifting runs in a forked child (copy on
// write), so neither readers nor writers are blocked while it works.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "qac/alphabet.hpp"
#include "qac/common.hpp"
#include "qac/dir_lock.hpp"
#include "qac/index.hpp"
#include "qac/spell_fixer.hpp"
#include "qac/io.hpp"
#include "qac/rwlock.hpp"
#include "qac/snapshot.hpp"
#include "qac/text.hpp"
#include "qac/wal.hpp"

namespace qac {

enum class Status { Ok, BadInput, NotFound, Busy, Unavailable, Error };
const char* to_string(Status s);

struct Result {
  Status status = Status::Ok;
  std::string message;
  bool ok() const noexcept { return status == Status::Ok; }
};

enum class SnapshotMode {
  Fork,    // child process serializes a copy-on-write view; nothing blocks
  Inline,  // stop-the-world in-process (portable fallback)
};
bool parse_snapshot_mode(const std::string& s, SnapshotMode& out);

struct EngineConfig {
  std::string data_dir;
  Limits limits;
  Index::ScanLimits scan;

  FsyncPolicy fsync = FsyncPolicy::Interval;
  unsigned fsync_interval_ms = 100;
  std::uint64_t wal_segment_bytes = 256ull << 20;

  // Automatic snapshots (0 disables a trigger).
  unsigned snapshot_interval_s = 900;
  std::uint64_t snapshot_wal_bytes = 1ull << 30;
  SnapshotMode snapshot_mode = SnapshotMode::Fork;

  // Cache for global suggestions (staleness is bounded by the ttl; removals
  // clear it immediately). entries == 0 disables it.
  std::size_t cache_entries = 10000;
  unsigned cache_ttl_ms = 2000;
};

struct Suggestion {
  QueryId id = kNoQuery;
  std::string text;
  std::uint32_t freq = 0;
  std::uint32_t last_used = 0;  // user history only
  // Mean frequency of the words of the query; set only when the request asked for
  // GlobalRank::AverageWordFrequency (global suggestions).
  std::optional<double> score;
};

struct CompleteRequest {
  std::string query;
  std::size_t n = 10;                 // top-N from the global pool
  std::optional<std::uint32_t> user;  // user whose history is searched too
  std::size_t m = 5;                  // top-M from that user's history
  bool correct = false;               // spell-correct the complete words
  // Ranking of the global candidates: by query frequency (default), or by the mean
  // frequency of the words each candidate consists of. Does not affect the user list.
  Index::GlobalRank rank = Index::GlobalRank::Frequency;
};

struct CompleteResult : Result {
  std::optional<std::string> corrected;  // set when correction changed the input
  std::vector<Suggestion> global;
  std::vector<Suggestion> user;
  bool truncated = false;  // a scan budget ran out: results may be approximate
  std::uint64_t took_us = 0;
};

struct AddResult : Result {
  QueryId id = kNoQuery;
  std::uint32_t freq = 0;
  bool created = false;
  std::string text;  // normalized text of the stored query
};

struct MaintenanceResult : Result {
  std::uint64_t last_seq = 0;
  std::uint64_t file_bytes = 0;
  std::uint64_t duration_ms = 0;
  std::uint64_t dead_queries_before = 0;
};

struct EngineStats {
  Index::Stats index;
  std::uint64_t last_seq = 0;
  std::uint64_t wal_bytes = 0;
  std::uint64_t snapshot_seq = 0;
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t snapshot_unix = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t completes = 0;
  std::uint64_t writes = 0;
  std::uint64_t recovered_records = 0;
  bool maintenance_running = false;
  std::uint64_t uptime_s = 0;
};

class ResultCache;

class Engine {
 public:
  // Opens (or creates) the data directory: loads the snapshot, replays the WAL,
  // and takes an exclusive lock on the directory (one owner process).
  // Throws std::runtime_error on failure. `alphabet` must outlive the engine.
  Engine(const Alphabet& alphabet, EngineConfig config);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void set_spell_fixer(std::shared_ptr<const SpellFixer> fixer);
  bool has_spell_fixer() const;

  // Starts the WAL fsync and automatic-snapshot threads; stop_background() joins them.
  void start_background();
  void stop_background();

  // ---- reads ----
  CompleteResult complete(const CompleteRequest& req) const;

  // ---- writes (all durable through the WAL) ----
  AddResult add_global(std::string_view query, std::uint32_t count = 1);
  AddResult add_user(std::uint32_t user, std::string_view query, std::uint32_t count = 1,
                     bool also_global = false);
  Result remove_global(QueryId id);
  Result remove_user(std::uint32_t user, QueryId id);

  // ---- maintenance (one at a time; Busy otherwise) ----
  MaintenanceResult snapshot();
  MaintenanceResult optimize();

  EngineStats stats() const;
  const TextProcessor& text() const noexcept { return text_; }
  const EngineConfig& config() const noexcept { return cfg_; }

 private:
  MaintenanceResult run_maintenance(bool compact);
  // Logs `rec` and applies it under the exclusive lock; fills `add` for add operations.
  Result write_record(WalRecord& rec, AddResult* add);
  std::string render_query(const Index& idx, QueryId id) const;
  std::string path(const char* name) const;
  void recover();
  void open_directory();  // lock, load snapshot, replay WAL, open the log

  const Alphabet& alphabet_;
  EngineConfig cfg_;
  TextProcessor text_;
  std::chrono::steady_clock::time_point started_;

  mutable RWLock mu_;
  std::unique_ptr<Index> idx_;
  std::uint64_t last_seq_ = 0;
  std::unique_ptr<WalWriter> wal_;

  std::unique_ptr<DirLock> dir_lock_;

  std::mutex maint_mu_;
  std::atomic<bool> maint_running_{false};

  mutable std::mutex info_mu_;
  SnapshotInfo snap_info_;
  std::uint64_t recovered_records_ = 0;

  mutable std::mutex fixer_mu_;
  std::shared_ptr<const SpellFixer> fixer_;

  std::unique_ptr<ResultCache> cache_;
  mutable std::atomic<std::uint64_t> completes_{0};
  std::atomic<std::uint64_t> writes_{0};

  std::atomic<bool> stop_{false};
  std::mutex bg_mu_;
  std::condition_variable bg_cv_;
  std::thread sync_thread_;
  std::thread snapshot_thread_;
};

}  // namespace qac
