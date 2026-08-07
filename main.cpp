#include <iostream>
#include <leveldb/db.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;
    
    leveldb::Status status = leveldb::DB::Open(options, "./testdb", &db);
    
    if (status.ok()) {
        std::cout << "LevelDB initialized successfully!\n";
        
        json j;
        j["status"] = "online";
        std::cout << "JSON working: " << j.dump() << "\n";
        
        delete db;
    } else {
        std::cerr << "Failed to open LevelDB\n";
    }

    return 0;
}