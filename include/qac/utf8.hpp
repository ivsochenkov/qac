// Strict UTF-8 <-> UTF-32 conversion (no dependencies).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace qac::utf8 {

// Decodes `in` into code points. Returns false on malformed input: truncated
// sequences, stray continuation bytes, overlong forms, surrogates, > U+10FFFF.
inline bool decode(std::string_view in, std::u32string& out) {
  out.clear();
  out.reserve(in.size());
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  const std::size_t n = in.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    char32_t cp;
    std::size_t len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
      cp = c & 0x1Fu;
      len = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
      cp = c & 0x0Fu;
      len = 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
      cp = c & 0x07u;
      len = 4;
    } else {
      return false;
    }
    if (i + len > n) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const unsigned char cc = p[i + k];
      if ((cc & 0xC0u) != 0x80u) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (len == 3 && cp < 0x800) return false;
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    out.push_back(cp);
    i += len;
  }
  return true;
}

inline void append(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

inline std::string encode(std::u32string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char32_t cp : s) append(cp, out);
  return out;
}

}  // namespace qac::utf8
