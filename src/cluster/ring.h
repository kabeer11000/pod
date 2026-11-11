// Consistent-hash ring mapping namespaces to database nodes. Each node owns
// `virtual_nodes` points on a 64-bit ring; a namespace belongs to the first
// point at or after its hash. Adding or removing a node only moves the
// namespaces adjacent to its points (about 1/N of them).
//
// ponytail: membership is static (POD_NODES). Changing it reassigns some
// namespaces to nodes that do not have their data; add data migration before
// supporting live resharding.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pod::cluster {

// 64-bit FNV-1a followed by a splitmix64 finalizer for an even spread.
uint64_t RingHash(std::string_view key);

class Ring {
public:
  Ring(const std::vector<std::string> &node_ids, unsigned int virtual_nodes);

  // Id of the node owning `key`. The ring must not be empty.
  const std::string &Owner(std::string_view key) const;
  bool Empty() const { return _points.empty(); }

private:
  struct Point {
    uint64_t hash;
    size_t node; // index into _nodes
  };
  std::vector<std::string> _nodes;
  std::vector<Point> _points; // sorted by hash
};

} // namespace pod::cluster
