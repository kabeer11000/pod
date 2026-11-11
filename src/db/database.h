// The Pod document database engine.
//
// * Every operation is scoped to a namespace (see db/keys.h for isolation).
// * Reads and queries are lock-free and run against storage snapshots.
// * Mutations are serialised by a single commit lock and applied as one
//   atomic batch containing: the document changes, the secondary index
//   updates, a commit-log record (before-images, used for resume) and the new
//   commit sequence number. The sequence number is the resume token.
// * After each successful commit the registered CommitListener is invoked,
//   still under the commit lock, so listeners observe commits in exact order.
//
// Commit log (write-ahead change log), retained for `log_retention` commits:
//   'l' 0 <ns> 0 <seq be64>  -> msgpack {c: [{p: path, b: before|null}, ...]}
//   'L' 0 <seq be64>         -> ns          (lets retention prune by seq)
#pragma once
#include "db/document.h"
#include "db/error.h"
#include "db/field_path.h"
#include "db/index.h"
#include "db/path.h"
#include "db/query.h"
#include "storage/interface.h"
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pod {

struct ChangeRecord {
  std::string path;       // document path
  std::string collection; // parent collection path
  std::optional<Document> before;
  std::optional<Document> after;
};

struct CommitInfo {
  std::string ns;
  uint64_t sequence;
  std::vector<ChangeRecord> changes;
};

struct QueryPage {
  std::vector<Document> documents;
  bool has_more = false;
  std::optional<std::string> next_cursor;
  std::optional<std::string> index_used; // nullopt = collection scan
  uint64_t documents_scanned = 0;
};

struct WriteResult {
  std::string path;
  DocumentVersion version;
  bool created;
};

struct DeleteResult {
  bool is_collection;
  std::string path;
  uint32_t affected_count;
};

struct MutateManyResult {
  std::string path;
  uint32_t affected_count;
};

struct CreateIndexResult {
  IndexDefinition index;
  bool created;
  uint64_t documents_indexed;
};

struct DatabaseOptions {
  // Number of most recent commits kept in the commit log. 0 disables the log
  // (and therefore delta resumption).
  uint64_t log_retention = 100000;
};

// Read-side document filter (e.g. security rules): false hides a document.
using Visibility = std::function<bool(const Document &)>;
// Write-side check run for every change before a commit (e.g. security
// rules). Returning an error aborts the whole commit.
using ChangeGuard =
    std::function<std::expected<void, DbError>(const ChangeRecord &)>;

class Database {
public:
  using CommitListener = std::function<void(const CommitInfo &)>;

  explicit Database(StorageDriver &storage, DatabaseOptions options = {});
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  // Must be set before the database starts serving requests.
  void SetCommitListener(CommitListener listener);

  std::expected<std::optional<Document>, DbError>
  Read(const std::string &ns, const ResourcePath &doc);

  // Filtered, ordered, paginated query over the direct documents of a
  // collection. `cursor` is the id of the last document of the previous page.
  // Uses a secondary index when one fits the query.
  std::expected<QueryPage, DbError>
  Query(const std::string &ns, const ResourcePath &collection,
        const QuerySpec &spec, const std::optional<std::string> &cursor,
        const Visibility *visible = nullptr);

  // Every matching document, sorted, ignoring limit and projection.
  std::expected<std::vector<Document>, DbError>
  QueryAll(const std::string &ns, const ResourcePath &collection,
           const QuerySpec &spec, const Visibility *visible = nullptr);

  // Creates or replaces a document; with merge=true deep-merges into it.
  std::expected<WriteResult, DbError> Write(const std::string &ns,
                                            const ResourcePath &doc,
                                            const nlohmann::json &data,
                                            bool merge,
                                            const ChangeGuard *guard = nullptr);

  // Deletes a document, or every document of a collection, together with
  // all sub-collections nested underneath (recursive).
  std::expected<DeleteResult, DbError> Delete(const std::string &ns,
                                              const ResourcePath &path,
                                              const ChangeGuard *guard = nullptr);

  // Applies `remove` then `set` (dot-notation field paths) to every document
  // of the collection matching `where`, atomically.
  std::expected<MutateManyResult, DbError>
  MutateMany(const std::string &ns, const ResourcePath &collection,
             const std::vector<Filter> &where,
             const std::vector<FieldPath> &remove,
             const std::vector<std::pair<FieldPath, nlohmann::json>> &set,
             const ChangeGuard *guard = nullptr);

  // Secondary index management (per namespace).
  std::expected<CreateIndexResult, DbError>
  CreateIndex(const std::string &ns, const IndexDefinition &def);
  std::expected<void, DbError> DropIndex(const std::string &ns,
                                         const std::string &index_id);
  std::vector<IndexDefinition> ListIndexes(const std::string &ns);

  // Runs `fn(current_sequence)` while holding the commit lock, so no commit
  // can happen concurrently. Used to register subscriptions atomically with
  // their initial snapshot.
  template <class F> decltype(auto) WithCommitLock(F &&fn) {
    std::lock_guard<std::mutex> lock(_commit_mu);
    return fn(_sequence);
  }

  // --- The following require the commit lock (call inside WithCommitLock).
  // True if every commit after `token` is still in the commit log.
  bool CanResumeFrom(uint64_t token) const;
  // The documents of `target` (a document, or the direct documents of a
  // collection) as they were right after commit `token`, unsorted.
  std::expected<std::vector<Document>, DbError>
  StateAt(const std::string &ns, const ResourcePath &target, uint64_t token);

private:
  std::expected<std::vector<Document>, DbError>
  ScanDocuments(StorageReader &reader, const std::string &ns,
                const std::string &prefix);
  std::expected<std::optional<Document>, DbError>
  ReadFrom(StorageReader &reader, const std::string &ns,
           const ResourcePath &doc);
  std::expected<QueryPage, DbError>
  Execute(const std::string &ns, const ResourcePath &collection,
          const QuerySpec &spec, const std::optional<std::string> &cursor,
          bool apply_limit, const Visibility *visible);
  // Requires _commit_mu. Persists `ops` + index maintenance + commit log +
  // the next sequence number atomically, then notifies the listener.
  std::expected<uint64_t, DbError> Commit(const std::string &ns,
                                          std::vector<WriteOp> ops,
                                          std::vector<ChangeRecord> changes);
  std::vector<IndexDefinition> IndexesFor(const std::string &ns,
                                          const std::string &collection_id);
  void LoadIndexes();
  void InitCommitLog();

  StorageDriver &_storage;
  DatabaseOptions _options;
  std::mutex _commit_mu;
  uint64_t _sequence = 0;
  uint64_t _log_start = 1; // first sequence number that has a log record
  CommitListener _listener;
  // ns -> index definitions. Written under _commit_mu + unique _index_mu.
  std::shared_mutex _index_mu;
  std::unordered_map<std::string, std::vector<IndexDefinition>> _indexes;
};

} // namespace pod
