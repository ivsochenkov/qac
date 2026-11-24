// Shared basic types and limits.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qac {

using WordId = std::uint32_t;
using QueryId = std::uint32_t;

// Query ids start at 1; 0 means "no query".
inline constexpr QueryId kNoQuery = 0;

// Hard limits that keep keys small and the trie well-behaved.
struct Limits {
  std::size_t max_words = 32;         // words per query
  std::size_t max_word_letters = 64;  // letters per word (alphabet codes)
  std::size_t max_input_bytes = 1024; // UTF-8 bytes of one query
};

}  // namespace qac
