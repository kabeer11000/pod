#include "indexing/engine.h"
#include "common/log.h"
#include "indexing/codec.h"
#include <exception>

IndexEngine::IndexEngine(StorageDriver &d, IndexRegistry &registry)
    : _d(d), _registry(registry) {}

void IndexEngine::IndexOneDocument(const IndexDefinition &def,
                                   const std::string &doc_path,
                                   const json &document) {
  auto key = indexing::EncodeIndexKey(def.id, def.fields, document, doc_path);
  ByteVector value(doc_path.begin(), doc_path.end());
  _d.Put(key, value);
}

void IndexEngine::DeindexOneDocument(const IndexDefinition &def,
                                    const std::string &doc_path,
                                    const json &document) {
  auto key = indexing::EncodeIndexKey(def.id, def.fields, document, doc_path);
  _d.Delete(key);
}

void IndexEngine::ApplyWrite(const std::string &collection,
                            const std::string &doc_path,
                            const std::optional<json> &old_document,
                            const json &new_document) {
  auto indexes = _registry.ListForCollection(collection);
  for (const auto &def : indexes) {
    if (def.status == IndexStatus::Failed)
      continue;
    // Old and new indexed values may differ in the fields this index
    // covers even though the documents aren't byte-identical elsewhere;
    // simplest correct approach is to always deindex-then-reindex rather
    // than diff field-by-field. EncodeIndexKey is deterministic, so if the
    // indexed fields didn't actually change this is a harmless
    // delete-then-put-the-same-key no-op.
    if (old_document.has_value())
      DeindexOneDocument(def, doc_path, *old_document);
    IndexOneDocument(def, doc_path, new_document);
  }
}

void IndexEngine::ApplyDelete(const std::string &collection,
                             const std::string &doc_path,
                             const json &old_document) {
  auto indexes = _registry.ListForCollection(collection);
  for (const auto &def : indexes)
    DeindexOneDocument(def, doc_path, old_document);
}

void IndexEngine::Backfill(IndexDefinition def) {
  Log(LogLevel::Info, "indexing",
      "Backfill starting for index (" + def.id + ")");
  _registry.UpdateStatus(def.id, IndexStatus::Building);

  uint64_t scanned = 0;
  bool failed = false;
  std::string error_message;

  try {
    // Direct documents of a collection live one segment below it; scanning
    // the "<collection>/" prefix also reaches deeper subcollections, so
    // each hit is filtered to direct children only below.
    const std::string prefix = def.collection + "/";
    _d.ScanPrefix(prefix, [&](std::string_view key, ByteView value) {
      std::string_view rest = key;
      rest.remove_prefix(prefix.size());
      if (rest.find('/') != std::string_view::npos)
        return; // belongs to a deeper subcollection, not this collection

      std::string doc_path(key);
      json document = json::from_msgpack(value.begin(), value.end());
      IndexOneDocument(def, doc_path, document);
      scanned++;
      if (scanned % 500 == 0)
        _registry.UpdateProgress(def.id, scanned);
    });
  } catch (const std::exception &e) {
    failed = true;
    error_message = e.what();
  }

  if (failed) {
    Log(LogLevel::Error, "indexing",
        "Backfill failed for index (" + def.id + "): " + error_message);
    _registry.UpdateStatus(def.id, IndexStatus::Failed, error_message);
    return;
  }

  _registry.UpdateProgress(def.id, scanned, scanned);
  _registry.UpdateStatus(def.id, IndexStatus::Ready);
  Log(LogLevel::Info, "indexing",
      "Backfill complete for index (" + def.id +
          "), scanned=" + std::to_string(scanned));
}
