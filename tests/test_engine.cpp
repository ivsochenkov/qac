#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "qac/engine.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;
namespace fs = std::filesystem;

namespace {

const Alphabet& alphabet() {
  static const Alphabet a = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  return a;
}

EngineConfig make_config(const TempDir& dir, SnapshotMode mode) {
  EngineConfig c;
  c.data_dir = dir.sub("data");
  c.fsync = FsyncPolicy::Never;
  c.snapshot_mode = mode;
  c.snapshot_interval_s = 0;
  c.snapshot_wal_bytes = 0;
  c.cache_entries = 0;  // tests that exercise the cache switch it on
  return c;
}

CompleteResult global(const Engine& e, const std::string& q, std::size_t n = 10) {
  CompleteRequest r;
  r.query = q;
  r.n = n;
  r.m = 0;
  return e.complete(r);
}

CompleteResult user(const Engine& e, std::uint32_t u, const std::string& q, std::size_t m = 10) {
  CompleteRequest r;
  r.query = q;
  r.n = 0;
  r.user = u;
  r.m = m;
  return e.complete(r);
}

std::vector<std::string> texts(const std::vector<Suggestion>& v) {
  std::vector<std::string> out;
  for (const auto& s : v) out.push_back(s.text);
  return out;
}

// A deterministic fingerprint of everything observable through complete().
std::vector<std::string> dump(const Engine& e) {
  std::vector<std::string> out;
  for (const char* typed : {"", "a", "ab", "new ", "new y", "q", "query ", "query 1", "hello ", "h",
                            "w", "word", "my ", "sh"}) {
    const auto r = global(e, typed, 200);
    EXPECT_TRUE(r.ok()) << r.message;
    for (const auto& s : r.global)
      out.push_back(std::string("g|") + typed + "|" + std::to_string(s.id) + "|" + s.text + "|" +
                    std::to_string(s.freq));
    for (std::uint32_t u : {1u, 2u, 3u, 42u}) {
      const auto ur = user(e, u, typed, 200);
      for (const auto& s : ur.user)
        out.push_back("u" + std::to_string(u) + "|" + typed + "|" + std::to_string(s.id) + "|" +
                      s.text + "|" + std::to_string(s.freq));
    }
  }
  return out;
}

void fill_sample(Engine& e) {
  ASSERT_TRUE(e.add_global("New York", 5).ok());
  ASSERT_TRUE(e.add_global("new york weather", 9).ok());
  ASSERT_TRUE(e.add_global("new zealand", 2).ok());
  ASSERT_TRUE(e.add_global("hello world", 4).ok());
  ASSERT_TRUE(e.add_global("hello there", 3).ok());
  ASSERT_TRUE(e.add_global("привет мир", 7).ok());
  ASSERT_TRUE(e.add_user(1, "my shopping list", 2).ok());
  ASSERT_TRUE(e.add_user(1, "my shoes", 5, true).ok());
  ASSERT_TRUE(e.add_user(2, "my secret", 1).ok());
  ASSERT_TRUE(e.add_user(42, "query 1", 3).ok());
}

class StubFixer : public SpellFixer {
 public:
  std::string fix(const std::string& text) const override {
    std::string out = text;
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"teh", "the"}, {"wrold", "world"}, {"bad", "日本"}, {"boom", "BOOM"}}) {
      std::size_t pos = 0;
      while ((pos = out.find(from, pos)) != std::string::npos) {
        out.replace(pos, from.size(), to);
        pos += to.size();
      }
    }
    if (text.find("boom") != std::string::npos) throw std::runtime_error("model exploded");
    return out;
  }
};

}  // namespace

// ================================================================== basics

class EngineTest : public ::testing::TestWithParam<SnapshotMode> {
 protected:
  TempDir dir;
  EngineConfig cfg() const { return make_config(dir, GetParam()); }
};

INSTANTIATE_TEST_SUITE_P(Modes, EngineTest,
                         ::testing::Values(SnapshotMode::Fork, SnapshotMode::Inline),
                         [](const auto& info) {
                           return info.param == SnapshotMode::Fork ? "Fork" : "Inline";
                         });

TEST_P(EngineTest, AddAndComplete) {
  Engine e(alphabet(), cfg());
  fill_sample(e);

  auto r = global(e, "new ");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"new york weather", "new york", "new zealand"}));
  EXPECT_EQ(r.global[0].freq, 9u);
  EXPECT_NE(r.global[0].id, kNoQuery);

  r = global(e, "NEW YO");  // case-insensitive, partial last word
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"new york weather", "new york"}));

  r = global(e, "прив");
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"привет мир"}));

  EXPECT_TRUE(global(e, "nothing like it ").global.empty());
  EXPECT_TRUE(global(e, "").global.empty());
}

TEST_P(EngineTest, TopNIsHonoured) {
  Engine e(alphabet(), cfg());
  for (int i = 0; i < 30; ++i) ASSERT_TRUE(e.add_global("item " + std::to_string(i), 1 + i).ok());
  EXPECT_EQ(global(e, "item ", 5).global.size(), 5u);
  EXPECT_EQ(global(e, "item ", 5).global[0].text, "item 29");
  EXPECT_EQ(global(e, "item ", 100).global.size(), 30u);
  EXPECT_TRUE(global(e, "item ", 0).global.empty());
}

TEST_P(EngineTest, UserHistoryAndGlobalTogether) {
  Engine e(alphabet(), cfg());
  fill_sample(e);
  CompleteRequest req;
  req.query = "my sh";
  req.n = 5;
  req.user = 1;
  req.m = 5;
  const auto r = e.complete(req);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"my shoes"}));  // added with also_global
  ASSERT_EQ(r.user.size(), 2u);
  EXPECT_EQ(r.user[0].text, "my shoes");  // higher frequency first
  EXPECT_EQ(r.user[0].freq, 5u);
  EXPECT_GT(r.user[0].last_used, 1'600'000'000u);
  EXPECT_EQ(r.user[1].text, "my shopping list");

  // another user sees none of it
  req.user = 2;
  EXPECT_TRUE(e.complete(req).user.empty());
}

TEST_P(EngineTest, InvalidInputIsRejected) {
  Engine e(alphabet(), cfg());
  EXPECT_EQ(e.add_global("日本語", 1).status, Status::BadInput);
  EXPECT_EQ(e.add_global("", 1).status, Status::BadInput);
  EXPECT_EQ(e.add_global("!!!", 1).status, Status::BadInput);
  EXPECT_EQ(e.add_global("ok", 0).status, Status::BadInput);
  EXPECT_EQ(e.add_global(std::string("a\xFF"), 1).status, Status::BadInput);
  EXPECT_EQ(e.add_user(1, "emoji 😀", 1).status, Status::BadInput);
  EXPECT_EQ(global(e, "bad 日本").status, Status::BadInput);
  EXPECT_EQ(e.stats().index.query_ids_allocated, 0u);  // nothing leaked into the index
  EXPECT_EQ(e.stats().last_seq, 0u);                   // and nothing was logged
}

TEST_P(EngineTest, AddReturnsNormalizedTextAndStableIds) {
  Engine e(alphabet(), cfg());
  const auto a = e.add_global("  Hello,   WORLD!  ", 2);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.text, "hello world");
  EXPECT_TRUE(a.created);
  const auto b = e.add_global("hello world", 3);
  EXPECT_FALSE(b.created);
  EXPECT_EQ(a.id, b.id);
  EXPECT_EQ(b.freq, 5u);
}

TEST_P(EngineTest, RemoveByIdFromGlobalPoolAndUserHistory) {
  Engine e(alphabet(), cfg());
  const auto g = e.add_global("shared query", 3);
  ASSERT_TRUE(e.add_user(7, "shared query", 1).ok());
  const auto only_user = e.add_user(7, "private query", 1);

  EXPECT_TRUE(e.remove_global(g.id).ok());
  EXPECT_TRUE(global(e, "shared ").global.empty());
  EXPECT_EQ(user(e, 7, "shared ").user.size(), 1u);  // history entry survives
  EXPECT_EQ(e.remove_global(g.id).status, Status::NotFound);

  EXPECT_TRUE(e.remove_user(7, g.id).ok());
  EXPECT_TRUE(user(e, 7, "shared ").user.empty());
  EXPECT_EQ(e.remove_user(7, g.id).status, Status::NotFound);

  EXPECT_EQ(e.remove_user(8, only_user.id).status, Status::NotFound);  // not this user's
  EXPECT_TRUE(e.remove_user(7, only_user.id).ok());
  EXPECT_EQ(e.remove_global(999999).status, Status::NotFound);
  EXPECT_EQ(e.remove_global(0).status, Status::NotFound);
}

TEST_P(EngineTest, FailedRemovalsAreNotLogged) {
  Engine e(alphabet(), cfg());
  e.add_global("x y", 1);
  const auto before = e.stats().last_seq;
  EXPECT_EQ(e.remove_global(424242).status, Status::NotFound);
  EXPECT_EQ(e.stats().last_seq, before);
}

// ================================================================== persistence

TEST_P(EngineTest, WalAloneRestoresEverythingIncludingIds) {
  std::vector<std::string> before;
  QueryId new_id = 0;
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
    const auto doomed = e.add_global("to be removed", 1);
    ASSERT_TRUE(e.remove_global(doomed.id).ok());
    new_id = e.add_global("fresh query after all that", 1).id;
    before = dump(e);
  }
  EXPECT_FALSE(fs::exists(dir.sub("data/snapshot.bin")));  // recovery uses the log only
  Engine e2(alphabet(), cfg());
  EXPECT_EQ(e2.stats().recovered_records, 13u);
  EXPECT_EQ(dump(e2), before);
  EXPECT_EQ(global(e2, "fresh ").global.at(0).id, new_id);
  // the next id continues where the first run stopped
  EXPECT_EQ(e2.add_global("another one", 1).id, new_id + 1);
}

TEST_P(EngineTest, ReopenGivesIdenticalState) {
  std::vector<std::string> before;
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
    for (int i = 0; i < 50; ++i) e.add_global("query " + std::to_string(i), 1 + i % 5);
    for (int i = 0; i < 20; ++i) e.add_user(1 + i % 3, "my item " + std::to_string(i), 1 + i);
    before = dump(e);
  }
  Engine e2(alphabet(), cfg());
  EXPECT_EQ(dump(e2), before);
}

TEST_P(EngineTest, SnapshotPlusWalTail) {
  std::vector<std::string> before;
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
    const auto snap = e.snapshot();
    ASSERT_TRUE(snap.ok()) << snap.message;
    EXPECT_EQ(snap.last_seq, 10u);
    EXPECT_GT(snap.file_bytes, 0u);
    for (int i = 0; i < 10; ++i) e.add_global("query " + std::to_string(i), 2);
    ASSERT_TRUE(e.remove_user(2, e.add_user(2, "my temp", 1).id).ok());
    before = dump(e);
    EXPECT_EQ(e.stats().snapshot_seq, 10u);
  }
  EXPECT_TRUE(fs::exists(dir.sub("data/snapshot.bin")));
  Engine e2(alphabet(), cfg());
  EXPECT_EQ(e2.stats().recovered_records, 12u);  // only the tail is replayed
  EXPECT_EQ(dump(e2), before);
}

TEST_P(EngineTest, SnapshotTruncatesTheLog) {
  Engine e(alphabet(), cfg());
  for (int i = 0; i < 100; ++i) e.add_global("padding query number " + std::to_string(i), 1);
  EXPECT_GE(e.stats().wal_bytes, 1000u);
  ASSERT_TRUE(e.snapshot().ok());
  EXPECT_EQ(list_wal_segments(dir.sub("data")).size(), 1u);  // only the fresh segment remains
  EXPECT_LT(e.stats().wal_bytes, 64u);                       // header only
  e.add_global("after snapshot", 1);
  EXPECT_GT(e.stats().wal_bytes, 64u);
}

TEST_P(EngineTest, RepeatedSnapshotsAreHarmless) {
  Engine e(alphabet(), cfg());
  fill_sample(e);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(e.snapshot().ok());
    e.add_global("round " + std::to_string(i), 1);
  }
  const auto before = dump(e);
  ASSERT_TRUE(e.snapshot().ok());
  ASSERT_TRUE(e.snapshot().ok());  // nothing changed in between
  EXPECT_EQ(dump(e), before);
}

TEST_P(EngineTest, TornWalTailIsDiscardedOnStart) {
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
    e.add_global("last complete record", 1);
  }
  const auto seg = list_wal_segments(dir.sub("data")).back().path;
  {
    std::ofstream f(seg, std::ios::binary | std::ios::app);
    f << std::string("\x40\x00\x00\x00\xDE\xAD\xBE\xEF" "partial-garbage", 23);
  }
  Engine e2(alphabet(), cfg());
  EXPECT_EQ(global(e2, "last ").global.size(), 1u);
  // writing continues normally and survives yet another restart
  ASSERT_TRUE(e2.add_global("written after recovery", 1).ok());
  e2.~Engine();
  new (&e2) Engine(alphabet(), cfg());
  EXPECT_EQ(global(e2, "written ").global.size(), 1u);
  EXPECT_EQ(global(e2, "last ").global.size(), 1u);
}

TEST_P(EngineTest, CorruptSnapshotRefusesToStart) {
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
    ASSERT_TRUE(e.snapshot().ok());
  }
  const std::string snap = dir.sub("data/snapshot.bin");
  {
    std::fstream f(snap, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(fs::file_size(snap) / 2));
    f.put('\x55');
    f.put('\xAA');
  }
  EXPECT_THROW(Engine(alphabet(), cfg()), std::runtime_error);
}

TEST_P(EngineTest, DifferentAlphabetRefusesToStart) {
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
    ASSERT_TRUE(e.snapshot().ok());
  }
  const Alphabet other = Alphabet::from_string("a A\nb B\nc C\n");
  EXPECT_THROW(Engine(other, cfg()), std::runtime_error);
}

TEST_P(EngineTest, StaleTemporarySnapshotIsIgnored) {
  {
    Engine e(alphabet(), cfg());
    fill_sample(e);
  }
  { std::ofstream(dir.sub("data/snapshot.tmp")) << "half written garbage"; }
  { std::ofstream(dir.sub("data/snapshot.opt.tmp")) << "half written garbage"; }
  Engine e2(alphabet(), cfg());
  EXPECT_EQ(global(e2, "new ").global.size(), 3u);
  EXPECT_FALSE(fs::exists(dir.sub("data/snapshot.tmp")));
}

TEST_P(EngineTest, WalRotatesBySize) {
  auto c = cfg();
  c.wal_segment_bytes = 2000;
  std::vector<std::string> before;
  {
    Engine e(alphabet(), c);
    for (int i = 0; i < 200; ++i) e.add_global("rotating segments query " + std::to_string(i), 1);
    before = dump(e);
    EXPECT_GE(list_wal_segments(dir.sub("data")).size(), 4u);
  }
  Engine e2(alphabet(), c);
  EXPECT_EQ(dump(e2), before);
  EXPECT_EQ(e2.stats().recovered_records, 200u);
}

// A process that dies without running any destructor must lose nothing that was acknowledged.
TEST_P(EngineTest, SurvivesProcessKill) {
  const auto config = cfg();
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    try {
      Engine e(alphabet(), config);
      for (int i = 0; i < 300; ++i) {
        if (!e.add_global("crash test query " + std::to_string(i), 1 + i % 4).ok()) ::_exit(3);
        if (i % 3 == 0 && !e.add_user(1 + i % 3, "my crash item " + std::to_string(i), 2).ok()) ::_exit(3);
      }
      if (!e.snapshot().ok()) ::_exit(4);
      for (int i = 300; i < 400; ++i) e.add_global("crash test query " + std::to_string(i), 1);
    } catch (...) {
      ::_exit(5);
    }
    ::_exit(0);  // no destructors, no clean shutdown
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  Engine e(alphabet(), config);
  EXPECT_EQ(e.stats().index.global_queries, 400u);
  EXPECT_EQ(global(e, "crash test query 399").global.size(), 1u);
  // user 1 received every i with i % 3 == 0, i.e. 100 items
  EXPECT_EQ(user(e, 1, "my crash ", 500).user.size(), 100u);
}

TEST_P(EngineTest, SecondOpenOfTheSameDirectoryIsRejected) {
  Engine e(alphabet(), cfg());
  EXPECT_THROW(Engine(alphabet(), cfg()), std::runtime_error);

  // ... also from another process
  const auto config = cfg();
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    try {
      Engine other(alphabet(), config);
      ::_exit(1);  // must not succeed
    } catch (const std::runtime_error&) {
      ::_exit(0);
    }
    ::_exit(2);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

TEST_P(EngineTest, DirectoryCanBeReopenedAfterClose) {
  { Engine e(alphabet(), cfg()); }
  EXPECT_NO_THROW(Engine(alphabet(), cfg()));
}

// ================================================================== optimize

TEST_P(EngineTest, OptimizeKeepsBehaviourAndIdsAndDropsDeadData) {
  Engine e(alphabet(), cfg());
  fill_sample(e);
  std::vector<QueryId> doomed;
  for (int i = 0; i < 200; ++i) doomed.push_back(e.add_global("temporary unique words " + std::to_string(i), 1).id);
  for (QueryId id : doomed) ASSERT_TRUE(e.remove_global(id).ok());
  const auto before = dump(e);
  const auto stats_before = e.stats().index;
  ASSERT_EQ(stats_before.dead_queries, 200u);

  const auto r = e.optimize();
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.dead_queries_before, 200u);

  const auto stats_after = e.stats().index;
  EXPECT_EQ(stats_after.dead_queries, 0u);
  EXPECT_LT(stats_after.words, stats_before.words);
  EXPECT_LT(stats_after.arena_bytes, stats_before.arena_bytes);
  EXPECT_EQ(stats_after.global_queries, stats_before.global_queries);
  EXPECT_EQ(dump(e), before);

  // new ids continue after the old ones, as if nothing happened
  const auto next = e.add_global("brand new", 1);
  EXPECT_EQ(next.id, stats_before.query_ids_allocated + 1);

  // the old snapshot file was replaced; a restart sees the same state
  EXPECT_TRUE(fs::exists(dir.sub("data/snapshot.bin")));
  EXPECT_FALSE(fs::exists(dir.sub("data/snapshot.opt.tmp")));
  const auto after_add = dump(e);
  e.~Engine();
  new (&e) Engine(alphabet(), cfg());
  EXPECT_EQ(dump(e), after_add);
}

TEST_P(EngineTest, OptimizeOnEmptyEngine) {
  Engine e(alphabet(), cfg());
  EXPECT_TRUE(e.optimize().ok());
  EXPECT_TRUE(e.add_global("works afterwards", 1).ok());
}

TEST_P(EngineTest, OptimizeWithConcurrentWriters) {
  Engine e(alphabet(), cfg());
  fill_sample(e);
  for (int i = 0; i < 2000; ++i) e.add_global("base query " + std::to_string(i), 1 + i % 3);
  for (int i = 0; i < 1000; ++i) e.remove_global(static_cast<QueryId>(10 + i * 2));

  struct Acked {
    std::string text;
    QueryId id;
  };
  std::vector<Acked> acked;
  std::mutex acked_mu;
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int t = 0; t < 3; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; !stop; ++i) {
        const std::string text = "during optimize writer" + std::to_string(t) + " item " + std::to_string(i);
        const auto r = e.add_global(text, 1);
        if (!r.ok()) {
          ++failures;
          continue;
        }
        if (i % 10 == 0) e.add_user(100 + t, text, 1);
        if (i % 7 == 0) e.remove_global(r.id);
        {
          std::lock_guard<std::mutex> l(acked_mu);
          if (i % 7 != 0) acked.push_back({text, r.id});
        }
        if (i % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  std::thread reader([&] {
    while (!stop) {
      if (!global(e, "during ").ok()) ++failures;
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto o1 = e.optimize();
  const auto s1 = e.snapshot();
  const auto o2 = e.optimize();
  stop = true;
  for (auto& w : writers) w.join();
  reader.join();

  ASSERT_TRUE(o1.ok()) << o1.message;
  ASSERT_TRUE(s1.ok()) << s1.message;
  ASSERT_TRUE(o2.ok()) << o2.message;
  EXPECT_EQ(failures.load(), 0);
  ASSERT_GT(acked.size(), 100u);

  // every acknowledged add is present with the id the client was given
  for (const auto& a : acked) {
    const auto r = global(e, a.text + " ", 1);
    ASSERT_EQ(r.global.size(), 1u) << a.text;
    EXPECT_EQ(r.global[0].id, a.id) << a.text;
  }

  // and a restart reproduces the very same state
  const auto live = dump(e);
  const auto live_stats = e.stats().index;
  e.~Engine();
  new (&e) Engine(alphabet(), cfg());
  EXPECT_EQ(dump(e), live);
  EXPECT_EQ(e.stats().index.global_queries, live_stats.global_queries);
  EXPECT_EQ(e.stats().index.query_ids_allocated, live_stats.query_ids_allocated);
  for (const auto& a : acked) {
    const auto r = global(e, a.text + " ", 1);
    ASSERT_EQ(r.global.size(), 1u) << a.text;
    EXPECT_EQ(r.global[0].id, a.id) << a.text;
  }
}

TEST_P(EngineTest, ConcurrentMaintenanceReturnsBusy) {
  Engine e(alphabet(), cfg());
  for (int i = 0; i < 20000; ++i) e.add_global("busy test query " + std::to_string(i), 1);
  std::atomic<int> ok{0}, busy{0}, other{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&] {
      const auto r = e.snapshot();
      if (r.ok()) ++ok;
      else if (r.status == Status::Busy) ++busy;
      else ++other;
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(other.load(), 0);
  EXPECT_GE(ok.load(), 1);
  EXPECT_EQ(ok.load() + busy.load(), 4);
}

// ================================================================== spell correction

TEST_P(EngineTest, SpellCorrectionOfCompleteWords) {
  Engine e(alphabet(), cfg());
  fill_sample(e);
  e.set_spell_fixer(std::make_shared<StubFixer>());
  ASSERT_TRUE(e.has_spell_fixer());

  CompleteRequest req;
  req.query = "hello wrold";  // wrold is still being typed: not corrected
  req.correct = true;
  auto r = e.complete(req);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.corrected.has_value());

  req.query = "helo wrld ";
  r = e.complete(req);
  EXPECT_FALSE(r.corrected.has_value());  // the stub only knows teh / wrold

  e.add_global("the world cup", 3);
  req.query = "teh wrold c";
  r = e.complete(req);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r.corrected.has_value());
  EXPECT_EQ(*r.corrected, "the world c");
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"the world cup"}));

  req.correct = false;  // off by default
  r = e.complete(req);
  EXPECT_FALSE(r.corrected.has_value());
  EXPECT_TRUE(r.global.empty());
}

TEST_P(EngineTest, SpellCorrectionRequestedWithoutModel) {
  Engine e(alphabet(), cfg());
  CompleteRequest req;
  req.query = "teh ";
  req.correct = true;
  EXPECT_EQ(e.complete(req).status, Status::Unavailable);
}

TEST_P(EngineTest, SpellCorrectionFailuresAreIgnored) {
  Engine e(alphabet(), cfg());
  e.add_global("bad idea", 1);
  e.add_global("boom shakalaka", 1);
  e.set_spell_fixer(std::make_shared<StubFixer>());
  CompleteRequest req;
  req.correct = true;

  req.query = "bad ";  // the fixer answers with characters outside the alphabet
  auto r = e.complete(req);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.corrected.has_value());
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"bad idea"}));

  req.query = "boom ";  // the fixer throws
  r = e.complete(req);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"boom shakalaka"}));
}

// ================================================================== cache, limits, background

TEST_P(EngineTest, CacheServesRepeatedRequestsAndNeverReturnsRemovedQueries) {
  auto c = cfg();
  c.cache_entries = 100;
  c.cache_ttl_ms = 60000;
  Engine e(alphabet(), c);
  const auto a = e.add_global("cached thing one", 5);
  e.add_global("cached thing two", 3);

  EXPECT_EQ(global(e, "cached ").global.size(), 2u);
  EXPECT_EQ(e.stats().cache_hits, 0u);
  EXPECT_EQ(global(e, "cached ").global.size(), 2u);
  EXPECT_EQ(e.stats().cache_hits, 1u);

  ASSERT_TRUE(e.remove_global(a.id).ok());
  const auto r = global(e, "cached ");
  EXPECT_EQ(texts(r.global), (std::vector<std::string>{"cached thing two"}));
}

TEST_P(EngineTest, ScanBudgetMarksResultsTruncated) {
  auto c = cfg();
  c.scan.max_visited = 50;
  Engine e(alphabet(), c);
  for (int i = 0; i < 300; ++i) e.add_global("many results " + std::to_string(i), 1 + i % 9);
  const auto r = global(e, "many ", 5);
  EXPECT_EQ(r.global.size(), 5u);
  EXPECT_TRUE(r.truncated);
}

TEST_P(EngineTest, AutomaticSnapshotsRun) {
  auto c = cfg();
  c.snapshot_interval_s = 1;
  Engine e(alphabet(), c);
  e.start_background();
  fill_sample(e);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (e.stats().snapshot_seq != e.stats().last_seq && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(e.stats().snapshot_seq, e.stats().last_seq);
  EXPECT_GT(e.stats().last_seq, 0u);
  e.stop_background();
}

TEST_P(EngineTest, SizeTriggeredSnapshot) {
  auto c = cfg();
  c.snapshot_wal_bytes = 3000;
  Engine e(alphabet(), c);
  e.start_background();
  for (int i = 0; i < 200; ++i) e.add_global("size trigger query " + std::to_string(i), 1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (e.stats().snapshot_seq == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_GT(e.stats().snapshot_seq, 0u);
  e.stop_background();
}

TEST_P(EngineTest, StatsAreConsistent) {
  Engine e(alphabet(), cfg());
  fill_sample(e);
  global(e, "new ");
  const auto s = e.stats();
  EXPECT_EQ(s.last_seq, 10u);
  EXPECT_EQ(s.writes, 10u);
  EXPECT_EQ(s.completes, 1u);
  EXPECT_EQ(s.index.users, 3u);
  EXPECT_EQ(s.index.user_entries, 4u);
  EXPECT_EQ(s.index.global_queries, 7u);  // 6 global + "my shoes" (also_global)
  EXPECT_FALSE(s.maintenance_running);
}

// ================================================================== stress

TEST_P(EngineTest, ConcurrentMixedWorkloadThenRestart) {
  Engine e(alphabet(), cfg());
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; !stop; ++i) {
        const std::string text = "stress t" + std::to_string(t) + " q" + std::to_string(i % 200);
        const auto a = e.add_global(text, 1);
        if (!a.ok()) ++errors;
        if (i % 3 == 0) e.add_user(1 + t, text, 1, i % 2 == 0);
        if (i % 11 == 0) e.remove_global(a.id);
        if (i % 13 == 0) e.remove_user(1 + t, a.id);
      }
    });
  }
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      while (!stop) {
        CompleteRequest r;
        r.query = (t % 2) ? "stress t1 q" : "stress ";
        r.n = 10;
        r.user = 1 + t;
        r.m = 10;
        if (!e.complete(r).ok()) ++errors;
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  const auto snap = e.snapshot();  // in the middle of all that
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop = true;
  for (auto& t : threads) t.join();
  EXPECT_TRUE(snap.ok()) << snap.message;
  EXPECT_EQ(errors.load(), 0);

  std::vector<std::string> live;
  for (const char* typed : {"stress ", "stress t0 ", "stress t3 q1"}) {
    for (const auto& s : global(e, typed, 500).global) live.push_back(std::to_string(s.id) + s.text + std::to_string(s.freq));
    for (std::uint32_t u = 1; u <= 4; ++u)
      for (const auto& s : user(e, u, typed, 500).user) live.push_back(std::to_string(u) + ":" + std::to_string(s.id) + s.text + std::to_string(s.freq));
  }
  ASSERT_FALSE(live.empty());
  e.~Engine();
  new (&e) Engine(alphabet(), cfg());
  std::vector<std::string> again;
  for (const char* typed : {"stress ", "stress t0 ", "stress t3 q1"}) {
    for (const auto& s : global(e, typed, 500).global) again.push_back(std::to_string(s.id) + s.text + std::to_string(s.freq));
    for (std::uint32_t u = 1; u <= 4; ++u)
      for (const auto& s : user(e, u, typed, 500).user) again.push_back(std::to_string(u) + ":" + std::to_string(s.id) + s.text + std::to_string(s.freq));
  }
  EXPECT_EQ(again, live);
}

// ============================================== ranking by mean word frequency

namespace {

CompleteResult ranked_global(const Engine& e, const std::string& q, Index::GlobalRank rank, std::size_t n = 10) {
  CompleteRequest r;
  r.query = q;
  r.n = n;
  r.m = 0;
  r.rank = rank;
  return e.complete(r);
}

// Word frequencies: apple 112, pie 10, tart 1, juice 1001.
void fill_apples(Engine& e) {
  ASSERT_TRUE(e.add_global("apple", 100).ok());
  ASSERT_TRUE(e.add_global("Apple Pie", 10).ok());
  ASSERT_TRUE(e.add_global("apple tart", 1).ok());
  ASSERT_TRUE(e.add_global("apple juice", 1).ok());
  ASSERT_TRUE(e.add_global("juice", 1000).ok());
}

using Rank = Index::GlobalRank;

}  // namespace

TEST_P(EngineTest, RankByAverageWordFrequency) {
  Engine e(alphabet(), cfg());
  fill_apples(e);

  const auto by_freq = ranked_global(e, "apple ", Rank::Frequency);
  ASSERT_TRUE(by_freq.ok());
  EXPECT_EQ(texts(by_freq.global), (std::vector<std::string>{"apple", "apple pie", "apple tart", "apple juice"}));
  for (const auto& s : by_freq.global) EXPECT_FALSE(s.score.has_value());  // no score unless asked

  const auto avg = ranked_global(e, "apple ", Rank::AverageWordFrequency);
  ASSERT_TRUE(avg.ok());
  EXPECT_EQ(texts(avg.global), (std::vector<std::string>{"apple juice", "apple", "apple pie", "apple tart"}));
  ASSERT_EQ(avg.global.size(), 4u);
  EXPECT_DOUBLE_EQ(*avg.global[0].score, 556.5);
  EXPECT_DOUBLE_EQ(*avg.global[1].score, 112.0);
  EXPECT_DOUBLE_EQ(*avg.global[2].score, 61.0);
  EXPECT_DOUBLE_EQ(*avg.global[3].score, 56.5);
  EXPECT_EQ(avg.global[0].freq, 1u);  // the query's own frequency is still reported

  // top-N under the average ranking, partial last word, and the text is normalised first
  EXPECT_EQ(texts(ranked_global(e, "APPLE ", Rank::AverageWordFrequency, 2).global),
            (std::vector<std::string>{"apple juice", "apple"}));
  EXPECT_EQ(texts(ranked_global(e, "app", Rank::AverageWordFrequency, 1).global),
            (std::vector<std::string>{"apple juice"}));
}

TEST_P(EngineTest, RankingDoesNotAffectTheUserList) {
  Engine e(alphabet(), cfg());
  fill_apples(e);
  ASSERT_TRUE(e.add_user(5, "apple crumble", 7).ok());
  ASSERT_TRUE(e.add_user(5, "apple strudel", 3).ok());

  CompleteRequest r;
  r.query = "apple ";
  r.user = 5;
  r.m = 10;
  const auto a = e.complete(r);
  r.rank = Rank::AverageWordFrequency;
  const auto b = e.complete(r);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(texts(a.user), (std::vector<std::string>{"apple crumble", "apple strudel"}));
  EXPECT_EQ(texts(b.user), texts(a.user));
  for (const auto& s : b.user) EXPECT_FALSE(s.score.has_value());
  EXPECT_NE(texts(a.global), texts(b.global));  // while the global list did change
}

TEST_P(EngineTest, RankingFollowsWrites) {
  Engine e(alphabet(), cfg());
  fill_apples(e);
  EXPECT_EQ(texts(ranked_global(e, "apple ", Rank::AverageWordFrequency, 1).global).front(), "apple juice");
  // make "pie" and "apple" much more frequent
  ASSERT_TRUE(e.add_global("apple pie", 2000).ok());  // apple 2112, pie 2010
  EXPECT_EQ(texts(ranked_global(e, "apple ", Rank::AverageWordFrequency, 2).global),
            (std::vector<std::string>{"apple", "apple pie"}));
  // removing the query "apple" lowers the weight of the word "apple"
  const auto top = ranked_global(e, "apple ", Rank::Frequency, 1);
  ASSERT_EQ(top.global.size(), 1u);
  EXPECT_EQ(top.global[0].text, "apple pie");
  const auto apple = ranked_global(e, "apple", Rank::Frequency, 10);  // partial: whole-word match + extensions
  QueryId apple_id = 0;
  for (const auto& s : apple.global)
    if (s.text == "apple") apple_id = s.id;
  ASSERT_NE(apple_id, 0u);
  ASSERT_TRUE(e.remove_global(apple_id).ok());
  const auto after = ranked_global(e, "apple ", Rank::AverageWordFrequency);
  EXPECT_EQ(texts(after.global), (std::vector<std::string>{"apple pie", "apple juice", "apple tart"}));
}

TEST_P(EngineTest, CacheKeepsTheRankingsApart) {
  auto c = cfg();
  c.cache_entries = 100;
  c.cache_ttl_ms = 60000;
  Engine e(alphabet(), c);
  fill_apples(e);
  for (int round = 0; round < 3; ++round) {  // alternate: a cached entry must never answer the other ranking
    EXPECT_EQ(texts(ranked_global(e, "apple ", Rank::Frequency).global).front(), "apple") << round;
    const auto avg = ranked_global(e, "apple ", Rank::AverageWordFrequency);
    EXPECT_EQ(texts(avg.global).front(), "apple juice") << round;
    EXPECT_TRUE(avg.global.front().score.has_value()) << round;  // the cached entry keeps its scores
  }
  EXPECT_GE(e.stats().cache_hits, 4u);
}

TEST_P(EngineTest, RankingSurvivesRestartSnapshotAndOptimize) {
  const auto dump_avg = [](const Engine& e) {
    std::vector<std::string> out;
    for (const char* typed : {"apple ", "app", "apple j", "ju"}) {
      for (const auto& s : ranked_global(e, typed, Rank::AverageWordFrequency, 50).global)
        out.push_back(std::string(typed) + "|" + s.text + "|" + std::to_string(*s.score));
    }
    return out;
  };
  std::vector<std::string> expected;
  {
    Engine e(alphabet(), cfg());
    fill_apples(e);
    ASSERT_TRUE(e.snapshot().ok());
    ASSERT_TRUE(e.add_global("apple sauce", 4).ok());   // after the snapshot: only in the WAL
    ASSERT_TRUE(e.add_global("scratch", 9).ok());
    const auto scratch = ranked_global(e, "scratch", Rank::Frequency, 1);
    ASSERT_EQ(scratch.global.size(), 1u);
    ASSERT_TRUE(e.remove_global(scratch.global[0].id).ok());  // leaves a dead query for optimize
    expected = dump_avg(e);
    ASSERT_FALSE(expected.empty());
  }
  Engine again(alphabet(), cfg());  // snapshot + WAL replay
  EXPECT_EQ(dump_avg(again), expected);
  const auto opt = again.optimize();
  ASSERT_TRUE(opt.ok()) << opt.message;
  EXPECT_EQ(dump_avg(again), expected);  // compaction changes nothing observable
}
