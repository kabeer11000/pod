#include "storage/driver_leveldb.h"
#include "storage/interface.h"
#include "storage/types.h"
#include "string"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/types.h>
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
  std::vector<uint32_t> virtual_nodes;
  StorageDriver &_d;
  int splits = 2000;
  void load_ring_from_storage() {
    auto [status, v] = _d.Get("_sys/cluster/virtual_nodes");
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
    virtual_nodes.resize(v.size() / sizeof(uint32_t));
    memcpy(virtual_nodes.data(), v.data(), v.size());
  }
  void save_ring_to_storage() {
    ByteView bytes(reinterpret_cast<const uint8_t *>(virtual_nodes.data()),
                   virtual_nodes.size() * sizeof(uint32_t));
    _d.Put("_sys/cluster/virtual_nodes", bytes);
  }

public:
  Ring(StorageDriver &d) : _d(d) {
    std::cout << "[log][cluster/ring]: Ring object is being initialised";
    load_ring_from_storage();
  };
  ~Ring() { save_ring_to_storage(); };
  // To-do add a RemoveNode method
  std::vector<uint32_t> AddNode(std::string &node_id) {
    // Creates virtual positions for a given node_id,
    // These reduce burden on a single node and make ring more uniform
    std::vector<uint32_t> virtual_nodes;
    for (int i = 0; i < splits; i++) {
      // if node_id = 1; {1_1, 1_2, 1_3 ... 1_1999}
      auto input = node_id + "_" + std::to_string(i);
      virtual_nodes.push_back(hash(input));
    }
    return virtual_nodes;
  }
}
