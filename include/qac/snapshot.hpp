// Snapshot files: a cereal-serialized Index plus a small header and a CRC-32C trailer.
//
//   [magic u64][format u32][alphabet fingerprint u64][last WAL seq u64][created unix u64]
//   [Index payload ...][crc32c u32 of everything before it]
#pragma once

#include <cstdint>
#include <string>

#include "qac/index.hpp"

namespace qac {

struct SnapshotInfo {
  std::uint64_t alphabet_fingerprint = 0;
  std::uint64_t last_seq = 0;  // WAL records up to and including this seq are contained
  std::uint64_t created_unix = 0;
  std::uint64_t file_bytes = 0;
};

// Writes to `path` (created/truncated) and fsyncs it. The caller publishes it with
// an atomic rename. Throws std::runtime_error on failure.
SnapshotInfo write_snapshot(const std::string& path, const Index& index, std::uint64_t last_seq,
                            std::uint64_t alphabet_fingerprint);

struct LoadedSnapshot {
  Index index;
  SnapshotInfo info;
};

// Throws std::runtime_error when the file is missing, truncated, corrupt (CRC) or
// written by an incompatible version.
LoadedSnapshot read_snapshot(const std::string& path);

}  // namespace qac
