#include <iostream>
#include "storage/driver_leveldb.h"
#include "entry/interface.h"

int main() {
  std::cout << "Starting Pod..." << std::endl;
  LevelDBStorageDriver d("./pod-storage");
  entry_standalone(d);
  return 0;
}
