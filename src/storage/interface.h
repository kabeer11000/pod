#pragma once
#include "types.h"
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
  // virtual std::pair<DBStatus, std::vector<ByteVector>>
  // Query(std::string_view q) = 0;
};
