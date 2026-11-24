// Letter alphabet used to encode words as byte strings.
//
// Every letter class gets a one-byte code in 1..255 (0 is reserved, so a word
// never contains NUL). A class lists all code points that count as the same
// letter, which gives case folding (and optional accent folding) for free:
//
//     # comment
//     @separators  . , ; : ! ? ( ) " ' - / U+00A0     <- extra token separators
//     a A                                              <- letter class "a"
//     ß ẞ                                              <- first entry = canonical form
//     е Е ё Ё                                          <- folds ё into е
//     0
//
// Whitespace always separates tokens. Any other character that is neither a
// letter nor a declared separator makes a query malformed.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qac {

class Alphabet {
 public:
  static constexpr std::size_t kMaxLetters = 255;

  // Throws std::runtime_error with "file:line: reason" on any format error.
  static Alphabet load(const std::string& path);
  static Alphabet from_string(std::string_view text,
                              const std::string& origin = "<string>");

  std::size_t size() const noexcept { return canonical_.size() - 1; }

  // Code in 1..size() or 0 when `cp` is not a letter.
  unsigned char code(char32_t cp) const noexcept {
    if (cp < low_.size()) return low_[cp];
    const auto it = high_.find(cp);
    return it == high_.end() ? 0 : it->second;
  }

  bool is_separator(char32_t cp) const noexcept;

  // Encodes one token. Returns false if any code point is not a letter.
  bool encode_word(std::u32string_view word, std::string& out) const;

  // Decodes an encoded word to UTF-8 using the canonical form of each letter.
  std::string decode_word(std::string_view encoded) const;

  // Hash of the whole mapping + separators; stored in snapshots and WAL
  // segments so data is never read with a different alphabet.
  std::uint64_t fingerprint() const noexcept { return fingerprint_; }

  static bool is_space(char32_t cp) noexcept;

 private:
  std::array<unsigned char, 0x800> low_{};
  std::unordered_map<char32_t, unsigned char> high_;
  std::vector<char32_t> canonical_{0};  // index = code
  std::vector<char32_t> separators_;    // sorted
  std::uint64_t fingerprint_ = 0;
};

}  // namespace qac
