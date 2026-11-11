#pragma once
#include <cstdint>
#include <string>

// ISO-8601 UTC timestamp with millisecond precision, e.g. 2026-11-11T10:00:05.123Z
std::string current_iso8601_timestamp();
// Milliseconds since the Unix epoch.
int64_t current_unix_millis();
