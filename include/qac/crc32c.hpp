// CRC-32C (Castagnoli), software slice-by-8.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qac {

// Continues a CRC: crc32c_extend(crc32c_extend(0, a), b) == crc32c(a ++ b).
std::uint32_t crc32c_extend(std::uint32_t crc, const void* data, std::size_t n);

inline std::uint32_t crc32c(const void* data, std::size_t n) {
  return crc32c_extend(0, data, n);
}

}  // namespace qac
