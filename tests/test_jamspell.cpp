// Integration tests for the JamSpell-backed SpellFixer. They train a tiny model
// through the fork's own public API from the data bundled with the fork, so they
// need the ru_RU.UTF-8 locale that the fork requires; without it they are skipped.
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "qac/engine.hpp"
#include "qac/jamspell_fixer.hpp"
#include "test_util.hpp"

using namespace qac;
using namespace qac::test;

namespace {

// The model is trained at build time (see tests/CMakeLists.txt). It is absent on hosts
// without the ru_RU.UTF-8 locale that the fork needs; the tests are skipped there.
class JamSpellTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::ifstream(model()))
      GTEST_SKIP() << "no JamSpell test model (ru_RU.UTF-8 locale not installed at build time?)";
  }
  static std::string model() { return QAC_JAMSPELL_MODEL; }
};

}  // namespace

TEST_F(JamSpellTest, CorrectsMisspelledWords) {
  const auto fixer = JamSpellFixer::load(model());
  EXPECT_EQ(fixer->fix("teh hound of the baskervilles"), "the found of the baskervilles");
  EXPECT_EQ(fixer->fix("i am sherlok holmes"), "i am sherlock holmes");
  EXPECT_EQ(fixer->fix("i am sherlock holmes"), "i am sherlock holmes");
}

TEST_F(JamSpellTest, EdgeCases) {
  const auto fixer = JamSpellFixer::load(model());
  EXPECT_EQ(fixer->fix(""), "");
  EXPECT_EQ(fixer->fix("\xC3\x28 broken"), "\xC3\x28 broken");  // invalid UTF-8: unchanged
  EXPECT_EQ(fixer->fix("ПРИВЕТ мир"), "ПРИВЕТ мир");            // Cyrillic round-trips
  EXPECT_EQ(fixer->fix("ß ü é"), fixer->fix("ß ü é"));          // other scripts: stable, no throw
}

TEST_F(JamSpellTest, LoadFailureIsReportedWithAHint) {
  try {
    JamSpellFixer::load(model() + ".does-not-exist");
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    const std::string msg = e.what();
    EXPECT_NE(msg.find(".does-not-exist"), std::string::npos) << msg;
    EXPECT_NE(msg.find("ru_RU.UTF-8"), std::string::npos) << msg;
  }
}

// The engine calls fix() from many request threads at once.
TEST_F(JamSpellTest, ConcurrentCallsAreConsistent) {
  const auto fixer = JamSpellFixer::load(model());
  const std::vector<std::string> inputs = {"teh hound of the baskervilles", "i am sherlok holmes",
                                           "dr watson and mr holmes", "ПРИВЕТ мир", "the  hound"};
  std::vector<std::string> expected;
  for (const auto& in : inputs) expected.push_back(fixer->fix(in));

  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&, t] {
      for (int i = 0; i < 300; ++i) {
        const std::size_t k = static_cast<std::size_t>(i + t) % inputs.size();
        if (fixer->fix(inputs[k]) != expected[k]) ++mismatches;
      }
    });
  for (auto& th : threads) th.join();
  EXPECT_EQ(mismatches.load(), 0);
}

TEST_F(JamSpellTest, EngineUsesTheRealModel) {
  static const Alphabet alphabet = Alphabet::load(std::string(QAC_DATA_DIR) + "/alphabet.txt");
  TempDir dir;
  EngineConfig cfg;
  cfg.data_dir = dir.sub("data");
  cfg.fsync = FsyncPolicy::Never;
  cfg.snapshot_interval_s = 0;
  cfg.snapshot_wal_bytes = 0;
  cfg.cache_entries = 0;
  Engine e(alphabet, cfg);
  e.add_global("i am sherlock holmes", 5);

  CompleteRequest req;
  req.query = "i am sherlok holm";
  req.correct = true;
  EXPECT_EQ(e.complete(req).status, Status::Unavailable);  // no model attached yet

  e.set_spell_fixer(std::shared_ptr<const SpellFixer>(JamSpellFixer::load(model())));
  auto r = e.complete(req);
  ASSERT_TRUE(r.ok()) << r.message;
  ASSERT_TRUE(r.corrected.has_value());
  EXPECT_EQ(*r.corrected, "i am sherlock holm");
  ASSERT_EQ(r.global.size(), 1u);
  EXPECT_EQ(r.global[0].text, "i am sherlock holmes");

  req.correct = false;  // correction is off unless asked for
  r = e.complete(req);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.corrected.has_value());
  EXPECT_TRUE(r.global.empty());
}
