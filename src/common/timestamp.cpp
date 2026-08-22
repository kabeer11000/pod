#include <chrono>
#include <format> // C++20
#include "string"

std::string current_iso8601_timestamp() {
  using namespace std::chrono;
  auto now = system_clock::now();
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z", floor<milliseconds>(now));
}
