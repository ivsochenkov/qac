// Writer-preferring shared mutex.
//
// pthread/std::shared_mutex on Linux prefers readers, which lets a steady
// stream of completion requests starve index writers. This tiny lock blocks new
// readers as soon as a writer is waiting. It models SharedMutex, so it works
// with std::shared_lock and std::unique_lock.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>  // std::shared_lock, for users of RWLock

namespace qac {

class RWLock {
 public:
  void lock_shared() {
    std::unique_lock<std::mutex> l(m_);
    readers_cv_.wait(l, [this] { return !writer_ && writers_waiting_ == 0; });
    ++readers_;
  }

  void unlock_shared() {
    std::unique_lock<std::mutex> l(m_);
    if (--readers_ == 0 && writers_waiting_ > 0) writer_cv_.notify_one();
  }

  void lock() {
    std::unique_lock<std::mutex> l(m_);
    ++writers_waiting_;
    writer_cv_.wait(l, [this] { return !writer_ && readers_ == 0; });
    --writers_waiting_;
    writer_ = true;
  }

  void unlock() {
    std::unique_lock<std::mutex> l(m_);
    writer_ = false;
    if (writers_waiting_ > 0) {
      writer_cv_.notify_one();
    } else {
      readers_cv_.notify_all();
    }
  }

 private:
  std::mutex m_;
  std::condition_variable readers_cv_;
  std::condition_variable writer_cv_;
  std::uint32_t readers_ = 0;
  std::uint32_t writers_waiting_ = 0;
  bool writer_ = false;
};

}  // namespace qac
