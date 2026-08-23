#pragma once
// Applies document mutations to every registered index over a collection
// (live write-time maintenance), and performs the initial full-collection
// backfill for a newly created index. Shared by the live write path
// (ApplyWrite/ApplyDelete, called from the WRITE/DELETE handlers after every
// successful primary mutation) and the background backfill worker
// (Backfill, called by IndexBuilder on its dedicated thread).
//
// Index-entry writes are idempotent - the physical key is a deterministic
// function of the document's own content + path - so overlap between a live
// write and a concurrent backfill scan of the same document is always safe:
// at worst it's a harmless duplicate Put of the same key/value. This is why
// live maintenance is active from the moment an index is registered
// (PENDING/BUILDING), not only once it reaches READY.
#include "indexing/interface.h"
#include "indexing/registry.h"
#include "storage/interface.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using json = nlohmann::json;

class IndexEngine {
public:
  IndexEngine(StorageDriver &d, IndexRegistry &registry);

  // Called after a document at `doc_path` (inside `collection`) is written.
  // `old_document` is its previous value (nullopt on insert). Every index
  // registered on `collection` gets its stale entry removed (if the old
  // document had a different indexed value) and the new one inserted.
  void ApplyWrite(const std::string &collection, const std::string &doc_path,
                  const std::optional<json> &old_document,
                  const json &new_document);

  // Called after a document is deleted; removes its entry from every index
  // registered on `collection`.
  void ApplyDelete(const std::string &collection, const std::string &doc_path,
                   const json &old_document);

  // Full-collection backfill for one index definition: scans every direct
  // document under `def.collection` (never recursing into subcollections,
  // matching collection-query scan semantics), indexes each one, and
  // reports progress/terminal status through the registry. Runs on
  // whatever thread calls it - IndexBuilder gives it a dedicated one.
  void Backfill(IndexDefinition def);

private:
  StorageDriver &_d;
  IndexRegistry &_registry;

  void IndexOneDocument(const IndexDefinition &def, const std::string &doc_path,
                       const json &document);
  void DeindexOneDocument(const IndexDefinition &def,
                         const std::string &doc_path, const json &document);
};
