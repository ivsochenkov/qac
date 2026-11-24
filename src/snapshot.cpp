#include "qac/snapshot.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cereal/archives/binary.hpp>

#include "qac/crc32c.hpp"
#include "qac/io.hpp"

namespace qac {

namespace {
constexpr std::uint64_t kMagic = 0x31504E5343415100ull;  // "\0QACSNP1" little-endian tag
constexpr std::uint32_t kFileFormat = 1;

// Verifies the CRC trailer over the whole file *before* any length field in it
// is trusted: a damaged snapshot must be rejected up front instead of making the
// deserializer allocate memory according to garbage.
void verify_checksum(int fd, const std::string& path) {
  struct stat st{};
  if (::fstat(fd, &st) != 0) throw_errno("cannot stat snapshot " + path);
  const auto size = static_cast<std::uint64_t>(st.st_size);
  if (size < sizeof(std::uint32_t) + 8)
    throw std::runtime_error("snapshot " + path + " is truncated");

  std::vector<char> buf(4u << 20);
  std::uint32_t crc = 0;
  std::uint64_t left = size - sizeof(std::uint32_t);
  std::uint64_t off = 0;
  while (left > 0) {
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
    const ssize_t r = ::pread(fd, buf.data(), want, static_cast<off_t>(off));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw_errno("cannot read snapshot " + path);
    }
    if (r == 0) throw std::runtime_error("snapshot " + path + " is truncated");
    crc = crc32c_extend(crc, buf.data(), static_cast<std::size_t>(r));
    off += static_cast<std::uint64_t>(r);
    left -= static_cast<std::uint64_t>(r);
  }
  std::uint32_t stored = 0;
  if (::pread(fd, &stored, sizeof stored, static_cast<off_t>(size - sizeof stored)) !=
      static_cast<ssize_t>(sizeof stored))
    throw std::runtime_error("snapshot " + path + " is truncated (missing checksum)");
  if (stored != crc)
    throw std::runtime_error("snapshot " + path + " is corrupt (checksum mismatch)");
}
}  // namespace

SnapshotInfo write_snapshot(const std::string& path, const Index& index, std::uint64_t last_seq,
                            std::uint64_t alphabet_fingerprint) {
  Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  if (!fd.valid()) throw_errno("cannot create snapshot " + path);

  SnapshotInfo info;
  info.alphabet_fingerprint = alphabet_fingerprint;
  info.last_seq = last_seq;
  info.created_unix = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  CrcOutBuf buf(fd.get());
  std::ostream os(&buf);
  {
    cereal::BinaryOutputArchive ar(os);
    const std::uint64_t magic = kMagic;
    const std::uint32_t format = kFileFormat;
    ar(magic, format, info.alphabet_fingerprint, info.last_seq, info.created_unix);
  }
  index.save(os);
  os.flush();
  if (!os) throw std::runtime_error("failed to write snapshot " + path);
  buf.write_crc_trailer();
  info.file_bytes = buf.bytes() + sizeof(std::uint32_t);
  fsync_fd(fd.get());
  return info;
}

LoadedSnapshot read_snapshot(const std::string& path) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd.valid()) throw_errno("cannot open snapshot " + path);

  verify_checksum(fd.get(), path);

  CrcInBuf buf(fd.get());
  std::istream is(&buf);

  LoadedSnapshot out;
  try {
    std::uint64_t magic = 0;
    std::uint32_t format = 0;
    {
      cereal::BinaryInputArchive ar(is);
      ar(magic, format);
      if (magic != kMagic) throw std::runtime_error("not a snapshot file");
      if (format != kFileFormat)
        throw std::runtime_error("unsupported snapshot format " + std::to_string(format));
      ar(out.info.alphabet_fingerprint, out.info.last_seq, out.info.created_unix);
    }
    out.index = Index::load(is);
  } catch (const std::exception& e) {
    throw std::runtime_error("cannot read snapshot " + path + ": " + e.what());
  }

  std::uint32_t stored = 0;
  const std::uint32_t computed = buf.crc();
  if (!buf.read_crc_trailer(stored))
    throw std::runtime_error("snapshot " + path + " is truncated (missing checksum)");
  if (stored != computed)
    throw std::runtime_error("snapshot " + path + " is corrupt (checksum mismatch)");

  struct stat st{};
  if (::fstat(fd.get(), &st) == 0) out.info.file_bytes = static_cast<std::uint64_t>(st.st_size);
  return out;
}

}  // namespace qac
