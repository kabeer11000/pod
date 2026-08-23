#include "indexing/registry.h"
#include "common/timestamp.h"
#include "indexing/codec.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
constexpr std::string_view kManifestKey = "_sys/indexes";
}

std::string ComputeIndexId(const std::string &collection,
                           const std::vector<IndexFieldSpec> &fields) {
  std::string id = collection;
  for (char &c : id)
    if (c == '/')
      c = '_';
  id += "__";
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0)
      id += "_";
    std::string field_part = fields[i].field;
    for (char &c : field_part)
      if (c == '.')
        c = '_';
    id += field_part;
    id += (fields[i].direction == OrderByDirection::Ascending ? "_ASC"
                                                               : "_DESC");
  }
  return id;
}

IndexRegistry::IndexRegistry(StorageDriver &d) : _d(d) {}

std::unordered_map<std::string, IndexDefinition> IndexRegistry::_load() {
  auto [status, value] = _d.Get(kManifestKey);
  std::unordered_map<std::string, IndexDefinition> defs;
  if (!status.is_ok || status.is_not_found || value.empty())
    return defs;

  json parsed = json::from_msgpack(value);
  if (!parsed.contains("indexes"))
    return defs;
  for (auto &[id, def_json] : parsed.at("indexes").items())
    defs[id] = def_json.get<IndexDefinition>();
  return defs;
}

void IndexRegistry::_save(
    const std::unordered_map<std::string, IndexDefinition> &defs) {
  json indexes = json::object();
  for (const auto &[id, def] : defs)
    indexes[id] = def;
  json manifest = {{"indexes", indexes}};
  auto packed = json::to_msgpack(manifest);
  _d.Put(kManifestKey, packed);
}

std::pair<IndexDefinition, bool>
IndexRegistry::CreateIndex(const std::string &collection,
                           const std::vector<IndexFieldSpec> &fields) {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();

  const std::string id = ComputeIndexId(collection, fields);
  auto it = defs.find(id);
  if (it != defs.end())
    return {it->second, false};

  const std::string now = current_iso8601_timestamp();
  IndexDefinition def{
      .id = id,
      .collection = collection,
      .fields = fields,
      .status = IndexStatus::Pending,
      .created_at = now,
      .updated_at = now,
      .progress = {},
      .error = std::nullopt,
  };
  defs[id] = def;
  _save(defs);
  return {def, true};
}

std::optional<IndexDefinition> IndexRegistry::Get(const std::string &index_id) {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();
  auto it = defs.find(index_id);
  if (it == defs.end())
    return std::nullopt;
  return it->second;
}

std::vector<IndexDefinition> IndexRegistry::List() {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();
  std::vector<IndexDefinition> out;
  out.reserve(defs.size());
  for (auto &[id, def] : defs)
    out.push_back(def);
  return out;
}

std::vector<IndexDefinition>
IndexRegistry::ListForCollection(const std::string &collection) {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();
  std::vector<IndexDefinition> out;
  for (auto &[id, def] : defs)
    if (def.collection == collection)
      out.push_back(def);
  return out;
}

bool IndexRegistry::Delete(const std::string &index_id) {
  {
    std::lock_guard<std::mutex> lock(_mu);
    auto defs = _load();
    auto it = defs.find(index_id);
    if (it == defs.end())
      return false;
    defs.erase(it);
    _save(defs);
  }

  // Best-effort cleanup of the index's physical entries. The manifest entry
  // is already gone - which is what "the index no longer exists" means -
  // so any data left behind by a crash mid-cleanup is simply orphaned and
  // unreachable, not a correctness problem.
  std::vector<std::string> keys_to_delete;
  const std::string prefix = indexing::IndexEntryPrefix(index_id);
  _d.ScanPrefix(prefix, [&](std::string_view key, ByteView) {
    keys_to_delete.emplace_back(key);
  });
  for (auto &k : keys_to_delete)
    _d.Delete(k);
  return true;
}

void IndexRegistry::UpdateStatus(const std::string &index_id,
                                 IndexStatus status,
                                 std::optional<std::string> error) {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();
  auto it = defs.find(index_id);
  if (it == defs.end())
    return;
  it->second.status = status;
  it->second.error = error;
  it->second.updated_at = current_iso8601_timestamp();
  _save(defs);
}

void IndexRegistry::UpdateProgress(const std::string &index_id,
                                   uint64_t scanned,
                                   std::optional<uint64_t> total) {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();
  auto it = defs.find(index_id);
  if (it == defs.end())
    return;
  it->second.progress.scanned = scanned;
  if (total.has_value())
    it->second.progress.total = total;
  it->second.updated_at = current_iso8601_timestamp();
  _save(defs);
}

std::vector<IndexDefinition> IndexRegistry::ReclaimStuckBuilds() {
  std::lock_guard<std::mutex> lock(_mu);
  auto defs = _load();
  std::vector<IndexDefinition> reclaimed;
  for (auto &[id, def] : defs) {
    if (def.status != IndexStatus::Building)
      continue;
    def.status = IndexStatus::Pending;
    def.progress = {};
    def.updated_at = current_iso8601_timestamp();
    reclaimed.push_back(def);
  }
  if (!reclaimed.empty())
    _save(defs);
  return reclaimed;
}
