#include "cluster/ring.h"
#include "common/log.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr std::string_view kRingKey = "_sys/cluster/ring";

uint32_t FnvHash(const std::string &data) {
  uint32_t offset_basis = 2166136261u;
  uint32_t fnv_prime = 16777619u; // FNV-1a 32-bit prime
  uint32_t h = offset_basis;
  for (unsigned char c : data) {
    h ^= c;
    h *= fnv_prime;
  }
  return h;
}

} // namespace

ShardRing::ShardRing(StorageDriver &d) : _d(d) { LoadFromStorage(); }
ShardRing::~ShardRing() { SaveToStorage(); }

void ShardRing::LoadFromStorage() {
  auto [status, value] = _d.Get(kRingKey);
  if (!status.is_ok || status.is_not_found || value.empty())
    return;
  json parsed = json::from_msgpack(value);
  for (auto &[hash_str, node_id] : parsed.items())
    _ring[static_cast<uint32_t>(std::stoul(hash_str))] =
        node_id.get<std::string>();
}

void ShardRing::SaveToStorage() const {
  json obj = json::object();
  for (const auto &[hash, node_id] : _ring)
    obj[std::to_string(hash)] = node_id;
  auto packed = json::to_msgpack(obj);
  _d.Put(kRingKey, packed);
}

void ShardRing::AddNode(const std::string &node_id) {
  for (int i = 0; i < _splits; ++i)
    _ring[FnvHash(node_id + "_" + std::to_string(i))] = node_id;
  SaveToStorage();
  Log(LogLevel::Info, "cluster",
      "Added node (" + node_id + ") to shard ring with " +
          std::to_string(_splits) + " virtual points");
}

void ShardRing::RemoveNode(const std::string &node_id) {
  for (auto it = _ring.begin(); it != _ring.end();) {
    if (it->second == node_id)
      it = _ring.erase(it);
    else
      ++it;
  }
  SaveToStorage();
  Log(LogLevel::Info, "cluster",
      "Removed node (" + node_id + ") from shard ring");
}

std::optional<std::string>
ShardRing::Owner(const std::string &shard_key) const {
  if (_ring.empty())
    return std::nullopt;
  uint32_t h = FnvHash(shard_key);
  auto it = _ring.upper_bound(h);
  if (it == _ring.end())
    it = _ring.begin();
  return it->second;
}
