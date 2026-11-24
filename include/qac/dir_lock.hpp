// Single-owner lock of a data directory.
//
// A data directory (snapshot + WAL) must have exactly one owner. The lock is a
// POSIX file lock on <dir>/LOCK, which the kernel drops when the process dies, plus
// an in-process registry, because file locks do not conflict between threads of
// one process.
#pragma once

#include <memory>
#include <string>

namespace boost::interprocess {
class file_lock;
}

namespace qac {

class DirLock {
 public:
  // Creates the directory if needed. Throws std::runtime_error when it is already
  // owned, by this or by another process.
  explicit DirLock(const std::string& dir);
  ~DirLock();
  DirLock(const DirLock&) = delete;
  DirLock& operator=(const DirLock&) = delete;

 private:
  std::string registered_;
  std::unique_ptr<boost::interprocess::file_lock> lock_;
};

}  // namespace qac
