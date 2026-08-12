#include "types.h"
#include <leveldb/db.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <string>
#include <utility>
#include "storage/driver_leveldb.h"

DBStatus convert_leveldb_status(leveldb::Status& s) {
    if (s.ok()) return DBStatus::Ok();
    return DBStatus::Error(s.ToString());
}
// Take a buffer vector and store value into that for a given key
// virtual DBStatus Get(std::string_view key, ByteVector* value) = 0;
// virtual DBStatus Delete(std::string_view key) = 0;
DBStatus LevelDBStorageDriver::Put(std::string_view key, ByteView value) {
    leveldb::DB *db;
    // Level DB likes its Slice as const character pointer bytes,
    // What we have is a custom byteview (std::span). We recast this binary pointer to const char*
    leveldb::Slice k = {reinterpret_cast<const char*>(key.data()), key.size()};
    leveldb::Slice v = {reinterpret_cast<const char*>(value.data()), value.size()};
    auto s = db->Put(leveldb::WriteOptions(), k, v);

    return convert_leveldb_status(s);
}
// Take a buffer vector and store value into that for a given key
// virtual DBStatus Get(std::string_view key, ByteVector* value) = 0;
// virtual DBStatus Delete(std::string_view key) = 0;
std::pair<DBStatus, ByteVector> LevelDBStorageDriver::Get(std::string_view key) {
    leveldb::DB *db;
    // Level DB likes its Slice as const character pointer bytes,
    // What we have is a custom byteview (std::span). We recast this binary pointer to const char*
    leveldb::Slice k = {reinterpret_cast<const char*>(key.data()), key.size()};
    std::string v;
    auto s = db->Get(leveldb::ReadOptions(), k, &v);
    return {convert_leveldb_status(s), ByteVector(v.begin(), v.end())};
}

DBStatus LevelDBStorageDriver::Delete(std::string_view key) {
    leveldb::DB *db;
    // Level DB likes its Slice as const character pointer bytes,
    // What we have is a custom byteview (std::span). We recast this binary pointer to const char*
    leveldb::Slice k = {reinterpret_cast<const char*>(key.data()), key.size()};
    auto s = db->Delete(leveldb::WriteOptions(), k);
    return convert_leveldb_status(s);
}
