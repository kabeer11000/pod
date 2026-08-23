#include <iostream>
#include <string>
#include "storage/driver_leveldb.h"
#include "entry/interface.h"

// `./pod` runs the single-node server; `./pod pods` runs the router
// instead, on its own LevelDB (it only persists its own bookkeeping - the
// shard ring - not application data, so it doesn't share storage with any
// backend node).
int main(int argc, char **argv) {
  bool router_mode = argc > 1 && std::string(argv[1]) == "pods";

  if (router_mode) {
    std::cout << "Starting Pod (router)..." << std::endl;
    LevelDBStorageDriver d("./pod-storage-pods");
    entry_pods(d);
  } else {
    std::cout << "Starting Pod..." << std::endl;
    LevelDBStorageDriver d("./pod-storage");
    entry_standalone(d);
  }
  return 0;
}
