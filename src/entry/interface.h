#pragma once
#include "storage/interface.h"
#include <string>

struct PodConfig {
  int port = 3000;
  std::string storage_path = "./pod-storage";
  // Namespace used when a client connects without ?ns=...
  std::string default_namespace = "default";
  unsigned int threads = 0; // 0 = hardware concurrency
  bool sync_writes = false;
};

// Reads POD_PORT, POD_STORAGE_PATH, DEFAULT_NAMESPACE, POD_THREADS,
// POD_SYNC_WRITES and POD_LOG_LEVEL from the environment. Throws
// std::invalid_argument on invalid values.
PodConfig LoadConfigFromEnv();

int entry_standalone(StorageDriver &d, const PodConfig &config);
