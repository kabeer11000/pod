#include "storage/driver_leveldb.h"
#include "types.h"
#include <functional>
#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

DBStatus convert_leveldb_status(leveldb::Status &s) {
  if (s.ok())
    return DBStatus::Ok();
  if (s.IsNotFound())
    return DBStatus::NotFound();

  return DBStatus::Error(s.ToString());
}
// Take a buffer vector and store value into that for a given key
// virtual DBStatus Get(std::string_view key, ByteVector* value) = 0;
// virtual DBStatus Delete(std::string_view key) = 0;
DBStatus LevelDBStorageDriver::Put(std::string_view key, ByteView value) {
  // Level DB likes its Slice as const character pointer bytes,
  // What we have is a custom byteview (std::span). We recast this binary
  // pointer to const char*
  leveldb::Slice k = {reinterpret_cast<const char *>(key.data()), key.size()};
  leveldb::Slice v = {reinterpret_cast<const char *>(value.data()),
                      value.size()};
  auto s = _db->Put(leveldb::WriteOptions(), k, v);

  return convert_leveldb_status(s);
}
// Take a buffer vector and store value into that for a given key
// virtual DBStatus Get(std::string_view key, ByteVector* value) = 0;
// virtual DBStatus Delete(std::string_view key) = 0;
std::pair<DBStatus, ByteVector>
LevelDBStorageDriver::Get(std::string_view key) {
  // Level DB likes its Slice as const character pointer bytes,
  // What we have is a custom byteview (std::span). We recast this binary
  // pointer to const char*
  leveldb::Slice k = {reinterpret_cast<const char *>(key.data()), key.size()};
  std::string v;
  auto s = _db->Get(leveldb::ReadOptions(), k, &v);
  return {convert_leveldb_status(s), ByteVector(v.begin(), v.end())};
}

DBStatus LevelDBStorageDriver::Delete(std::string_view key) {
  // Level DB likes its Slice as const character pointer bytes,
  // What we have is a custom byteview (std::span). We recast this binary
  // pointer to const char*
  leveldb::Slice k = {reinterpret_cast<const char *>(key.data()), key.size()};
  auto s = _db->Delete(leveldb::WriteOptions(), k);
  return convert_leveldb_status(s);
}
// A simple search method, which takes a callback function.
// Gets an iterator at prefix passed and then iterates and
// get all keys which have the prefix as a substring (passed to callback one by one)
// This way we can have an accumulator that saves and responds
void LevelDBStorageDriver::ScanPrefix(
    std::string_view prefix,
    const std::function<void(std::string_view key, ByteView value)>
        &visitor) {
  leveldb::Slice prefix_slice(prefix.data(), prefix.size());
  std::unique_ptr<leveldb::Iterator> iter(
      _db->NewIterator(leveldb::ReadOptions()));
  for (iter->Seek(prefix_slice); iter->Valid(); iter->Next()) {
    leveldb::Slice key = iter->key();
    if (key.size() < prefix.size() ||
        std::string_view(key.data(), prefix.size()) != prefix)
      break; // past the prefix range - LevelDB iterates in key order
    leveldb::Slice value = iter->value();
    visitor(std::string_view(key.data(), key.size()),
            ByteView(reinterpret_cast<const uint8_t *>(value.data()),
                     value.size()));
  }
}
// Constructors and Destructors on LevelDBStorageDriver
LevelDBStorageDriver::LevelDBStorageDriver(const std::string db_path) {
  leveldb::Options o;
  o.create_if_missing = true;
  leveldb::DB *d;
  auto status = leveldb::DB::Open(o, db_path, &d);
  if (!status.ok())
    throw std::runtime_error(
        "[error][driver_leveldb] Failed to open a new Level DB database. (" +
        status.ToString() + ")");

  _db = d;
}
LevelDBStorageDriver::~LevelDBStorageDriver() { delete _db; }
