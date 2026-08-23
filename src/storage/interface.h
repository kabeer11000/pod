#pragma once
#include "types.h"
#include <functional>
#include <leveldb/status.h>
#include <string_view>
#include <utility>

class StorageDriver {
public:
  virtual ~StorageDriver() = default;
  // Store a ByteView (string_view) to the database
  virtual DBStatus Put(std::string_view key, ByteView value) = 0;
  // Take a buffer vector and store value into that for a given key
  virtual std::pair<DBStatus, ByteVector> Get(std::string_view key) = 0;
  virtual DBStatus Delete(std::string_view key) = 0;
  // Iterates every key with the given byte prefix, in ascending key order,
  // invoking `visitor` once per entry. Used for collection scans (index
  // backfill today; QUERY later) - LevelDB's natural key ordering makes a
  // prefix scan a contiguous range, so this never touches unrelated keys.
  virtual void
  ScanPrefix(std::string_view prefix,
             const std::function<void(std::string_view key, ByteView value)>
                 &visitor) = 0;
};
