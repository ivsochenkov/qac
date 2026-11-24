#include "qac/alphabet.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "qac/utf8.hpp"

namespace qac {

namespace {

[[noreturn]] void fail(const std::string& origin, int line,
                       const std::string& msg) {
  throw std::runtime_error(origin + ":" + std::to_string(line) + ": " + msg);
}

std::vector<std::string_view> split_ws(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const std::size_t b = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    if (i > b) out.push_back(s.substr(b, i - b));
  }
  return out;
}

char32_t parse_token(std::string_view tok, const std::string& origin, int line) {
  if (tok.size() >= 3 && (tok[0] == 'U' || tok[0] == 'u') && tok[1] == '+') {
    const std::string hex(tok.substr(2));
    if (hex.empty() || hex.size() > 6) fail(origin, line, "bad code point '" + std::string(tok) + "'");
    char* end = nullptr;
    const unsigned long v = std::strtoul(hex.c_str(), &end, 16);
    if (*end != '\0' || v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
      fail(origin, line, "bad code point '" + std::string(tok) + "'");
    return static_cast<char32_t>(v);
  }
  std::u32string cps;
  if (!utf8::decode(tok, cps)) fail(origin, line, "invalid UTF-8");
  if (cps.size() != 1)
    fail(origin, line,
         "each entry must be a single character or U+XXXX, got '" + std::string(tok) + "'");
  if (cps[0] == 0) fail(origin, line, "NUL is not allowed");
  return cps[0];
}

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

void fnv_u32(std::uint64_t& h, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (v >> (8 * i)) & 0xFFu;
    h *= kFnvPrime;
  }
}

}  // namespace

bool Alphabet::is_space(char32_t cp) noexcept {
  return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool Alphabet::is_separator(char32_t cp) const noexcept {
  return is_space(cp) ||
         std::binary_search(separators_.begin(), separators_.end(), cp);
}

Alphabet Alphabet::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open alphabet file: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_string(ss.str(), path);
}

Alphabet Alphabet::from_string(std::string_view text, const std::string& origin) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.remove_prefix(3);

  std::vector<std::vector<char32_t>> classes;
  std::vector<char32_t> seps;

  int lineno = 0;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++lineno;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    const auto toks = split_ws(line);
    if (toks.empty() || toks[0][0] == '#') continue;

    if (toks[0][0] == '@') {
      if (toks[0] != "@separators") fail(origin, lineno, "unknown directive '" + std::string(toks[0]) + "'");
      for (std::size_t i = 1; i < toks.size(); ++i) {
        if (toks[i][0] == '#') break;  // trailing comment
        seps.push_back(parse_token(toks[i], origin, lineno));
      }
      continue;
    }

    std::vector<char32_t> cls;
    for (const auto& t : toks) {
      if (t[0] == '#' && t.size() == 1) break;  // trailing comment marker
      cls.push_back(parse_token(t, origin, lineno));
    }
    if (!cls.empty()) classes.push_back(std::move(cls));
  }

  if (classes.empty()) throw std::runtime_error(origin + ": alphabet has no letters");
  if (classes.size() > kMaxLetters)
    throw std::runtime_error(origin + ": too many letters (" + std::to_string(classes.size()) +
                             "), at most " + std::to_string(kMaxLetters) + " are supported");

  Alphabet a;
  std::map<char32_t, unsigned char> seen;
  for (std::size_t i = 0; i < classes.size(); ++i) {
    const auto code = static_cast<unsigned char>(i + 1);
    a.canonical_.push_back(classes[i][0]);
    for (char32_t cp : classes[i]) {
      if (is_space(cp))
        throw std::runtime_error(origin + ": whitespace cannot be a letter (U+" +
                                 std::to_string(static_cast<unsigned>(cp)) + ")");
      if (!seen.emplace(cp, code).second)
        throw std::runtime_error(origin + ": character U+" +
                                 std::to_string(static_cast<unsigned>(cp)) +
                                 " is listed twice");
      if (cp < a.low_.size())
        a.low_[cp] = code;
      else
        a.high_[cp] = code;
    }
  }

  std::sort(seps.begin(), seps.end());
  seps.erase(std::unique(seps.begin(), seps.end()), seps.end());
  for (char32_t s : seps)
    if (seen.count(s))
      throw std::runtime_error(origin + ": character U+" +
                               std::to_string(static_cast<unsigned>(s)) +
                               " is both a letter and a separator");
  a.separators_ = std::move(seps);

  std::uint64_t h = kFnvOffset;
  for (const auto& [cp, code] : seen) {
    fnv_u32(h, static_cast<std::uint32_t>(cp));
    fnv_u32(h, code);
  }
  fnv_u32(h, 0xFFFFFFFFu);
  for (char32_t s : a.separators_) fnv_u32(h, static_cast<std::uint32_t>(s));
  for (std::size_t i = 1; i < a.canonical_.size(); ++i)
    fnv_u32(h, static_cast<std::uint32_t>(a.canonical_[i]));
  a.fingerprint_ = h;
  return a;
}

bool Alphabet::encode_word(std::u32string_view word, std::string& out) const {
  out.clear();
  out.reserve(word.size());
  for (char32_t cp : word) {
    const unsigned char c = code(cp);
    if (c == 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<char>(c));
  }
  return true;
}

std::string Alphabet::decode_word(std::string_view encoded) const {
  std::string out;
  out.reserve(encoded.size() * 2);
  for (char ch : encoded) {
    const auto c = static_cast<unsigned char>(ch);
    utf8::append(c < canonical_.size() ? canonical_[c] : U'?', out);
  }
  return out;
}

}  // namespace qac
