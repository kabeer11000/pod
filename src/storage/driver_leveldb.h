#pragma once
#include "storage/interface.h"
#include "string"
#include "functional"

namespace leveldb {
class DB;
}

class LevelDBStorageDriver : public StorageDriver {
public:
  LevelDBStorageDriver(
      const std::string db_path); // you'll want this too, see #3
  ~LevelDBStorageDriver() override;

  DBStatus Put(std::string_view key, ByteView value) override;
  std::pair<DBStatus, ByteVector> Get(std::string_view key) override;
  DBStatus Delete(std::string_view key) override;
  // std::pair<DBStatus, std::vector<ByteVector>>  Query(std::function<std::pair<std::string, ByteVector>>) override;

private:
  leveldb::DB *_db;
};
