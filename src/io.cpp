#include "qac/io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

#include "qac/crc32c.hpp"

namespace qac {

Fd& Fd::operator=(Fd&& o) noexcept {
  if (this != &o) {
    reset();
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}

void Fd::reset() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

void throw_errno(const std::string& what) {
  const int e = errno;
  throw std::runtime_error(what + ": " + std::strerror(e));
}

void write_all(int fd, const void* data, std::size_t n) {
  const char* p = static_cast<const char*>(data);
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw_errno("write");
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
}

void fsync_fd(int fd) {
  while (::fsync(fd) != 0) {
    if (errno == EINTR) continue;
    throw_errno("fsync");
  }
}

void fsync_directory(const std::string& dir) {
  Fd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY));
  if (!fd.valid()) throw_errno("open directory " + dir);
  fsync_fd(fd.get());
}

// ---------------------------------------------------------------- CrcOutBuf

CrcOutBuf::CrcOutBuf(int fd, std::size_t buffer_bytes) : fd_(fd), cap_(buffer_bytes) {
  buf_.reserve(cap_);
}

void CrcOutBuf::consume(const char* s, std::size_t n) {
  crc_ = crc32c_extend(crc_, s, n);
  bytes_ += n;
  write_all(fd_, s, n);
}

void CrcOutBuf::flush_buffer() {
  if (buf_.empty()) return;
  consume(buf_.data(), buf_.size());
  buf_.clear();
}

std::streamsize CrcOutBuf::xsputn(const char* s, std::streamsize n) {
  const auto total = static_cast<std::size_t>(n);
  if (total >= cap_) {  // large block: skip the copy
    flush_buffer();
    consume(s, total);
    return n;
  }
  if (buf_.size() + total > cap_) flush_buffer();
  buf_.insert(buf_.end(), s, s + total);
  return n;
}

CrcOutBuf::int_type CrcOutBuf::overflow(int_type ch) {
  if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
  const char c = traits_type::to_char_type(ch);
  xsputn(&c, 1);
  return ch;
}

int CrcOutBuf::sync() {
  flush_buffer();
  return 0;
}

void CrcOutBuf::write_crc_trailer() {
  flush_buffer();
  const std::uint32_t c = crc_;
  write_all(fd_, &c, sizeof c);  // deliberately not part of the checksum
}

// ---------------------------------------------------------------- CrcInBuf

CrcInBuf::CrcInBuf(int fd, std::size_t buffer_bytes) : fd_(fd), buf_(buffer_bytes) {}

bool CrcInBuf::refill() {
  for (;;) {
    const ssize_t r = ::read(fd_, buf_.data(), buf_.size());
    if (r < 0) {
      if (errno == EINTR) continue;
      throw_errno("read");
    }
    pos_ = 0;
    end_ = static_cast<std::size_t>(r);
    return r > 0;
  }
}

std::size_t CrcInBuf::take(char* out, std::size_t n, bool checksum) {
  std::size_t got = 0;
  while (got < n) {
    if (pos_ == end_ && !refill()) break;
    const std::size_t k = std::min(n - got, end_ - pos_);
    std::memcpy(out + got, buf_.data() + pos_, k);
    if (checksum) crc_ = crc32c_extend(crc_, out + got, k);
    pos_ += k;
    got += k;
  }
  if (checksum) bytes_ += got;
  return got;
}

std::streamsize CrcInBuf::xsgetn(char* s, std::streamsize n) {
  return static_cast<std::streamsize>(take(s, static_cast<std::size_t>(n), true));
}

CrcInBuf::int_type CrcInBuf::underflow() {
  // Single-character access is not used by the archives; support it anyway
  // without consuming (the byte is not checksummed until read through xsgetn).
  if (pos_ == end_ && !refill()) return traits_type::eof();
  return traits_type::to_int_type(buf_[pos_]);
}

bool CrcInBuf::read_crc_trailer(std::uint32_t& stored) {
  return take(reinterpret_cast<char*>(&stored), sizeof stored, false) == sizeof stored;
}

}  // namespace qac
