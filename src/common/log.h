// common/log.h
#pragma once
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline const char *LevelTag(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  }
  return "?";
}

inline std::atomic<int> &MinLogLevel() {
  static std::atomic<int> level{static_cast<int>(LogLevel::Info)};
  return level;
}

inline void SetMinLogLevel(LogLevel level) {
  MinLogLevel().store(static_cast<int>(level));
}

// Accepts debug|info|warn|error; returns false for anything else.
inline bool ParseLogLevel(std::string_view s, LogLevel &out) {
  if (s == "debug")
    out = LogLevel::Debug;
  else if (s == "info")
    out = LogLevel::Info;
  else if (s == "warn")
    out = LogLevel::Warn;
  else if (s == "error")
    out = LogLevel::Error;
  else
    return false;
  return true;
}

inline void Log(LogLevel level, const std::string &component,
                const std::string &message) {
  if (static_cast<int>(level) < MinLogLevel().load(std::memory_order_relaxed))
    return;
  static std::mutex mu; // keep lines from different threads intact
  std::lock_guard<std::mutex> lock(mu);
  std::ostream &out = (level == LogLevel::Error || level == LogLevel::Warn)
                          ? std::cerr
                          : std::cout;
  out << "[" << LevelTag(level) << "][" << component << "]: " << message << "\n"
      << std::flush;
}
