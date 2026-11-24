// Train mode: builds a data directory from a text file, one query per line.
//
// It bypasses the WAL on purpose: the index is built in memory and written as one
// snapshot, which is far cheaper than logging hundreds of millions of records.
// The resulting directory is what `Engine` opens (snapshot, empty WAL).
//
// Line format: `query` or `query<TAB>count`. The suffix after the last tab counts
// only if it consists of digits (disable with counts=false). Blank lines are
// skipped; malformed queries (characters outside the alphabet, too long, ...) are
// rejected and reported.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "qac/alphabet.hpp"
#include "qac/common.hpp"
#include "qac/index.hpp"

namespace qac {

struct TrainOptions {
  std::string input;     // file path; "-" reads standard input
  std::string data_dir;  // created if missing
  Limits limits;
  bool counts = true;      // parse the optional tab-separated count
  bool overwrite = false;  // replace existing snapshot / WAL in data_dir
  std::size_t error_samples = 10;
  // Called every `progress_every` lines (0 disables) with the number of lines read.
  std::uint64_t progress_every = 1000000;
  std::function<void(std::uint64_t lines)> progress;
};

struct TrainRejection {
  std::uint64_t line = 0;
  std::string reason;
  std::string text;  // truncated
};

struct TrainReport {
  std::uint64_t lines = 0;
  std::uint64_t blank = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t total_count = 0;  // sum of the counts of accepted lines
  std::vector<TrainRejection> samples;
  Index::Stats index;
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t duration_ms = 0;
};

// Throws std::runtime_error on I/O errors, an unreadable input, a data directory
// that is in use or already holds data (without `overwrite`).
TrainReport train(const Alphabet& alphabet, const TrainOptions& options);

}  // namespace qac
