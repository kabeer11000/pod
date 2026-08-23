#pragma once
// Index definition types shared by the registry (manifest storage), the
// codec (physical key encoding) and the engine/builder (backfill + live
// maintenance).
#include "network/ws/wire_protocol.h" // OrderByDirection
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

struct IndexFieldSpec {
  std::string field;
  OrderByDirection direction;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(IndexFieldSpec, field, direction)

enum class IndexStatus { Pending, Building, Ready, Failed };
NLOHMANN_JSON_SERIALIZE_ENUM(IndexStatus,
                             {
                                 {IndexStatus::Pending, "PENDING"},
                                 {IndexStatus::Building, "BUILDING"},
                                 {IndexStatus::Ready, "READY"},
                                 {IndexStatus::Failed, "FAILED"},
                             })

struct IndexBuildProgress {
  uint64_t scanned = 0;
  std::optional<uint64_t> total;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(IndexBuildProgress, scanned, total)

// One compound (or single-field) index definition, keyed by `id` (derived
// deterministically from `collection` + `fields` - see ComputeIndexId in
// registry.h). Because `id` embeds the FULL collection path, an index on a
// subcollection (e.g. "users/alice/orders") is automatically distinct from
// one on a top-level collection of the same name.
//
// `status` reaches READY only once the initial backfill AND live write-time
// maintenance are both active for it - see indexing/engine.h - so READY can
// be trusted by a future query planner without a separate "synced" flag.
struct IndexDefinition {
  std::string id;
  std::string collection;
  std::vector<IndexFieldSpec> fields;
  IndexStatus status = IndexStatus::Pending;
  std::string created_at;
  std::string updated_at;
  IndexBuildProgress progress;
  std::optional<std::string> error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(IndexDefinition, id, collection, fields,
                                   status, created_at, updated_at, progress,
                                   error)
