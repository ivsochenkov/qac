#include "qac/crc32c.hpp"

#include <cstring>

namespace qac {

namespace {

struct Tables {
  std::uint32_t t[8][256];
  Tables() {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
      t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = t[0][i];
      for (int j = 1; j < 8; ++j) {
        c = t[0][c & 0xFFu] ^ (c >> 8);
        t[j][i] = c;
      }
    }
  }
};

const Tables& tables() {
  static const Tables instance;
  return instance;
}

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t crc, const void* data, std::size_t n) {
  const Tables& T = tables();
  const auto* p = static_cast<const unsigned char*>(data);
  std::uint32_t c = ~crc;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  while (n >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    v ^= c;
    c = T.t[7][v & 0xFF] ^ T.t[6][(v >> 8) & 0xFF] ^ T.t[5][(v >> 16) & 0xFF] ^
        T.t[4][(v >> 24) & 0xFF] ^ T.t[3][(v >> 32) & 0xFF] ^
        T.t[2][(v >> 40) & 0xFF] ^ T.t[1][(v >> 48) & 0xFF] ^
        T.t[0][(v >> 56) & 0xFF];
    p += 8;
    n -= 8;
  }
#endif
  while (n--) c = T.t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
  return ~c;
}

}  // namespace qac
