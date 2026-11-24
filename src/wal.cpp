#include "qac/wal.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qac/crc32c.hpp"

namespace qac {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kSegmentMagic = 0x31304C4157434151ull;  // "QACWAL01"
constexpr std::size_t kHeaderBytes = 8 + 8 + 8 + 4;
constexpr std::uint32_t kMaxPayload = 1u << 20;  // far above any valid record

template <class T>
void put(std::string& out, T v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof v);
}

class Cursor {
 public:
  explicit Cursor(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}
  template <class T>
  bool get(T& v) {
    if (static_cast<std::size_t>(end_ - p_) < sizeof v) return false;
    std::memcpy(&v, p_, sizeof v);
    p_ += sizeof v;
    return true;
  }
  bool bytes(std::string& out, std::size_t n) {
    if (static_cast<std::size_t>(end_ - p_) < n) return false;
    out.assign(p_, n);
    p_ += n;
    return true;
  }
  bool done() const { return p_ == end_; }

 private:
  const char* p_;
  const char* end_;
};

std::string segment_name(std::uint64_t first_seq) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "wal-%020" PRIu64 ".log", first_seq);
  return buf;
}

void put_words(std::string& out, const std::vector<std::string>& words) {
  put<std::uint8_t>(out, static_cast<std::uint8_t>(words.size()));
  for (const auto& w : words) {
    put<std::uint8_t>(out, static_cast<std::uint8_t>(w.size()));
    out += w;
  }
}

bool get_words(Cursor& c, std::vector<std::string>& words) {
  std::uint8_t n = 0;
  if (!c.get(n) || n == 0) return false;
  words.assign(n, {});
  for (auto& w : words) {
    std::uint8_t len = 0;
    if (!c.get(len) || len == 0 || !c.bytes(w, len)) return false;
  }
  return true;
}

}  // namespace

void encode_wal_payload(const WalRecord& rec, std::string& out) {
  out.clear();
  put<std::uint64_t>(out, rec.seq);
  put<std::uint8_t>(out, static_cast<std::uint8_t>(rec.op));
  switch (rec.op) {
    case WalOp::AddGlobal:
      put<std::uint32_t>(out, rec.count);
      put_words(out, rec.words);
      break;
    case WalOp::AddUser:
      put<std::uint32_t>(out, rec.user);
      put<std::uint32_t>(out, rec.count);
      put<std::uint32_t>(out, rec.timestamp);
      put<std::uint8_t>(out, rec.also_global ? 1 : 0);
      put_words(out, rec.words);
      break;
    case WalOp::RemoveGlobal:
      put<std::uint32_t>(out, rec.id);
      break;
    case WalOp::RemoveUser:
      put<std::uint32_t>(out, rec.user);
      put<std::uint32_t>(out, rec.id);
      break;
  }
}

bool decode_wal_payload(std::string_view payload, WalRecord& rec) {
  Cursor c(payload);
  std::uint8_t op = 0;
  rec = WalRecord{};
  if (!c.get(rec.seq) || !c.get(op)) return false;
  rec.op = static_cast<WalOp>(op);
  switch (rec.op) {
    case WalOp::AddGlobal:
      if (!c.get(rec.count) || !get_words(c, rec.words)) return false;
      break;
    case WalOp::AddUser: {
      std::uint8_t also = 0;
      if (!c.get(rec.user) || !c.get(rec.count) || !c.get(rec.timestamp) || !c.get(also) ||
          !get_words(c, rec.words))
        return false;
      rec.also_global = also != 0;
      break;
    }
    case WalOp::RemoveGlobal:
      if (!c.get(rec.id)) return false;
      break;
    case WalOp::RemoveUser:
      if (!c.get(rec.user) || !c.get(rec.id)) return false;
      break;
    default:
      return false;
  }
  return c.done();
}

bool parse_fsync_policy(const std::string& s, FsyncPolicy& out) {
  if (s == "always") out = FsyncPolicy::Always;
  else if (s == "interval") out = FsyncPolicy::Interval;
  else if (s == "never") out = FsyncPolicy::Never;
  else return false;
  return true;
}

std::vector<WalSegmentFile> list_wal_segments(const std::string& dir) {
  std::vector<WalSegmentFile> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    std::uint64_t first = 0;
    char tail[8] = {};
    // wal-<20 digits>.log
    if (name.size() == 4 + 20 + 4 && name.compare(0, 4, "wal-") == 0 &&
        std::sscanf(name.c_str(), "wal-%20" SCNu64 "%7s", &first, tail) == 2 &&
        std::strcmp(tail, ".log") == 0)
      out.push_back({first, e.path().string()});
  }
  std::sort(out.begin(), out.end(),
            [](const WalSegmentFile& a, const WalSegmentFile& b) { return a.first_seq < b.first_seq; });
  return out;
}

// ---------------------------------------------------------------- WalWriter

WalWriter::WalWriter(std::string dir, std::uint64_t alphabet_fingerprint, FsyncPolicy policy)
    : dir_(std::move(dir)), fingerprint_(alphabet_fingerprint), policy_(policy) {}

WalWriter::~WalWriter() {
  try {
    sync_if_dirty();
  } catch (...) {
  }
}

void WalWriter::write_header_locked(std::uint64_t first_seq) {
  std::string h;
  put<std::uint64_t>(h, kSegmentMagic);
  put<std::uint64_t>(h, fingerprint_);
  put<std::uint64_t>(h, first_seq);
  put<std::uint32_t>(h, crc32c(h.data(), h.size()));
  write_all(fd_.get(), h.data(), h.size());
  current_bytes_ = h.size();
}

void WalWriter::open_segment(std::uint64_t first_seq) {
  std::lock_guard<std::mutex> lock(mu_);
  // Rotating an empty segment to the same name would only recreate it.
  if (fd_.valid() && current_first_seq_ == first_seq && current_bytes_ == kHeaderBytes) return;
  if (fd_.valid() && dirty_) {
    fsync_fd(fd_.get());
    dirty_ = false;
  }
  const std::string path = (fs::path(dir_) / segment_name(first_seq)).string();
  Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644));
  if (!fd.valid()) throw_errno("cannot create WAL segment " + path);
  fd_ = std::move(fd);
  current_first_seq_ = first_seq;
  write_header_locked(first_seq);
  // Make the file visible and its header durable, so a later crash never finds
  // records in a file whose name or header is lost.
  if (policy_ != FsyncPolicy::Never) {
    fsync_fd(fd_.get());
    fsync_directory(dir_);
  }
}

void WalWriter::append(const WalRecord& rec) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!fd_.valid()) throw std::runtime_error("WAL is not open");
  scratch_.clear();
  scratch_.resize(8);  // room for [length][crc]
  std::string payload;
  encode_wal_payload(rec, payload);
  const auto len = static_cast<std::uint32_t>(payload.size());
  const std::uint32_t crc = crc32c(payload.data(), payload.size());
  std::memcpy(scratch_.data(), &len, 4);
  std::memcpy(scratch_.data() + 4, &crc, 4);
  scratch_ += payload;
  write_all(fd_.get(), scratch_.data(), scratch_.size());  // one write: records are not interleaved
  current_bytes_ += scratch_.size();
  dirty_ = true;
  if (policy_ == FsyncPolicy::Always) {
    if (::fdatasync(fd_.get()) != 0) throw_errno("fdatasync");
    dirty_ = false;
  }
}

void WalWriter::sync_if_dirty() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!fd_.valid() || !dirty_) return;
  if (::fdatasync(fd_.get()) != 0) throw_errno("fdatasync");
  dirty_ = false;
}

void WalWriter::prune_covered_by(std::uint64_t seq) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto segs = list_wal_segments(dir_);
  // Segment i holds seqs [first_i, first_{i+1} - 1]; the newest segment is the open one.
  bool removed = false;
  for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
    if (segs[i].first_seq == current_first_seq_) continue;
    if (segs[i + 1].first_seq <= seq + 1) {
      std::error_code ec;
      fs::remove(segs[i].path, ec);
      removed = removed || !ec;
    }
  }
  if (removed && policy_ != FsyncPolicy::Never) fsync_directory(dir_);
}

std::uint64_t WalWriter::segment_bytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_bytes_;
}

std::uint64_t WalWriter::total_bytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::uint64_t total = 0;
  for (const auto& s : list_wal_segments(dir_)) {
    std::error_code ec;
    const auto sz = fs::file_size(s.path, ec);
    if (!ec) total += sz;
  }
  return total;
}

// ---------------------------------------------------------------- WalReader

WalReader::WalReader(std::string dir, std::uint64_t alphabet_fingerprint, std::uint64_t after_seq)
    : dir_(std::move(dir)), fingerprint_(alphabet_fingerprint), last_seq_(after_seq) {}

WalReader::Result WalReader::read_more(const std::function<void(const WalRecord&)>& fn) {
  Result result;
  for (;;) {
    const auto segs = list_wal_segments(dir_);
    if (segs.empty()) return result;

    if (!started_) {
      // The segment that holds last_seq_ + 1 is the last one starting at or before it.
      std::size_t pick = 0;
      for (std::size_t i = 0; i < segs.size(); ++i)
        if (segs[i].first_seq <= last_seq_ + 1) pick = i;
      cur_first_seq_ = segs[pick].first_seq;
      cur_offset_ = 0;
      started_ = true;
    }

    std::size_t idx = segs.size();
    for (std::size_t i = 0; i < segs.size(); ++i)
      if (segs[i].first_seq == cur_first_seq_) idx = i;
    if (idx == segs.size()) {
      // Our segment was pruned under us; this happens only if a snapshot covered
      // everything we still wanted. Resume at the oldest segment that remains.
      throw std::runtime_error("WAL segment " + segment_name(cur_first_seq_) + " disappeared");
    }
    const bool is_last = idx + 1 == segs.size();
    const std::string& path = segs[idx].path;

    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) throw_errno("cannot open WAL segment " + path);

    auto read_exact = [&](std::uint64_t off, void* dst, std::size_t n) -> bool {
      std::size_t got = 0;
      while (got < n) {
        const ssize_t r = ::pread(fd.get(), static_cast<char*>(dst) + got, n - got,
                                  static_cast<off_t>(off + got));
        if (r < 0) {
          if (errno == EINTR) continue;
          throw_errno("read WAL segment " + path);
        }
        if (r == 0) return false;
        got += static_cast<std::size_t>(r);
      }
      return true;
    };

    bool torn = false;
    if (cur_offset_ == 0) {
      char h[kHeaderBytes];
      if (!read_exact(0, h, sizeof h)) {
        torn = true;  // header not (completely) written
      } else {
        std::uint64_t magic, fp, first;
        std::uint32_t crc;
        std::memcpy(&magic, h, 8);
        std::memcpy(&fp, h + 8, 8);
        std::memcpy(&first, h + 16, 8);
        std::memcpy(&crc, h + 24, 4);
        if (magic != kSegmentMagic || crc != crc32c(h, 24) || first != cur_first_seq_)
          throw std::runtime_error("WAL segment " + path + " has a damaged header");
        if (fp != fingerprint_)
          throw std::runtime_error("WAL segment " + path +
                                   " was written with a different alphabet");
        cur_offset_ = kHeaderBytes;
      }
    }

    std::string payload;
    WalRecord rec;
    while (!torn) {
      std::uint32_t frame[2];
      if (!read_exact(cur_offset_, frame, sizeof frame)) {
        struct stat st{};
        const bool at_end = ::fstat(fd.get(), &st) == 0 &&
                            static_cast<std::uint64_t>(st.st_size) == cur_offset_;
        torn = !at_end;  // a partial length/crc prefix is a torn write
        break;
      }
      if (frame[0] == 0 || frame[0] > kMaxPayload) {
        torn = true;
        break;
      }
      payload.resize(frame[0]);
      if (!read_exact(cur_offset_ + 8, payload.data(), payload.size()) ||
          crc32c(payload.data(), payload.size()) != frame[1] ||
          !decode_wal_payload(payload, rec)) {
        torn = true;
        break;
      }
      cur_offset_ += 8 + payload.size();
      if (rec.seq <= last_seq_) continue;  // already covered by the snapshot
      if (rec.seq != last_seq_ + 1)
        throw std::runtime_error("WAL gap in " + path + ": expected record " +
                                 std::to_string(last_seq_ + 1) + ", found " +
                                 std::to_string(rec.seq));
      fn(rec);
      last_seq_ = rec.seq;
      ++result.delivered;
    }

    if (torn) {
      if (!is_last)
        throw std::runtime_error("WAL segment " + path + " is damaged at byte " +
                                 std::to_string(cur_offset_) + " (not the newest segment)");
      result.torn_tail = true;
      result.torn_path = path;
      result.torn_offset = cur_offset_;
      return result;
    }
    if (is_last) return result;  // caught up with the end of the log
    cur_first_seq_ = segs[idx + 1].first_seq;  // finished this segment, continue with the next
    cur_offset_ = 0;
  }
}

}  // namespace qac
