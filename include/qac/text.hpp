// Query text -> alphabet-encoded words, using Boost.Tokenizer.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "qac/alphabet.hpp"
#include "qac/common.hpp"

namespace qac {

enum class ParseError {
  None,
  Empty,             // add: no tokens at all
  InvalidUtf8,
  UnknownCharacter,  // a character outside the alphabet and the separators
  TooLong,           // input / word too long
  TooManyWords,
};

const char* to_string(ParseError e);

class TextProcessor {
 public:
  explicit TextProcessor(const Alphabet& alphabet, Limits limits = {})
      : alphabet_(&alphabet), limits_(limits) {}

  const Alphabet& alphabet() const noexcept { return *alphabet_; }
  const Limits& limits() const noexcept { return limits_; }

  // A query to be stored: every token is a complete word. `words` receives the
  // alphabet-encoded tokens. Malformed queries are rejected as a whole.
  ParseError parse_query(std::string_view utf8, std::vector<std::string>& words,
                         std::string* detail = nullptr) const;

  // Text typed so far. If it does not end with a separator, the last token is
  // still being typed and is returned in `partial` instead of `complete`.
  // Empty / separator-only text is valid and yields nothing.
  ParseError parse_prefix(std::string_view utf8, std::vector<std::string>& complete,
                          std::string& partial, std::string* detail = nullptr) const;

  // Encoded words -> UTF-8 text joined by single spaces.
  std::string render(const std::vector<std::string>& words) const;

 private:
  ParseError tokenize(std::string_view utf8, std::vector<std::string>& words,
                      bool& ends_with_separator, std::string* detail) const;

  const Alphabet* alphabet_;
  Limits limits_;
};

}  // namespace qac
