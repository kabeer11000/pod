#pragma once
// Owns the single reserved LevelDB key `_sys/indexes`, which holds every
// index definition and its build status as one msgpack-encoded manifest.
// All access is guarded by an in-process mutex - sufficient because each Pod
// node's LevelDB has exactly one writer process (this node); it is not a
// cross-node coordination mechanism.
#include "indexing/interface.h"
#include "storage/interface.h"
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Derives the deterministic, human-readable index id from a collection path
// and its ordered field/direction list, e.g. collection "users/alice/orders"
// with fields [{total, DESCENDING}] -> "users_alice_orders__total_DESC".
// Identical (collection, fields) always produces the identical id, which is
// what makes CreateIndex idempotent - because the id embeds the FULL
// collection path, an index on a subcollection is automatically namespaced
// apart from one on a top-level collection of the same name.
std::string ComputeIndexId(const std::string &collection,
                           const std::vector<IndexFieldSpec> &fields);

class IndexRegistry {
public:
  explicit IndexRegistry(StorageDriver &d);

  // Registers (collection, fields) as an index if an identical one doesn't
  // already exist. Returns {definition, true} when this call created a
  // brand-new PENDING definition - the caller must enqueue exactly one
  // build for it, atomically with this call (both happen under the same
  // lock acquisition from the caller's perspective: CreateIndex itself does
  // not spawn a build, so the caller decides that only when `true` comes
  // back, which happens for exactly one racer even under concurrent
  // identical requests). Returns {existing, false} otherwise.
  std::pair<IndexDefinition, bool>
  CreateIndex(const std::string &collection,
              const std::vector<IndexFieldSpec> &fields);

  std::optional<IndexDefinition> Get(const std::string &index_id);
  std::vector<IndexDefinition> List();
  // Indexes registered directly on `collection` (not sub- or parent
  // collections) - used by the write path to find what to maintain live.
  std::vector<IndexDefinition> ListForCollection(const std::string &collection);

  // Removes the manifest entry AND deletes the index's physical entries
  // (idx:<id>:*) from storage. Returns false if the id didn't exist.
  bool Delete(const std::string &index_id);

  void UpdateStatus(const std::string &index_id, IndexStatus status,
                    std::optional<std::string> error = std::nullopt);
  void UpdateProgress(const std::string &index_id, uint64_t scanned,
                      std::optional<uint64_t> total = std::nullopt);

  // Startup recovery: resets any definition left in BUILDING (from a crash
  // or restart mid-backfill) back to PENDING and returns them, so the
  // caller can re-enqueue a build for each. Redoing a backfill from scratch
  // is always safe since index-entry writes are idempotent.
  std::vector<IndexDefinition> ReclaimStuckBuilds();

private:
  StorageDriver &_d;
  std::mutex _mu;

  std::unordered_map<std::string, IndexDefinition> _load();
  void _save(const std::unordered_map<std::string, IndexDefinition> &defs);
};
