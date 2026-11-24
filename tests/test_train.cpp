#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "qac/engine.hpp"
#include "qac/train.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;
namespace fs = std::filesystem;

namespace {

const Alphabet& alphabet() {
  static const Alphabet a = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  return a;
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream(path, std::ios::binary) << content;
}

TrainOptions options(const TempDir& dir, const std::string& input) {
  TrainOptions o;
  o.input = input;
  o.data_dir = dir.sub("data");
  o.progress_every = 0;
  return o;
}

EngineConfig engine_config(const TempDir& dir) {
  EngineConfig c;
  c.data_dir = dir.sub("data");
  c.fsync = FsyncPolicy::Never;
  c.snapshot_interval_s = 0;
  c.snapshot_wal_bytes = 0;
  c.cache_entries = 0;
  return c;
}

std::vector<std::string> complete(const Engine& e, const std::string& q) {
  CompleteRequest r;
  r.query = q;
  r.n = 50;
  r.m = 0;
  std::vector<std::string> out;
  for (const auto& s : e.complete(r).global) out.push_back(s.text + "|" + std::to_string(s.freq));
  return out;
}

}  // namespace

TEST(Train, BuildsADirectoryTheEngineServes) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in,
             "new york weather\t5\n"
             "new york times\n"
             "New York Times\n"        // case folds into the previous query
             "\n"
             "new york weather\t3\n"   // counts accumulate
             "привет мир\t2\n");
  const TrainReport rep = train(alphabet(), options(dir, in));
  EXPECT_EQ(rep.lines, 6u);
  EXPECT_EQ(rep.blank, 1u);
  EXPECT_EQ(rep.accepted, 5u);
  EXPECT_EQ(rep.rejected, 0u);
  EXPECT_EQ(rep.total_count, 5u + 1 + 1 + 3 + 2);
  EXPECT_EQ(rep.index.global_queries, 3u);
  EXPECT_GT(rep.snapshot_bytes, 0u);

  Engine e(alphabet(), engine_config(dir));
  EXPECT_EQ(complete(e, "new york "), (std::vector<std::string>{"new york weather|8", "new york times|2"}));
  EXPECT_EQ(complete(e, "при"), (std::vector<std::string>{"привет мир|2"}));
  // The directory stays fully writable afterwards.
  EXPECT_TRUE(e.add_global("new york marathon", 1).ok());
  EXPECT_EQ(complete(e, "new york m").size(), 1u);
}

TEST(Train, RejectsMalformedLinesAndReportsThem) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in,
             "good query\r\n"           // CRLF
             "日本語\n"                  // outside the alphabet
             "bad \xC3\x28 utf8\n"      // invalid UTF-8
             "zero count\t0\n"
             "huge count\t99999999999\n"
             "fine again\n");
  auto o = options(dir, in);
  o.error_samples = 2;
  const TrainReport rep = train(alphabet(), o);
  EXPECT_EQ(rep.accepted, 2u);
  EXPECT_EQ(rep.rejected, 4u);
  ASSERT_EQ(rep.samples.size(), 2u);  // capped
  EXPECT_EQ(rep.samples[0].line, 2u);
  EXPECT_EQ(rep.samples[1].line, 3u);

  Engine e(alphabet(), engine_config(dir));
  EXPECT_EQ(complete(e, "good ").size(), 1u);
  EXPECT_EQ(complete(e, "fine ").size(), 1u);
}

TEST(Train, ByteOrderMarkAndTabsInsideQueries) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in, "\xEF\xBB\xBF" "first line\nnot a count\tabc\n");
  const TrainReport rep = train(alphabet(), options(dir, in));
  EXPECT_EQ(rep.accepted, 2u);
  Engine e(alphabet(), engine_config(dir));
  EXPECT_EQ(complete(e, "first ").size(), 1u);
  EXPECT_EQ(complete(e, "not a count abc").size(), 1u);  // the tab was only whitespace
}

TEST(Train, CountsCanBeDisabled) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in, "route 66\t7\n");
  auto o = options(dir, in);
  o.counts = false;
  EXPECT_EQ(train(alphabet(), o).accepted, 1u);
  Engine e(alphabet(), engine_config(dir));
  EXPECT_EQ(complete(e, "route 66 7"), (std::vector<std::string>{"route 66 7|1"}));
}

TEST(Train, RefusesToReplaceDataWithoutOverwrite) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in, "alpha beta\n");
  train(alphabet(), options(dir, in));

  EXPECT_THROW(train(alphabet(), options(dir, in)), std::runtime_error);

  write_file(in, "gamma delta\n");
  auto o = options(dir, in);
  o.overwrite = true;
  EXPECT_EQ(train(alphabet(), o).accepted, 1u);
  Engine e(alphabet(), engine_config(dir));
  EXPECT_TRUE(complete(e, "alpha ").empty());
  EXPECT_EQ(complete(e, "gamma ").size(), 1u);
}

TEST(Train, WalLeftoversCountAsData) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in, "alpha beta\n");
  {
    Engine e(alphabet(), engine_config(dir));  // creates a WAL segment
    e.add_global("old data", 1);
  }
  EXPECT_THROW(train(alphabet(), options(dir, in)), std::runtime_error);
  auto o = options(dir, in);
  o.overwrite = true;
  train(alphabet(), o);
  Engine e(alphabet(), engine_config(dir));
  EXPECT_TRUE(complete(e, "old ").empty());
  EXPECT_EQ(complete(e, "alpha ").size(), 1u);
}

TEST(Train, RefusesADirectoryThatIsInUse) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  write_file(in, "alpha beta\n");
  Engine e(alphabet(), engine_config(dir));
  auto o = options(dir, in);
  o.overwrite = true;
  EXPECT_THROW(train(alphabet(), o), std::runtime_error);
  EXPECT_TRUE(complete(e, "alpha ").empty());  // untouched
}

TEST(Train, MissingInputIsAnError) {
  TempDir dir;
  EXPECT_THROW(train(alphabet(), options(dir, dir.sub("nope.txt"))), std::runtime_error);
}

TEST(Train, ReportsProgress) {
  TempDir dir;
  const std::string in = dir.sub("queries.txt");
  std::string content;
  for (int i = 0; i < 25; ++i) content += "query " + std::to_string(i) + "\n";
  write_file(in, content);
  auto o = options(dir, in);
  o.progress_every = 10;
  std::vector<std::uint64_t> seen;
  o.progress = [&](std::uint64_t n) { seen.push_back(n); };
  train(alphabet(), o);
  EXPECT_EQ(seen, (std::vector<std::uint64_t>{10, 20}));
}
