// Write-ahead log.
//
// Every index mutation is appended here before it is applied. After a crash the
// last snapshot is loaded and the log is replayed on top of it.
//
// Layout: a directory of segments `wal-<first seq, 20 digits>.log`.
//   segment header: [magic u64][alphabet fingerprint u64][first seq u64][crc32c u32]
//   record:         [payload length u32][crc32c of payload u32][payload]
//   payload:        [seq u64][op u8][op specific fields]
// Records hold alphabet-encoded words, so a log is only valid for the alphabet
// whose fingerprint is in its header.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "qac/common.hpp"
#include "qac/io.hpp"

namespace qac {

enum class WalOp : std::uint8_t {
  AddGlobal = 1,
  AddUser = 2,
  RemoveGlobal = 3,
  RemoveUser = 4,
};

struct WalRecord {
  std::uint64_t seq = 0;
  WalOp op = WalOp::AddGlobal;
  std::uint32_t user = 0;
  std::uint32_t count = 0;
  std::uint32_t timestamp = 0;
  bool also_global = false;
  QueryId id = kNoQuery;
  std::vector<std::string> words;  // alphabet-encoded
};

void encode_wal_payload(const WalRecord& rec, std::string& out);
bool decode_wal_payload(std::string_view payload, WalRecord& rec);

enum class FsyncPolicy { Always, Interval, Never };
bool parse_fsync_policy(const std::string& s, FsyncPolicy& out);

struct WalSegmentFile {
  std::uint64_t first_seq = 0;
  std::string path;
};
std::vector<WalSegmentFile> list_wal_segments(const std::string& dir);

class WalWriter {
 public:
  WalWriter(std::string dir, std::uint64_t alphabet_fingerprint, FsyncPolicy policy);
  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Closes the current segment (if any) and starts `wal-<first_seq>.log`. An
  // existing file of that name is empty by construction and is replaced.
  void open_segment(std::uint64_t first_seq);

  // Appends one record with a single write(2). With FsyncPolicy::Always it is
  // durable on return. Throws std::runtime_error on I/O errors.
  void append(const WalRecord& rec);

  // fdatasync if anything was written since the last sync. Thread-safe.
  void sync_if_dirty();

  // Deletes closed segments whose records are all <= `seq`.
  void prune_covered_by(std::uint64_t seq);

  std::uint64_t segment_bytes() const;  // size of the current segment
  std::uint64_t total_bytes() const;    // all segments on disk

 private:
  void write_header_locked(std::uint64_t first_seq);

  std::string dir_;
  std::uint64_t fingerprint_;
  FsyncPolicy policy_;
  mutable std::mutex mu_;
  Fd fd_;
  std::uint64_t current_first_seq_ = 0;
  std::uint64_t current_bytes_ = 0;
  bool dirty_ = false;
  std::string scratch_;
};

// Reads records from a directory of segments, in order.
class WalReader {
 public:
  // Records with seq <= after_seq are skipped. The first delivered record must be
  // after_seq + 1 and every following one must be consecutive, otherwise
  // read_more() throws (a gap means a segment is missing).
  WalReader(std::string dir, std::uint64_t alphabet_fingerprint, std::uint64_t after_seq);

  struct Result {
    std::uint64_t delivered = 0;
    bool torn_tail = false;           // stopped at an incomplete / corrupt record
    std::string torn_path;            // segment containing it
    std::uint64_t torn_offset = 0;    // bytes of that segment that are valid
  };

  // Delivers all complete records available now and remembers its position, so
  // a later call continues where this one stopped (the log may have grown). A
  // damaged record in any segment but the last one throws.
  Result read_more(const std::function<void(const WalRecord&)>& fn);

  std::uint64_t last_seq() const noexcept { return last_seq_; }

 private:
  std::string dir_;
  std::uint64_t fingerprint_;
  std::uint64_t last_seq_;
  bool started_ = false;
  std::uint64_t cur_first_seq_ = 0;  // segment being read
  std::uint64_t cur_offset_ = 0;
};

}  // namespace qac
