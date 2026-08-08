#include "leveldb/db.h"
#include <leveldb/status.h>
#include <optional>
// Caches db connection in a static / shared variable
leveldb::DB* getLevelDBInstance(std::optional<std::string> database_file = std::nullopt) {
    static leveldb::DB* db = nullptr;
    if (db == nullptr) {
        leveldb::Options options;
        options.create_if_missing = true;

        leveldb::Status status = leveldb::DB::Open(options, database_file.value_or("./pod-internal-locals"), &db);
        if (status.ok()) return db;
        else throw status.ToString();
    }
    return db;
}
