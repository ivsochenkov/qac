#include "qac/train.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "qac/dir_lock.hpp"
#include "qac/io.hpp"
#include "qac/log.hpp"
#include "qac/snapshot.hpp"
#include "qac/text.hpp"
#include "qac/wal.hpp"

namespace qac {

namespace fs = std::filesystem;

namespace {

// Splits an optional "\t<digits>" suffix off `line`. Returns false when the count
// does not fit into 32 bits or is zero (the line is then rejected).
bool split_count(std::string_view& line, std::uint32_t& count, bool& valid) {
  valid = true;
  count = 1;
  const auto tab = line.rfind('\t');
  if (tab == std::string_view::npos) return false;
  std::string_view tail = line.substr(tab + 1);
  while (!tail.empty() && tail.front() == ' ') tail.remove_prefix(1);
  while (!tail.empty() && tail.back() == ' ') tail.remove_suffix(1);
  if (tail.empty() || !std::all_of(tail.begin(), tail.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return false;  // not a count: the tab is just whitespace inside the query
  std::uint64_t v = 0;
  for (char c : tail) {
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
    if (v > std::numeric_limits<std::uint32_t>::max()) {
      valid = false;
      break;
    }
  }
  if (v == 0) valid = false;
  count = static_cast<std::uint32_t>(v);
  line = line.substr(0, tab);
  return true;
}

}  // namespace

TrainReport train(const Alphabet& alphabet, const TrainOptions& opt) {
  const auto started = std::chrono::steady_clock::now();
  if (opt.data_dir.empty()) throw std::runtime_error("data directory is not set");

  std::ifstream file;
  std::istream* in = &std::cin;
  if (opt.input != "-") {
    file.open(opt.input, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input file " + opt.input);
    in = &file;
  }

  DirLock lock(opt.data_dir);
  const std::string snap = (fs::path(opt.data_dir) / "snapshot.bin").string();
  const auto segments = list_wal_segments(opt.data_dir);
  if (fs::exists(snap) || !segments.empty()) {
    if (!opt.overwrite)
      throw std::runtime_error("data directory " + opt.data_dir +
                               " already contains data; pass --overwrite to replace it");
    std::error_code ec;
    fs::remove(snap, ec);
    for (const auto& s : segments) fs::remove(s.path, ec);
  }

  const TextProcessor text(alphabet, opt.limits);
  Index index;
  TrainReport rep;

  std::string line;
  std::vector<std::string> words;
  std::vector<std::string_view> views;
  std::string detail;
  while (std::getline(*in, line)) {
    ++rep.lines;
    if (rep.lines == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // BOM
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (opt.progress && opt.progress_every && rep.lines % opt.progress_every == 0)
      opt.progress(rep.lines);

    std::string_view q = line;
    std::uint32_t count = 1;
    bool valid = true;
    if (opt.counts) split_count(q, count, valid);

    auto reject = [&](const std::string& reason) {
      ++rep.rejected;
      if (rep.samples.size() < opt.error_samples)
        rep.samples.push_back({rep.lines, reason, std::string(line.substr(0, 120))});
    };

    if (!valid) {
      reject("invalid count");
      continue;
    }
    detail.clear();
    const ParseError err = text.parse_query(q, words, &detail);
    if (err == ParseError::Empty) {
      ++rep.blank;
      continue;
    }
    if (err != ParseError::None) {
      reject(detail.empty() ? to_string(err) : std::string(to_string(err)) + ": " + detail);
      continue;
    }
    views.assign(words.begin(), words.end());
    index.add_global(views, count);
    ++rep.accepted;
    rep.total_count += count;
  }
  if (in->bad()) throw std::runtime_error("read error on " + opt.input);

  rep.index = index.stats();

  const std::string tmp = (fs::path(opt.data_dir) / "snapshot.tmp").string();
  const SnapshotInfo info = write_snapshot(tmp, index, 0, alphabet.fingerprint());
  std::error_code ec;
  fs::rename(tmp, snap, ec);
  if (ec) throw std::runtime_error("cannot move the snapshot into place: " + ec.message());
  fsync_directory(opt.data_dir);
  rep.snapshot_bytes = info.file_bytes;
  rep.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
          .count());
  return rep;
}

}  // namespace qac
