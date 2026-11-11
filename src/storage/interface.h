#pragma once
#include "storage/types.h"
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

// Called for each key/value during a scan. Return false to stop iterating.
using ScanCallback =
    std::function<bool(std::string_view key, ByteView value)>;

// Read-only operations. Implemented by the live store and by snapshots.
class StorageReader {
public:
  virtual ~StorageReader() = default;
  virtual std::pair<DBStatus, ByteVector> Get(std::string_view key) = 0;
  // Visit every key that starts with `prefix`, in ascending byte order.
  virtual DBStatus Scan(std::string_view prefix, const ScanCallback &cb) = 0;
  // Visits keys starting with `prefix`, beginning at `start` and moving
  // forward (keys >= start) or backward (keys <= start). An empty `start`
  // means the first (forward) or last (reverse) key of the prefix range.
  virtual DBStatus Iterate(std::string_view prefix, std::string_view start,
                           bool reverse, const ScanCallback &cb) = 0;
};

class StorageDriver : public StorageReader {
public:
  ~StorageDriver() override = default;
  // Store a ByteView (string_view) to the database
  virtual DBStatus Put(std::string_view key, ByteView value) = 0;
  virtual DBStatus Delete(std::string_view key) = 0;
  // Apply every operation atomically: either all are persisted or none are.
  virtual DBStatus Write(const std::vector<WriteOp> &ops) = 0;
  // A consistent, immutable point-in-time view of the whole store. Every
  // read through it observes exactly the same state, regardless of writes
  // that happen afterwards.
  virtual std::unique_ptr<StorageReader> Snapshot() = 0;
};
