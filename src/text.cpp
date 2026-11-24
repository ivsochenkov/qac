#include "qac/text.hpp"

#include <boost/tokenizer.hpp>

#include "qac/utf8.hpp"

namespace qac {

const char* to_string(ParseError e) {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::Empty: return "empty query";
    case ParseError::InvalidUtf8: return "invalid UTF-8";
    case ParseError::UnknownCharacter: return "character outside the alphabet";
    case ParseError::TooLong: return "query or word too long";
    case ParseError::TooManyWords: return "too many words";
  }
  return "?";
}

namespace {

// Boost.Tokenizer TokenizerFunction: splits a UTF-32 string on whitespace and
// the alphabet's declared separators.
class SeparatorFunc {
 public:
  explicit SeparatorFunc(const Alphabet* a = nullptr) : alphabet_(a) {}

  void reset() {}

  template <typename Iterator, typename Token>
  bool operator()(Iterator& next, Iterator end, Token& tok) const {
    while (next != end && alphabet_->is_separator(*next)) ++next;
    if (next == end) return false;
    tok.clear();
    while (next != end && !alphabet_->is_separator(*next)) {
      tok.push_back(*next);
      ++next;
    }
    return true;
  }

 private:
  const Alphabet* alphabet_;
};

using U32Tokenizer =
    boost::tokenizer<SeparatorFunc, std::u32string::const_iterator, std::u32string>;

}  // namespace

ParseError TextProcessor::tokenize(std::string_view utf8, std::vector<std::string>& words,
                                   bool& ends_with_separator, std::string* detail) const {
  words.clear();
  ends_with_separator = true;
  if (utf8.size() > limits_.max_input_bytes) {
    if (detail) *detail = "input longer than " + std::to_string(limits_.max_input_bytes) + " bytes";
    return ParseError::TooLong;
  }
  std::u32string text;
  if (!utf8::decode(utf8, text)) return ParseError::InvalidUtf8;
  if (text.empty()) return ParseError::None;
  ends_with_separator = alphabet_->is_separator(text.back());

  const U32Tokenizer tokens(text.cbegin(), text.cend(), SeparatorFunc(alphabet_));
  std::string encoded;
  for (const std::u32string& tok : tokens) {
    if (tok.size() > limits_.max_word_letters) {
      if (detail) *detail = "word longer than " + std::to_string(limits_.max_word_letters) + " letters";
      return ParseError::TooLong;
    }
    if (!alphabet_->encode_word(tok, encoded)) {
      if (detail) {
        for (char32_t cp : tok) {
          if (alphabet_->code(cp) == 0) {
            std::string ch;
            utf8::append(cp, ch);
            *detail = "character '" + ch + "' is not in the alphabet";
            break;
          }
        }
      }
      return ParseError::UnknownCharacter;
    }
    words.push_back(encoded);
    if (words.size() > limits_.max_words) {
      if (detail) *detail = "more than " + std::to_string(limits_.max_words) + " words";
      return ParseError::TooManyWords;
    }
  }
  return ParseError::None;
}

ParseError TextProcessor::parse_query(std::string_view utf8, std::vector<std::string>& words,
                                      std::string* detail) const {
  bool ends_sep = true;
  const ParseError e = tokenize(utf8, words, ends_sep, detail);
  if (e != ParseError::None) {
    words.clear();
    return e;
  }
  if (words.empty()) return ParseError::Empty;
  return ParseError::None;
}

ParseError TextProcessor::parse_prefix(std::string_view utf8, std::vector<std::string>& complete,
                                       std::string& partial, std::string* detail) const {
  partial.clear();
  bool ends_sep = true;
  const ParseError e = tokenize(utf8, complete, ends_sep, detail);
  if (e != ParseError::None) {
    complete.clear();
    return e;
  }
  if (!ends_sep && !complete.empty()) {
    partial = std::move(complete.back());
    complete.pop_back();
  }
  return ParseError::None;
}

std::string TextProcessor::render(const std::vector<std::string>& words) const {
  std::string out;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i) out.push_back(' ');
    out += alphabet_->decode_word(words[i]);
  }
  return out;
}

}  // namespace qac
