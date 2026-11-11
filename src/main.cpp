#include "common/log.h"
#include "entry/interface.h"
#include "storage/driver_leveldb.h"
#include <exception>
#include <iostream>

int main() {
  try {
    const pod::PodConfig config = pod::LoadConfigFromEnv();
    if (config.role == "router") {
      Log(LogLevel::Info, "main", "Starting Pods router");
      return entry_router(config);
    }
    Log(LogLevel::Info, "main",
        "Starting Pod (storage: " + config.storage_path + ")");
    LevelDBStorageDriver d(config.storage_path, config.sync_writes);
    return entry_standalone(d, config);
  } catch (const std::exception &e) {
    std::cerr << "[error][main]: " << e.what() << std::endl;
    return 1;
  }
}
