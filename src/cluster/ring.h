#pragma once
// Consistent-hash ring mapping a shard key to the Pod node responsible for
// it. Node membership here is static (seeded once at startup by whoever
// constructs the ring) - join/leave, rebalancing, and moving data when
// membership changes ("resharding") are explicitly out of scope for this
// pass and will be added later. This only answers "which node owns this
// key right now."
#include "storage/interface.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>

class ShardRing {
public:
  explicit ShardRing(StorageDriver &d);
  ~ShardRing();

  // Adds `node_id` to the ring with `_splits` virtual points, spreading its
  // share of the key space evenly instead of owning one contiguous range.
  void AddNode(const std::string &node_id);
  // Removes every virtual point belonging to `node_id`.
  void RemoveNode(const std::string &node_id);

  // Returns the node_id responsible for `shard_key`: the node owning the
  // first virtual point at or after hash(shard_key) on the ring, wrapping
  // around to the first point if none is - or nullopt if the ring has no
  // nodes at all.
  std::optional<std::string> Owner(const std::string &shard_key) const;

private:
  StorageDriver &_d;
  std::map<uint32_t, std::string> _ring; // virtual point hash -> node_id
  int _splits = 200;

  void LoadFromStorage();
  void SaveToStorage() const;
};
