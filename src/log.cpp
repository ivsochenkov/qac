#include "qac/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace qac {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_mutex;

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    default: return "?";
  }
}
}  // namespace

void set_log_level(LogLevel level) { g_level.store(static_cast<int>(level)); }
LogLevel log_level() { return static_cast<LogLevel>(g_level.load()); }

void log(LogLevel level, const std::string& message) {
  if (static_cast<int>(level) < g_level.load()) return;
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fprintf(stderr, "%s [%s] %s\n", ts, level_name(level), message.c_str());
}

}  // namespace qac
