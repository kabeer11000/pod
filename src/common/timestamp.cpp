#include "common/timestamp.h"
#include <chrono>
#include <format> // C++20
#include <string>

std::string current_iso8601_timestamp() {
  using namespace std::chrono;
  auto now = floor<milliseconds>(system_clock::now());
  // %S on a millisecond-precision time_point prints "SS.mmm".
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

int64_t current_unix_millis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}
