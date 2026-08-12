#include "storage/driver_leveldb.h"
#include "storage/types.h"
#include "string"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

uint32_t hash(std::string &data) {
  // We add u at the end to remind compiler that this is an unsigned int.
  uint32_t offset_basis = 2166136261u;
  uint32_t fnv_prime = 16777573u;
  // Start from offset basis
  uint32_t h = offset_basis;

  for (auto c : data) {
    h ^= static_cast<uint32_t>(c);
    h *= fnv_prime;
  }
  return h;
}
class Ring {
private:
  std::vector<uint32_t> virtual_nodes(1);

public:
  ~Ring() {
    LevelDBStorageDriver d("./pod-internal-locals");
    auto [status, v] = d.Get("_sys/cluster/virtual_nodes");
    if (!status.is_ok) {
      std::cout << "[warn][cluster/ring]: Could not find existing "
                   "virtual_nodes, creating an empty ring";
      return;
    }
    if (v.size() % sizeof(uint32_t) != 0) {
      std::cout << "[warn][cluster/ring]: Failed to parse existing "
                   "virtual_nodes, type-mismatch can not convert to uint32_t "
                   "virtual_nodes, creating an empty ring";
      return;
    }
    v.
  };
  std::vector<uint32_t> add_server(std::string &node_id) {
    // Creates virtual positions for a given node_id,
    // These reduce burden on a single node and make ring more uniform
    int splits = 2000;
    std::vector<uint32_t> virtual_nodes;
    for (int i = 0; i < splits; i++) {
      // if node_id = 1; {1_1, 1_2, 1_3 ... 1_1999}
      auto input = node_id + "_" + std::to_string(i);
      virtual_nodes.push_back(hash(input));
    }
    return virtual_nodes;
  }
}
