// storage/types.h
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

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

// A single mutation inside an atomic batch (see StorageDriver::Write).
struct WriteOp {
    enum class Kind { Put, Delete };
    Kind kind;
    std::string key;
    ByteVector value; // empty for Delete

    static WriteOp Put(std::string key, ByteVector value) {
        return WriteOp{Kind::Put, std::move(key), std::move(value)};
    }
    static WriteOp Delete(std::string key) {
        return WriteOp{Kind::Delete, std::move(key), {}};
    }
};
