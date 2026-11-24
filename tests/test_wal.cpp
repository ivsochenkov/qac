#include <gtest/gtest.h>

#include <filesystem>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>

#include "qac/crc32c.hpp"
#include "qac/snapshot.hpp"
#include "qac/wal.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kFp = 0xABCDEF0123456789ull;

WalRecord make_add(std::uint64_t seq, const std::string& text, std::uint32_t count = 1) {
  WalRecord r;
  r.seq = seq;
  r.op = WalOp::AddGlobal;
  r.count = count;
  r.words = split(text);
  return r;
}

std::vector<WalRecord> read_all(const std::string& dir, std::uint64_t after = 0,
                                WalReader::Result* res = nullptr) {
  std::vector<WalRecord> out;
  WalReader reader(dir, kFp, after);
  const auto r = reader.read_more([&](const WalRecord& rec) { out.push_back(rec); });
  if (res) *res = r;
  return out;
}

std::string last_segment(const std::string& dir) { return list_wal_segments(dir).back().path; }

void append_bytes(const std::string& path, const std::string& bytes) {
  std::ofstream(path, std::ios::binary | std::ios::app) << bytes;
}

}  // namespace

TEST(WalRecord, RoundTripAllOps) {
  WalRecord a = make_add(7, "alpha beta", 3);
  WalRecord b;
  b.seq = 8;
  b.op = WalOp::AddUser;
  b.user = 4000000000u;
  b.count = 2;
  b.timestamp = 1700000000u;
  b.also_global = true;
  b.words = {"x", "yz"};
  WalRecord c;
  c.seq = 9;
  c.op = WalOp::RemoveGlobal;
  c.id = 123456;
  WalRecord d;
  d.seq = 10;
  d.op = WalOp::RemoveUser;
  d.user = 77;
  d.id = 5;

  for (const WalRecord* r : {&a, &b, &c, &d}) {
    std::string payload;
    encode_wal_payload(*r, payload);
    WalRecord back;
    ASSERT_TRUE(decode_wal_payload(payload, back));
    EXPECT_EQ(back.seq, r->seq);
    EXPECT_EQ(back.op, r->op);
    EXPECT_EQ(back.user, r->user);
    EXPECT_EQ(back.count, r->count);
    EXPECT_EQ(back.timestamp, r->timestamp);
    EXPECT_EQ(back.also_global, r->also_global);
    EXPECT_EQ(back.id, r->id);
    EXPECT_EQ(back.words, r->words);
  }
}

TEST(WalRecord, DecodeRejectsGarbage) {
  WalRecord r;
  EXPECT_FALSE(decode_wal_payload("", r));
  EXPECT_FALSE(decode_wal_payload("short", r));
  std::string payload;
  encode_wal_payload(make_add(1, "a b"), payload);
  EXPECT_FALSE(decode_wal_payload(payload.substr(0, payload.size() - 1), r));  // truncated
  EXPECT_FALSE(decode_wal_payload(payload + "x", r));                          // trailing bytes
  payload[8] = 99;                                                             // unknown op
  EXPECT_FALSE(decode_wal_payload(payload, r));
}

TEST(WalLog, WriteAndReadBack) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    for (std::uint64_t i = 1; i <= 100; ++i) w.append(make_add(i, "query number " + std::to_string(i), i));
  }
  const auto recs = read_all(dir.str());
  ASSERT_EQ(recs.size(), 100u);
  EXPECT_EQ(recs[0].seq, 1u);
  EXPECT_EQ(recs[99].words.back(), "100");
  EXPECT_EQ(recs[41].count, 42u);
}

TEST(WalLog, ReadsAcrossSegmentsAndSkipsCoveredRecords) {
  TempDir dir;
  WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
  w.open_segment(1);
  for (std::uint64_t i = 1; i <= 10; ++i) w.append(make_add(i, "a"));
  w.open_segment(11);
  for (std::uint64_t i = 11; i <= 20; ++i) w.append(make_add(i, "b"));
  w.open_segment(21);
  for (std::uint64_t i = 21; i <= 25; ++i) w.append(make_add(i, "c"));

  EXPECT_EQ(list_wal_segments(dir.str()).size(), 3u);
  EXPECT_EQ(read_all(dir.str(), 0).size(), 25u);
  const auto tail = read_all(dir.str(), 12);
  ASSERT_EQ(tail.size(), 13u);
  EXPECT_EQ(tail.front().seq, 13u);
  EXPECT_EQ(read_all(dir.str(), 25).size(), 0u);
}

TEST(WalLog, EmptySegmentRotationIsNotRepeated) {
  TempDir dir;
  WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
  w.open_segment(1);
  w.open_segment(1);
  w.open_segment(1);
  EXPECT_EQ(list_wal_segments(dir.str()).size(), 1u);
  w.append(make_add(1, "x"));
  w.open_segment(2);
  EXPECT_EQ(list_wal_segments(dir.str()).size(), 2u);
}

TEST(WalLog, TornTailIsReportedAndPositionIsExact) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    for (std::uint64_t i = 1; i <= 5; ++i) w.append(make_add(i, "hello world"));
  }
  const std::string seg = last_segment(dir.str());
  const auto good_size = fs::file_size(seg);
  fs::resize_file(seg, good_size - 3);  // cut into the last record

  WalReader::Result res;
  const auto recs = read_all(dir.str(), 0, &res);
  EXPECT_EQ(recs.size(), 4u);
  EXPECT_TRUE(res.torn_tail);
  EXPECT_EQ(res.torn_path, seg);
  EXPECT_LT(res.torn_offset, good_size - 3);
  // truncating at the reported offset yields a clean log
  fs::resize_file(seg, res.torn_offset);
  WalReader::Result res2;
  EXPECT_EQ(read_all(dir.str(), 0, &res2).size(), 4u);
  EXPECT_FALSE(res2.torn_tail);
}

TEST(WalLog, PartialLengthPrefixIsTorn) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    w.append(make_add(1, "x"));
  }
  append_bytes(last_segment(dir.str()), std::string("\x05\x00", 2));
  WalReader::Result res;
  EXPECT_EQ(read_all(dir.str(), 0, &res).size(), 1u);
  EXPECT_TRUE(res.torn_tail);
}

TEST(WalLog, CorruptedRecordStopsReplayInLastSegment) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    for (std::uint64_t i = 1; i <= 6; ++i) w.append(make_add(i, "record"));
  }
  const std::string seg = last_segment(dir.str());
  {
    // flip one byte in the middle of the file
    std::fstream f(seg, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(fs::file_size(seg) / 2));
    f.put('\x7F');
  }
  WalReader::Result res;
  const auto recs = read_all(dir.str(), 0, &res);
  EXPECT_LT(recs.size(), 6u);
  EXPECT_TRUE(res.torn_tail);
}

TEST(WalLog, DamageInOlderSegmentIsFatal) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    for (std::uint64_t i = 1; i <= 5; ++i) w.append(make_add(i, "aaa"));
    w.open_segment(6);
    for (std::uint64_t i = 6; i <= 8; ++i) w.append(make_add(i, "bbb"));
  }
  const std::string first = list_wal_segments(dir.str()).front().path;
  {
    std::fstream f(first, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(40);  // inside the first record's payload (the bytes there are 0x00)
    f.put('\xFF');
    f.put('\xFF');
  }
  EXPECT_THROW(read_all(dir.str()), std::runtime_error);
}

TEST(WalLog, MissingSegmentIsDetectedAsGap) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    for (std::uint64_t i = 1; i <= 3; ++i) w.append(make_add(i, "a"));
    w.open_segment(4);
    for (std::uint64_t i = 4; i <= 6; ++i) w.append(make_add(i, "b"));
    w.open_segment(7);
    for (std::uint64_t i = 7; i <= 9; ++i) w.append(make_add(i, "c"));
  }
  fs::remove(list_wal_segments(dir.str())[1].path);  // the middle one
  EXPECT_THROW(read_all(dir.str()), std::runtime_error);
  // but starting after the gap is fine (a snapshot covered it)
  EXPECT_EQ(read_all(dir.str(), 6).size(), 3u);
}

TEST(WalLog, SnapshotNewerThanLogDetectsMissingRecords) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(10);
    w.append(make_add(10, "a"));
  }
  // snapshot claims seq 3 but the log starts at 10: records 4..9 are gone
  EXPECT_THROW(read_all(dir.str(), 3), std::runtime_error);
}

TEST(WalLog, DifferentAlphabetIsRejected) {
  TempDir dir;
  {
    WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
    w.open_segment(1);
    w.append(make_add(1, "a"));
  }
  WalReader reader(dir.str(), kFp + 1, 0);
  EXPECT_THROW(reader.read_more([](const WalRecord&) {}), std::runtime_error);
}

TEST(WalLog, IncrementalReaderFollowsGrowthAndRotation) {
  TempDir dir;
  WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
  w.open_segment(1);
  for (std::uint64_t i = 1; i <= 3; ++i) w.append(make_add(i, "one"));

  std::vector<std::uint64_t> seen;
  WalReader reader(dir.str(), kFp, 0);
  const auto cb = [&](const WalRecord& r) { seen.push_back(r.seq); };
  EXPECT_EQ(reader.read_more(cb).delivered, 3u);
  EXPECT_EQ(reader.read_more(cb).delivered, 0u);  // nothing new

  for (std::uint64_t i = 4; i <= 6; ++i) w.append(make_add(i, "two"));
  w.open_segment(7);
  for (std::uint64_t i = 7; i <= 9; ++i) w.append(make_add(i, "three"));
  EXPECT_EQ(reader.read_more(cb).delivered, 6u);
  EXPECT_EQ(reader.last_seq(), 9u);
  ASSERT_EQ(seen.size(), 9u);
  for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], i + 1);
}

TEST(WalLog, IncrementalReaderRetriesAPartialRecord) {
  TempDir dir;
  WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
  w.open_segment(1);
  w.append(make_add(1, "complete"));
  const std::string seg = last_segment(dir.str());

  std::vector<std::uint64_t> seen;
  WalReader reader(dir.str(), kFp, 0);
  const auto cb = [&](const WalRecord& r) { seen.push_back(r.seq); };
  EXPECT_EQ(reader.read_more(cb).delivered, 1u);

  // simulate a write in progress: half of a record is visible
  std::string payload;
  encode_wal_payload(make_add(2, "partial one"), payload);
  std::string frame(8, '\0');
  const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  const std::uint32_t crc = crc32c(payload.data(), payload.size());
  std::memcpy(frame.data(), &len, 4);
  std::memcpy(frame.data() + 4, &crc, 4);
  frame += payload;
  append_bytes(seg, frame.substr(0, frame.size() - 4));
  const auto r1 = reader.read_more(cb);
  EXPECT_EQ(r1.delivered, 0u);
  EXPECT_TRUE(r1.torn_tail);

  append_bytes(seg, frame.substr(frame.size() - 4));  // the write completes
  const auto r2 = reader.read_more(cb);
  EXPECT_EQ(r2.delivered, 1u);
  EXPECT_FALSE(r2.torn_tail);
  EXPECT_EQ(seen, (std::vector<std::uint64_t>{1, 2}));
}

TEST(WalLog, PruneKeepsOnlySegmentsNeededAfterSnapshot) {
  TempDir dir;
  WalWriter w(dir.str(), kFp, FsyncPolicy::Never);
  w.open_segment(1);
  for (std::uint64_t i = 1; i <= 5; ++i) w.append(make_add(i, "a"));
  w.open_segment(6);
  for (std::uint64_t i = 6; i <= 10; ++i) w.append(make_add(i, "b"));
  w.open_segment(11);
  for (std::uint64_t i = 11; i <= 12; ++i) w.append(make_add(i, "c"));

  w.prune_covered_by(4);  // segment 1 still holds 5
  EXPECT_EQ(list_wal_segments(dir.str()).size(), 3u);
  w.prune_covered_by(5);  // segment 1 (1..5) is fully covered
  EXPECT_EQ(list_wal_segments(dir.str()).size(), 2u);
  w.prune_covered_by(100);  // the open segment is never deleted
  const auto left = list_wal_segments(dir.str());
  ASSERT_EQ(left.size(), 1u);
  EXPECT_EQ(left[0].first_seq, 11u);
  EXPECT_EQ(read_all(dir.str(), 10).size(), 2u);
}

TEST(WalLog, FsyncPolicies) {
  FsyncPolicy p;
  EXPECT_TRUE(parse_fsync_policy("always", p));
  EXPECT_EQ(p, FsyncPolicy::Always);
  EXPECT_TRUE(parse_fsync_policy("interval", p));
  EXPECT_TRUE(parse_fsync_policy("never", p));
  EXPECT_FALSE(parse_fsync_policy("sometimes", p));

  TempDir dir;
  WalWriter w(dir.str(), kFp, FsyncPolicy::Always);
  w.open_segment(1);
  w.append(make_add(1, "durable"));
  w.sync_if_dirty();  // must not throw
  EXPECT_EQ(read_all(dir.str()).size(), 1u);
}

// ------------------------------------------------------------------ snapshots

namespace {
Index sample_index() {
  Index idx;
  add(idx, "hello world", 3);
  add(idx, "hello there", 2);
  add_user(idx, 9, "my history", 4, 123);
  return idx;
}
}  // namespace

TEST(SnapshotFile, RoundTrip) {
  TempDir dir;
  const std::string path = dir.sub("snap.bin");
  const Index idx = sample_index();
  const auto info = write_snapshot(path, idx, 42, kFp);
  EXPECT_EQ(info.last_seq, 42u);
  EXPECT_EQ(info.file_bytes, fs::file_size(path));

  auto loaded = read_snapshot(path);
  EXPECT_EQ(loaded.info.last_seq, 42u);
  EXPECT_EQ(loaded.info.alphabet_fingerprint, kFp);
  EXPECT_EQ(global_texts(loaded.index, "hello ").size(), 2u);
  EXPECT_EQ(user_texts(loaded.index, 9, "my ").size(), 1u);
}

TEST(SnapshotFile, DetectsBitFlip) {
  TempDir dir;
  const std::string path = dir.sub("snap.bin");
  write_snapshot(path, sample_index(), 1, kFp);
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(fs::file_size(path) / 2));
    char c = 0;
    f.read(&c, 1);
    f.seekp(-1, std::ios::cur);
    f.put(static_cast<char>(c ^ 0x01));
  }
  EXPECT_THROW(read_snapshot(path), std::runtime_error);
}

TEST(SnapshotFile, DetectsTruncation) {
  TempDir dir;
  const std::string path = dir.sub("snap.bin");
  write_snapshot(path, sample_index(), 1, kFp);
  const auto size = fs::file_size(path);
  for (std::uintmax_t cut : {std::uintmax_t{0}, std::uintmax_t{10}, size / 2, size - 1, size - 4}) {
    fs::resize_file(path, cut);
    EXPECT_THROW(read_snapshot(path), std::runtime_error) << "cut at " << cut;
    write_snapshot(path, sample_index(), 1, kFp);
  }
}

TEST(SnapshotFile, RejectsForeignFilesAndMissingFiles) {
  TempDir dir;
  { std::ofstream(dir.sub("junk.bin")) << "this is not a snapshot at all, definitely not"; }
  EXPECT_THROW(read_snapshot(dir.sub("junk.bin")), std::runtime_error);
  EXPECT_THROW(read_snapshot(dir.sub("missing.bin")), std::runtime_error);
}
