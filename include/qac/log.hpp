// Minimal thread-safe stderr logger.
#pragma once

#include <string>

namespace qac {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void set_log_level(LogLevel level);
LogLevel log_level();
void log(LogLevel level, const std::string& message);

inline void log_debug(const std::string& m) { log(LogLevel::Debug, m); }
inline void log_info(const std::string& m) { log(LogLevel::Info, m); }
inline void log_warn(const std::string& m) { log(LogLevel::Warn, m); }
inline void log_error(const std::string& m) { log(LogLevel::Error, m); }

}  // namespace qac
