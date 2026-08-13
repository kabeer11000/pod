// storage/types.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "span"

using ByteVector = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;

struct DBStatus {
    bool is_ok;
    bool is_not_found;
    std::string message;

    static DBStatus Ok() { return {true, false, ""}; }
    static DBStatus NotFound() { return {true, true, "Key not found"}; }
    static DBStatus Error(const std::string& msg) { return {false, false, msg}; }
};
