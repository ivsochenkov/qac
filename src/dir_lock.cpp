#include "qac/dir_lock.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>

#include <boost/interprocess/sync/file_lock.hpp>

namespace qac {

namespace fs = std::filesystem;

namespace {
std::mutex g_mu;
std::set<std::string> g_open;  // canonical paths of the directories owned by this process
}  // namespace

DirLock::DirLock(const std::string& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw std::runtime_error("cannot create data directory " + dir + ": " + ec.message());

  const std::string canonical = fs::weakly_canonical(dir, ec).string();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_open.insert(canonical).second)
      throw std::runtime_error("data directory " + dir + " is already open in this process");
  }
  registered_ = canonical;
  try {
    const std::string path = (fs::path(dir) / "LOCK").string();
    { std::ofstream touch(path, std::ios::app); }
    lock_ = std::make_unique<boost::interprocess::file_lock>(path.c_str());
    if (!lock_->try_lock())
      throw std::runtime_error("data directory " + dir + " is in use by another process");
  } catch (...) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_open.erase(registered_);
    throw;
  }
}

DirLock::~DirLock() {
  lock_.reset();  // unlock before the directory becomes available to this process again
  if (!registered_.empty()) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_open.erase(registered_);
  }
}

}  // namespace qac
