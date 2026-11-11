#include "cluster/ring.h"
#include <algorithm>

namespace pod::cluster {

uint64_t RingHash(std::string_view key) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : key) {
    h ^= c;
    h *= 1099511628211ull;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

Ring::Ring(const std::vector<std::string> &node_ids,
           unsigned int virtual_nodes)
    : _nodes(node_ids) {
  for (size_t n = 0; n < _nodes.size(); ++n)
    for (unsigned int v = 0; v < virtual_nodes; ++v)
      _points.push_back(
          {RingHash(_nodes[n] + "#" + std::to_string(v)), n});
  // Ties (astronomically unlikely) resolve by node index, deterministically.
  std::sort(_points.begin(), _points.end(), [](const Point &a, const Point &b) {
    return a.hash != b.hash ? a.hash < b.hash : a.node < b.node;
  });
}

const std::string &Ring::Owner(std::string_view key) const {
  const uint64_t h = RingHash(key);
  auto it = std::lower_bound(
      _points.begin(), _points.end(), h,
      [](const Point &p, uint64_t value) { return p.hash < value; });
  if (it == _points.end())
    it = _points.begin();
  return _nodes[it->node];
}

} // namespace pod::cluster
