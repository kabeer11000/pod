// common/log.h
#pragma once
#include <iostream>
#include <string>

enum class LogLevel { Debug, Info, Warn, Error };

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

inline void Log(LogLevel level, const std::string &component,
                const std::string &message) {
  std::ostream &out = (level == LogLevel::Error || level == LogLevel::Warn)
                          ? std::cerr
                          : std::cout;
  out << "[" << LevelTag(level) << "][" << component << "]: " << message << "\n"
      << std::flush;
}
