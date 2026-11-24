// Helpers shared by the tests.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "qac/index.hpp"

namespace qac::test {

inline std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

// For Index-level tests words are plain ASCII bytes (the Index does not care
// about the alphabet, it only sees byte strings).
struct Q {
  std::vector<std::string> words;
  explicit Q(const std::string& text) : words(split(text)) {}
  std::vector<std::string_view> views() const { return {words.begin(), words.end()}; }
};

inline Index::AddOutcome add(Index& idx, const std::string& text, std::uint32_t n = 1) {
  return idx.add_global(Q(text).views(), n);
}

inline Index::AddOutcome add_user(Index& idx, std::uint32_t user, const std::string& text,
                                  std::uint32_t n = 1, std::uint32_t ts = 0,
                                  bool also_global = false) {
  return idx.add_user(user, Q(text).views(), n, ts, also_global);
}

inline std::string text_of(const Index& idx, QueryId id) {
  std::size_t n = 0;
  const std::uint32_t* w = idx.query_words(id, n);
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    if (i) out += ' ';
    out += std::string(idx.word(w[i]));
  }
  return out;
}

// Looks up the words of the complete part and returns {ids, ok}.
inline bool resolve(const Index& idx, const std::vector<std::string>& words,
                    std::vector<WordId>& ids) {
  ids.clear();
  for (const auto& w : words) {
    const auto id = idx.find_word(w);
    if (!id) return false;
    ids.push_back(*id);
  }
  return true;
}

// Query like the engine does: "new yo" -> complete ["new"], partial "yo";
// a trailing space means everything is complete.
inline std::vector<std::string> global_texts(const Index& idx, const std::string& typed,
                                             std::size_t n = 10,
                                             Index::ScanStats* stats_out = nullptr) {
  auto words = split(typed);
  std::string partial;
  if (!typed.empty() && typed.back() != ' ' && !words.empty()) {
    partial = words.back();
    words.pop_back();
  }
  std::vector<WordId> ids;
  std::vector<std::string> out;
  if (!resolve(idx, words, ids)) return out;
  Index::ScanStats st;
  for (const auto& h : idx.top_global(ids, partial, n, Index::ScanLimits{}, st))
    out.push_back(text_of(idx, h.id));
  if (stats_out) *stats_out = st;
  return out;
}

inline std::vector<std::string> user_texts(const Index& idx, std::uint32_t user,
                                           const std::string& typed, std::size_t n = 10) {
  auto words = split(typed);
  std::string partial;
  if (!typed.empty() && typed.back() != ' ' && !words.empty()) {
    partial = words.back();
    words.pop_back();
  }
  std::vector<WordId> ids;
  std::vector<std::string> out;
  if (!resolve(idx, words, ids)) return out;
  Index::ScanStats st;
  for (const auto& h : idx.top_user(user, ids, partial, n, Index::ScanLimits{}, st))
    out.push_back(text_of(idx, h.id));
  return out;
}

// A scratch directory removed on destruction.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "qac-test-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) std::abort();
    path_ = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return path_; }
  std::string str() const { return path_.string(); }
  std::string sub(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace qac::test
