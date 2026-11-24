// Low-level file helpers: checksummed stream buffers over POSIX file descriptors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <streambuf>
#include <string>
#include <vector>

namespace qac {

// RAII file descriptor.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  Fd& operator=(Fd&& o) noexcept;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(); }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  int release() noexcept {
    const int f = fd_;
    fd_ = -1;
    return f;
  }
  void reset() noexcept;

 private:
  int fd_ = -1;
};

// Throws std::runtime_error("<what>: <strerror>") built from errno.
[[noreturn]] void throw_errno(const std::string& what);

void write_all(int fd, const void* data, std::size_t n);
void fsync_fd(int fd);
void fsync_directory(const std::string& dir);

// std::streambuf that writes to a descriptor and maintains a CRC-32C of
// everything written through it.
class CrcOutBuf : public std::streambuf {
 public:
  explicit CrcOutBuf(int fd, std::size_t buffer_bytes = 1u << 20);

  void flush_buffer();
  std::uint32_t crc() const noexcept { return crc_; }
  std::uint64_t bytes() const noexcept { return bytes_; }
  // Flushes, then appends the CRC of everything so far as an unchecksummed trailer.
  void write_crc_trailer();

 protected:
  int_type overflow(int_type ch) override;
  std::streamsize xsputn(const char* s, std::streamsize n) override;
  int sync() override;

 private:
  void consume(const char* s, std::size_t n);

  int fd_;
  std::size_t cap_;
  std::vector<char> buf_;
  std::uint32_t crc_ = 0;
  std::uint64_t bytes_ = 0;
};

// std::streambuf that reads from a descriptor and maintains a CRC-32C of the
// bytes handed to the consumer. Only bulk reads (sgetn) are supported, which is
// what cereal's binary archives use.
class CrcInBuf : public std::streambuf {
 public:
  explicit CrcInBuf(int fd, std::size_t buffer_bytes = 1u << 20);

  std::uint32_t crc() const noexcept { return crc_; }
  std::uint64_t bytes() const noexcept { return bytes_; }
  // Reads the 4-byte trailer written by CrcOutBuf::write_crc_trailer.
  bool read_crc_trailer(std::uint32_t& stored);

 protected:
  std::streamsize xsgetn(char* s, std::streamsize n) override;
  int_type underflow() override;

 private:
  bool refill();
  std::size_t take(char* out, std::size_t n, bool checksum);

  int fd_;
  std::vector<char> buf_;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  std::uint32_t crc_ = 0;
  std::uint64_t bytes_ = 0;
};

}  // namespace qac
