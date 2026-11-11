#pragma once
#include "storage/interface.h"
#include <string>

namespace leveldb {
class DB;
class Snapshot;
} // namespace leveldb

class LevelDBStorageDriver : public StorageDriver {
public:
  // sync_writes=true fsyncs every write (survives OS crashes, slower).
  explicit LevelDBStorageDriver(const std::string &db_path,
                                bool sync_writes = false);
  ~LevelDBStorageDriver() override;
  LevelDBStorageDriver(const LevelDBStorageDriver &) = delete;
  LevelDBStorageDriver &operator=(const LevelDBStorageDriver &) = delete;

  DBStatus Put(std::string_view key, ByteView value) override;
  std::pair<DBStatus, ByteVector> Get(std::string_view key) override;
  DBStatus Delete(std::string_view key) override;
  DBStatus Write(const std::vector<WriteOp> &ops) override;
  DBStatus Scan(std::string_view prefix, const ScanCallback &cb) override;
  DBStatus Iterate(std::string_view prefix, std::string_view start,
                   bool reverse, const ScanCallback &cb) override;
  std::unique_ptr<StorageReader> Snapshot() override;

private:
  leveldb::DB *_db = nullptr;
  bool _sync_writes = false;
};
