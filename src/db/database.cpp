#include "db/database.h"
#include "common/log.h"
#include "common/timestamp.h"
#include "db/keys.h"
#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace pod {

using json = nlohmann::json;

namespace {

constexpr std::string_view kLogStartKey{"s\0log_start", 11};
constexpr size_t kMaxIndexesPerNamespace = 64;
constexpr size_t kBackfillChunk = 10000;

ByteVector EncodeU64(uint64_t v) {
  ByteVector out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<size_t>(i)] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
  return out;
}

void AppendU64(std::string &out, uint64_t v) {
  for (int i = 7; i >= 0; --i)
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

std::optional<uint64_t> DecodeU64(std::string_view bytes) {
  if (bytes.size() != 8)
    return std::nullopt;
  uint64_t v = 0;
  for (char c : bytes)
    v = (v << 8) | static_cast<uint8_t>(c);
  return v;
}

std::string LogPrefix(std::string_view ns) {
  std::string k;
  k += 'l';
  k += '\0';
  k += ns;
  k += '\0';
  return k;
}
std::string LogKey(std::string_view ns, uint64_t seq) {
  std::string k = LogPrefix(ns);
  AppendU64(k, seq);
  return k;
}
const std::string kLogIndexPrefix{"L\0", 2};
std::string LogIndexKey(uint64_t seq) {
  std::string k = kLogIndexPrefix;
  AppendU64(k, seq);
  return k;
}

DbError StorageError(const std::string &what, const DBStatus &s) {
  return DbError::Internal(what + ": " + s.message);
}

std::expected<ByteVector, DbError> EncodeChecked(const Document &doc) {
  ByteVector bytes = EncodeDocument(doc);
  if (bytes.size() > kMaxDocumentBytes)
    return std::unexpected(DbError::TooLarge(
        "Document '" + doc.path + "' exceeds the maximum size of " +
        std::to_string(kMaxDocumentBytes) + " bytes."));
  return bytes;
}

std::expected<DocumentVersion, DbError> NextVersion(const Document &before) {
  if (before.version == std::numeric_limits<DocumentVersion>::max())
    return std::unexpected(DbError::Internal(
        "Document '" + before.path + "' has exhausted its version counter."));
  return before.version + 1;
}

std::string LastSegment(const std::string &path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Smallest string strictly greater than every string starting with `s`;
// empty if there is none.
std::string Successor(std::string s) {
  while (!s.empty() && static_cast<uint8_t>(s.back()) == 0xFF)
    s.pop_back();
  if (!s.empty())
    s.back() = static_cast<char>(static_cast<uint8_t>(s.back()) + 1);
  return s;
}

std::string EscapedSize(std::string_view id) {
  std::string e;
  EncodeEscaped(e, id);
  return e;
}

// ----- Query planning ------------------------------------------------------

struct QueryPlan {
  IndexDefinition def;
  std::string prefix; // collection prefix + encodings of bound fields
  size_t bound = 0;   // number of leading fields bound by equality
  std::optional<std::string> lower; // encoded inclusive bound for field[bound]
  std::optional<std::string> upper;
  bool order_satisfied = false; // index order == query order
  bool reverse = false;
};

std::optional<QueryPlan> ChoosePlan(const std::vector<IndexDefinition> &defs,
                                    const std::string &ns,
                                    const std::string &collection_path,
                                    const QuerySpec &spec, bool has_limit) {
  auto referenced = [&](const FieldPath &p) {
    return std::any_of(spec.where.begin(), spec.where.end(),
                       [&](const Filter &f) { return f.field == p; }) ||
           std::any_of(spec.order_by.begin(), spec.order_by.end(),
                       [&](const OrderBy &o) { return o.field == p; });
  };

  std::optional<QueryPlan> best;
  std::tuple<size_t, bool, bool> best_score{0, false, false};
  for (const auto &def : defs) {
    // Sparse index: every indexed field must be required by the query, or
    // documents lacking it (absent from the index) could wrongly be missed.
    if (!std::all_of(def.fields.begin(), def.fields.end(),
                     [&](const IndexField &f) { return referenced(f.path); }))
      continue;

    QueryPlan plan{.def = def,
                   .prefix = index_keys::CollectionPrefix(ns, def.id,
                                                          collection_path),
                   .bound = 0,
                   .lower = std::nullopt,
                   .upper = std::nullopt,
                   .order_satisfied = false,
                   .reverse = false};
    size_t k = 0;
    for (; k < def.fields.size(); ++k) {
      const IndexField &f = def.fields[k];
      const FilterOp wanted = f.mode == IndexMode::Value ? FilterOp::Equal
                                                         : FilterOp::ArrayIn;
      auto eq = std::find_if(spec.where.begin(), spec.where.end(),
                             [&](const Filter &flt) {
                               return flt.field == f.path && flt.op == wanted;
                             });
      if (eq == spec.where.end())
        break;
      EncodeIndexValue(plan.prefix, eq->value);
    }
    plan.bound = k;
    // An unbound ARRAY_CONTAINS field would yield one key per element.
    bool unusable = false;
    for (size_t i = k; i < def.fields.size(); ++i)
      if (def.fields[i].mode == IndexMode::ArrayContains)
        unusable = true;
    if (unusable)
      continue;

    if (k < def.fields.size()) {
      const FieldPath &rf = def.fields[k].path;
      const json *lo = nullptr, *hi = nullptr;
      for (const auto &flt : spec.where) {
        if (flt.field != rf)
          continue;
        if (flt.op == FilterOp::Greater || flt.op == FilterOp::GreaterEqual) {
          if (!lo || CompareValues(flt.value, *lo) > 0)
            lo = &flt.value;
        } else if (flt.op == FilterOp::Lesser ||
                   flt.op == FilterOp::LesserEqual) {
          if (!hi || CompareValues(flt.value, *hi) < 0)
            hi = &flt.value;
        }
      }
      if (lo) {
        plan.lower.emplace();
        EncodeIndexValue(*plan.lower, *lo);
      }
      if (hi) {
        plan.upper.emplace();
        EncodeIndexValue(*plan.upper, *hi);
      }
    }

    const size_t rest = def.fields.size() - k;
    if (spec.order_by.empty()) {
      plan.order_satisfied = rest == 0; // key order is then doc id ascending
    } else if (rest == spec.order_by.size()) {
      bool same = true;
      for (size_t i = 0; i < rest && same; ++i)
        same = def.fields[k + i].path == spec.order_by[i].field &&
               spec.order_by[i].descending == spec.order_by[0].descending;
      plan.order_satisfied = same;
      plan.reverse = same && spec.order_by[0].descending;
    }

    const bool ranged = plan.lower.has_value() || plan.upper.has_value();
    if (!(k > 0 || ranged || (plan.order_satisfied && has_limit)))
      continue;
    const std::tuple<size_t, bool, bool> score{k, ranged,
                                               plan.order_satisfied};
    if (!best || score > best_score ||
        (score == best_score &&
         plan.def.fields.size() < best->def.fields.size())) {
      best = std::move(plan);
      best_score = score;
    }
  }
  return best;
}

} // namespace

// ----- Construction ----------------------------------------------------------

Database::Database(StorageDriver &storage, DatabaseOptions options)
    : _storage(storage), _options(options) {
  auto [status, bytes] = _storage.Get(keys::kCommitSequenceKey);
  if (!status.is_ok)
    throw std::runtime_error("Failed to load commit sequence: " +
                             status.message);
  if (status.is_not_found) {
    _sequence = 0;
  } else {
    auto seq = DecodeU64(std::string_view(
        reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    if (!seq)
      throw std::runtime_error("Stored commit sequence is corrupted.");
    _sequence = *seq;
  }
  InitCommitLog();
  LoadIndexes();
  Log(LogLevel::Info, "db",
      "Database opened at commit sequence " + std::to_string(_sequence));
}

void Database::InitCommitLog() {
  auto [status, bytes] = _storage.Get(kLogStartKey);
  if (!status.is_ok)
    throw std::runtime_error("Failed to load commit log state: " +
                             status.message);
  if (status.is_not_found) {
    // Commits made before the log existed cannot be resumed across.
    _log_start = _sequence + 1;
    const ByteVector v = EncodeU64(_log_start);
    const DBStatus s = _storage.Put(kLogStartKey, v);
    if (!s.is_ok)
      throw std::runtime_error("Failed to initialise commit log: " +
                               s.message);
  } else {
    auto v = DecodeU64(std::string_view(
        reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    if (!v)
      throw std::runtime_error("Stored commit log state is corrupted.");
    _log_start = *v;
  }

  // Apply the retention policy to what is on disk (it may have been lowered,
  // or the log disabled, since the last run).
  const uint64_t keep_after =
      _options.log_retention == 0
          ? std::numeric_limits<uint64_t>::max()
          : (_sequence > _options.log_retention
                 ? _sequence - _options.log_retention
                 : 0);
  std::vector<WriteOp> ops;
  const DBStatus s = _storage.Scan(
      kLogIndexPrefix, [&](std::string_view key, ByteView value) {
        auto seq = DecodeU64(key.substr(kLogIndexPrefix.size()));
        if (!seq || *seq > keep_after)
          return false; // keys are in sequence order
        ops.push_back(WriteOp::Delete(std::string(key)));
        ops.push_back(WriteOp::Delete(LogKey(
            std::string_view(reinterpret_cast<const char *>(value.data()),
                             value.size()),
            *seq)));
        return true;
      });
  if (!s.is_ok)
    throw std::runtime_error("Failed to scan commit log: " + s.message);
  if (!ops.empty()) {
    const DBStatus w = _storage.Write(ops);
    if (!w.is_ok)
      throw std::runtime_error("Failed to prune commit log: " + w.message);
  }
}

void Database::LoadIndexes() {
  const std::string prefix{"x\0", 2};
  std::optional<std::string> failure;
  const DBStatus s =
      _storage.Scan(prefix, [&](std::string_view key, ByteView value) {
        const std::string_view rest = key.substr(prefix.size());
        const size_t sep = rest.find('\0');
        if (sep == std::string_view::npos) {
          failure = "malformed index key";
          return false;
        }
        json j;
        try {
          j = json::parse(value.begin(), value.end());
        } catch (const std::exception &e) {
          failure = e.what();
          return false;
        }
        auto def = IndexDefinitionFromJson(j);
        if (!def) {
          failure = def.error().message;
          return false;
        }
        _indexes[std::string(rest.substr(0, sep))].push_back(std::move(*def));
        return true;
      });
  if (failure)
    throw std::runtime_error("Failed to load index definitions: " + *failure);
  if (!s.is_ok)
    throw std::runtime_error("Failed to load index definitions: " +
                             s.message);
}

void Database::SetCommitListener(CommitListener listener) {
  std::lock_guard<std::mutex> lock(_commit_mu);
  _listener = std::move(listener);
}

// ----- Reads -----------------------------------------------------------------

std::expected<std::vector<Document>, DbError>
Database::ScanDocuments(StorageReader &reader, const std::string &ns,
                        const std::string &prefix) {
  std::vector<Document> docs;
  std::optional<DbError> failure;
  const DBStatus s =
      reader.Scan(prefix, [&](std::string_view key, ByteView value) {
        auto path = keys::DocumentPathFromKey(ns, key);
        if (!path) {
          failure =
              DbError::DataCorruption("Malformed document key in storage.");
          return false;
        }
        auto doc = DecodeDocument(value, std::move(*path));
        if (!doc) {
          failure = doc.error();
          return false;
        }
        docs.push_back(std::move(*doc));
        return true;
      });
  if (failure)
    return std::unexpected(*failure);
  if (!s.is_ok)
    return std::unexpected(StorageError("Scan failed", s));
  return docs;
}

std::expected<std::optional<Document>, DbError>
Database::ReadFrom(StorageReader &reader, const std::string &ns,
                   const ResourcePath &doc) {
  auto [status, bytes] = reader.Get(keys::DocumentKey(ns, doc));
  if (!status.is_ok)
    return std::unexpected(StorageError("Read failed", status));
  if (status.is_not_found)
    return std::optional<Document>{};
  auto decoded = DecodeDocument(bytes, doc.ToString());
  if (!decoded)
    return std::unexpected(decoded.error());
  return std::optional<Document>{std::move(*decoded)};
}

std::expected<std::optional<Document>, DbError>
Database::Read(const std::string &ns, const ResourcePath &doc) {
  if (!doc.IsDocument())
    return std::unexpected(
        DbError::InvalidPath("Expected a document path for a read."));
  return ReadFrom(_storage, ns, doc);
}

std::vector<IndexDefinition>
Database::IndexesFor(const std::string &ns, const std::string &collection_id) {
  std::vector<IndexDefinition> out;
  auto it = _indexes.find(ns);
  if (it == _indexes.end())
    return out;
  for (const auto &d : it->second)
    if (d.collection_id == collection_id)
      out.push_back(d);
  return out;
}

std::expected<std::vector<Document>, DbError>
Database::QueryAll(const std::string &ns, const ResourcePath &collection,
                   const QuerySpec &spec, const Visibility *visible) {
  auto page = Execute(ns, collection, spec, std::nullopt, false, visible);
  if (!page)
    return std::unexpected(page.error());
  return std::move(page->documents);
}

std::expected<QueryPage, DbError>
Database::Query(const std::string &ns, const ResourcePath &collection,
                const QuerySpec &spec,
                const std::optional<std::string> &cursor,
                const Visibility *visible) {
  return Execute(ns, collection, spec, cursor, true, visible);
}

std::expected<QueryPage, DbError>
Database::Execute(const std::string &ns, const ResourcePath &collection,
                  const QuerySpec &spec,
                  const std::optional<std::string> &cursor, bool apply_limit,
                  const Visibility *visible) {
  if (!collection.IsCollection())
    return std::unexpected(
        DbError::InvalidPath("Expected a collection path for a query."));

  // Index definitions and the snapshot are taken together so the snapshot
  // always contains the complete entries of every index we may use.
  std::vector<IndexDefinition> defs;
  std::unique_ptr<StorageReader> snap;
  {
    std::shared_lock<std::shared_mutex> lock(_index_mu);
    defs = IndexesFor(ns, collection.Id());
    snap = _storage.Snapshot();
  }

  auto matches = [&](const Document &d) {
    return MatchesQuery(d, spec) && (!visible || (*visible)(d));
  };
  auto less = [&](const Document &a, const Document &b) {
    return DocumentLess(a, b, spec);
  };

  // The cursor document positions the page: results start strictly after it
  // in sort order, even if it changed or stopped matching since.
  std::optional<Document> anchor;
  if (cursor) {
    if (cursor->empty() || cursor->find('/') != std::string::npos)
      return std::unexpected(
          DbError::InvalidCursor("Cursor is not a valid document id."));
    auto cursor_path = ParsePath(collection.ToString() + "/" + *cursor);
    if (!cursor_path || !cursor_path->IsDocument())
      return std::unexpected(
          DbError::InvalidCursor("Cursor is not a valid document id."));
    auto read = ReadFrom(*snap, ns, *cursor_path);
    if (!read)
      return std::unexpected(read.error());
    if (!*read)
      return std::unexpected(DbError::InvalidCursor(
          "Cursor document '" + *cursor + "' no longer exists."));
    for (const auto &o : spec.order_by)
      if (!GetField((*read)->data, o.field))
        return std::unexpected(DbError::InvalidCursor(
            "Cursor document '" + *cursor + "' is missing an orderBy field."));
    anchor = std::move(*read);
  }

  const std::optional<uint32_t> limit =
      apply_limit ? spec.limit : std::optional<uint32_t>{};
  const auto plan = ChoosePlan(defs, ns, collection.ToString(), spec,
                               limit.has_value());

  QueryPage page;
  std::vector<Document> docs;

  if (plan) {
    page.index_used = plan->def.id;
    const std::string &prefix = plan->prefix;
    const bool early = plan->order_satisfied && limit.has_value();
    const bool reverse = early && plan->reverse;

    // Scan bounds (inclusive of whole collision groups; filters re-checked).
    const std::optional<std::string> lower_key =
        plan->lower ? std::optional<std::string>(prefix + *plan->lower)
                    : std::nullopt;
    const std::optional<std::string> upper_key =
        plan->upper ? std::optional<std::string>(prefix + *plan->upper)
                    : std::nullopt;
    std::string anchor_group;
    if (early && anchor) {
      anchor_group = prefix;
      for (size_t i = plan->bound; i < plan->def.fields.size(); ++i)
        EncodeIndexValue(anchor_group,
                         *GetField(anchor->data, plan->def.fields[i].path));
    }
    std::string start;
    if (!reverse) {
      if (lower_key)
        start = *lower_key;
      if (!anchor_group.empty() && anchor_group > start)
        start = anchor_group;
    } else {
      if (upper_key)
        start = Successor(*upper_key);
      if (!anchor_group.empty()) {
        const std::string a = Successor(anchor_group);
        if (start.empty() || (!a.empty() && a < start))
          start = a;
      }
    }

    std::unordered_set<std::string> seen;
    std::optional<DbError> failure;
    std::string last_group;
    const DBStatus s = snap->Iterate(
        prefix, start, reverse, [&](std::string_view key, ByteView value) {
          if (!reverse && upper_key && key > std::string_view(*upper_key) &&
              !key.starts_with(*upper_key))
            return false;
          if (reverse && lower_key && key < std::string_view(*lower_key))
            return false;
          if (reverse && upper_key && key > std::string_view(*upper_key) &&
              !key.starts_with(*upper_key))
            return true; // the exclusive successor itself
          const std::string id(reinterpret_cast<const char *>(value.data()),
                               value.size());
          const std::string escaped = EscapedSize(id);
          if (key.size() < prefix.size() + escaped.size()) {
            failure = DbError::DataCorruption("Malformed index entry.");
            return false;
          }
          const std::string_view group = key.substr(
              prefix.size(), key.size() - prefix.size() - escaped.size());
          if (early && docs.size() > *limit && group != last_group)
            return false; // past the collision group of the (limit+1)-th doc
          if (!seen.insert(id).second)
            return true;
          auto doc = ReadFrom(*snap, ns, collection.Child(id));
          if (!doc) {
            failure = doc.error();
            return false;
          }
          ++page.documents_scanned;
          if (!*doc)
            return true; // orphaned entry (e.g. interrupted backfill)
          if (!matches(**doc))
            return true;
          if (early && anchor && !less(*anchor, **doc))
            return true;
          docs.push_back(std::move(**doc));
          last_group.assign(group);
          return true;
        });
    if (failure)
      return std::unexpected(*failure);
    if (!s.is_ok)
      return std::unexpected(StorageError("Index scan failed", s));

    SortDocuments(docs, spec);
    if (early) {
      page.has_more = docs.size() > *limit;
      if (page.has_more)
        docs.resize(*limit);
      if (page.has_more && !docs.empty())
        page.next_cursor = docs.back().id;
      page.documents = std::move(docs);
      return page;
    }
  } else {
    auto all = ScanDocuments(*snap, ns,
                             keys::CollectionDocumentsPrefix(ns, collection));
    if (!all)
      return std::unexpected(all.error());
    page.documents_scanned = all->size();
    docs = std::move(*all);
    std::erase_if(docs, [&](const Document &d) { return !matches(d); });
    SortDocuments(docs, spec);
  }

  size_t start = 0;
  if (anchor)
    start = static_cast<size_t>(
        std::upper_bound(docs.begin(), docs.end(), *anchor, less) -
        docs.begin());
  const size_t remaining = docs.size() - start;
  const size_t take = limit ? std::min<size_t>(*limit, remaining) : remaining;
  page.documents.reserve(take);
  for (size_t i = start; i < start + take; ++i)
    page.documents.push_back(std::move(docs[i]));
  page.has_more = start + take < docs.size();
  if (page.has_more && !page.documents.empty())
    page.next_cursor = page.documents.back().id;
  return page;
}

// ----- Commit ----------------------------------------------------------------

std::expected<uint64_t, DbError>
Database::Commit(const std::string &ns, std::vector<WriteOp> ops,
                 std::vector<ChangeRecord> changes) {
  const uint64_t next = _sequence + 1;

  // Secondary index maintenance.
  for (const auto &c : changes) {
    for (const auto &def : IndexesFor(ns, LastSegment(c.collection))) {
      std::vector<std::string> before, after;
      if (c.before)
        before = IndexEntries(ns, def, c.collection, *c.before);
      if (c.after)
        after = IndexEntries(ns, def, c.collection, *c.after);
      const std::set<std::string> b(before.begin(), before.end());
      const std::set<std::string> a(after.begin(), after.end());
      for (const auto &k : b)
        if (!a.contains(k))
          ops.push_back(WriteOp::Delete(k));
      for (const auto &k : a)
        if (!b.contains(k)) {
          const std::string &id = c.after->id;
          ops.push_back(WriteOp::Put(k, ByteVector(id.begin(), id.end())));
        }
    }
  }

  // Commit log record (before-images) + retention.
  if (_options.log_retention > 0) {
    json entries = json::array();
    for (const auto &c : changes) {
      json e = {{"p", c.path}, {"b", nullptr}};
      if (c.before)
        e["b"] = json::binary(EncodeDocument(*c.before));
      entries.push_back(std::move(e));
    }
    ops.push_back(
        WriteOp::Put(LogKey(ns, next), json::to_msgpack(json{{"c", entries}})));
    ops.push_back(
        WriteOp::Put(LogIndexKey(next), ByteVector(ns.begin(), ns.end())));
    if (next > _options.log_retention) {
      const uint64_t prune = next - _options.log_retention;
      auto [st, owner] = _storage.Get(LogIndexKey(prune));
      if (!st.is_ok)
        return std::unexpected(StorageError("Commit log prune failed", st));
      if (!st.is_not_found) {
        ops.push_back(WriteOp::Delete(LogIndexKey(prune)));
        ops.push_back(WriteOp::Delete(LogKey(
            std::string_view(reinterpret_cast<const char *>(owner.data()),
                             owner.size()),
            prune)));
      }
    }
  }

  ops.push_back(WriteOp::Put(std::string(keys::kCommitSequenceKey),
                             EncodeU64(next)));
  const DBStatus s = _storage.Write(ops);
  if (!s.is_ok)
    return std::unexpected(StorageError("Commit failed", s));
  _sequence = next;
  if (_listener) {
    try {
      _listener(CommitInfo{ns, next, std::move(changes)});
    } catch (const std::exception &e) {
      Log(LogLevel::Error, "db",
          std::string("Commit listener threw: ") + e.what());
    } catch (...) {
      Log(LogLevel::Error, "db", "Commit listener threw an unknown exception");
    }
  }
  return next;
}

static std::expected<void, DbError> RunGuard(const ChangeGuard *guard,
                                             const std::vector<ChangeRecord> &changes) {
  if (!guard)
    return {};
  for (const auto &c : changes)
    if (auto ok = (*guard)(c); !ok)
      return ok;
  return {};
}

std::expected<WriteResult, DbError> Database::Write(const std::string &ns,
                                                    const ResourcePath &doc,
                                                    const json &data,
                                                    bool merge,
                                                    const ChangeGuard *guard) {
  if (!doc.IsDocument())
    return std::unexpected(
        DbError::InvalidPath("Expected a document path for a write."));
  if (!data.is_object())
    return std::unexpected(
        DbError::InvalidArgument("Document data must be an object."));

  std::lock_guard<std::mutex> lock(_commit_mu);
  auto existing = ReadFrom(_storage, ns, doc);
  if (!existing)
    return std::unexpected(existing.error());

  const int64_t now = current_unix_millis();
  Document after;
  after.path = doc.ToString();
  after.id = doc.Id();
  if (*existing) {
    const Document &before = **existing;
    auto v = NextVersion(before);
    if (!v)
      return std::unexpected(v.error());
    after.version = *v;
    after.create_time_ms = before.create_time_ms;
    if (merge) {
      after.data = before.data;
      DeepMerge(after.data, data);
    } else {
      after.data = data;
    }
  } else {
    after.version = 1;
    after.create_time_ms = now;
    after.data = data;
  }
  after.update_time_ms = now;

  auto bytes = EncodeChecked(after);
  if (!bytes)
    return std::unexpected(bytes.error());

  std::vector<ChangeRecord> changes;
  changes.push_back(
      ChangeRecord{after.path, doc.ParentCollection(), *existing, after});
  if (auto ok = RunGuard(guard, changes); !ok)
    return std::unexpected(ok.error());

  std::vector<WriteOp> ops;
  ops.push_back(WriteOp::Put(keys::DocumentKey(ns, doc), std::move(*bytes)));
  const bool created = !existing->has_value();
  const DocumentVersion version = after.version;
  auto committed = Commit(ns, std::move(ops), std::move(changes));
  if (!committed)
    return std::unexpected(committed.error());
  return WriteResult{doc.ToString(), version, created};
}

std::expected<DeleteResult, DbError>
Database::Delete(const std::string &ns, const ResourcePath &path,
                 const ChangeGuard *guard) {
  std::lock_guard<std::mutex> lock(_commit_mu);
  std::vector<Document> victims;

  if (path.IsDocument()) {
    auto self = ReadFrom(_storage, ns, path);
    if (!self)
      return std::unexpected(self.error());
    if (*self)
      victims.push_back(std::move(**self));
  } else {
    auto direct = ScanDocuments(_storage, ns,
                                keys::CollectionDocumentsPrefix(ns, path));
    if (!direct)
      return std::unexpected(direct.error());
    for (auto &d : *direct)
      victims.push_back(std::move(d));
  }
  auto nested =
      ScanDocuments(_storage, ns, keys::DescendantsPrefix(ns, path));
  if (!nested)
    return std::unexpected(nested.error());
  for (auto &d : *nested)
    victims.push_back(std::move(d));

  if (victims.size() > std::numeric_limits<uint32_t>::max())
    return std::unexpected(DbError::InvalidArgument(
        "Too many documents to delete in a single operation."));

  DeleteResult result{path.IsCollection(), path.ToString(),
                      static_cast<uint32_t>(victims.size())};
  if (victims.empty())
    return result;

  std::vector<WriteOp> ops;
  std::vector<ChangeRecord> changes;
  ops.reserve(victims.size());
  changes.reserve(victims.size());
  for (auto &d : victims) {
    auto p = ParsePath(d.path);
    if (!p || !p->IsDocument())
      return std::unexpected(
          DbError::DataCorruption("Stored document has an invalid path."));
    ops.push_back(WriteOp::Delete(keys::DocumentKey(ns, *p)));
    std::string collection = p->ParentCollection();
    std::string doc_path = d.path;
    changes.push_back(ChangeRecord{std::move(doc_path), std::move(collection),
                                   std::move(d), std::nullopt});
  }
  if (auto ok = RunGuard(guard, changes); !ok)
    return std::unexpected(ok.error());
  auto committed = Commit(ns, std::move(ops), std::move(changes));
  if (!committed)
    return std::unexpected(committed.error());
  return result;
}

std::expected<MutateManyResult, DbError> Database::MutateMany(
    const std::string &ns, const ResourcePath &collection,
    const std::vector<Filter> &where, const std::vector<FieldPath> &remove,
    const std::vector<std::pair<FieldPath, json>> &set,
    const ChangeGuard *guard) {
  if (!collection.IsCollection())
    return std::unexpected(DbError::InvalidPath(
        "Expected a collection path for MUTATE_MANY."));
  if (remove.empty() && set.empty())
    return std::unexpected(DbError::InvalidArgument(
        "MUTATE_MANY requires at least one of 'remove' or 'set'."));
  for (const auto &r : remove)
    for (const auto &[s, _] : set)
      if (FieldPathsOverlap(r, s))
        return std::unexpected(DbError::InvalidArgument(
            "Field '" + FieldPathToString(r) + "' in 'remove' overlaps '" +
            FieldPathToString(s) + "' in 'set'."));

  std::lock_guard<std::mutex> lock(_commit_mu);
  QuerySpec spec;
  spec.where = where;
  // Uses a secondary index for the filters when one fits.
  auto matched = QueryAll(ns, collection, spec);
  if (!matched)
    return std::unexpected(matched.error());

  const int64_t now = current_unix_millis();
  std::vector<WriteOp> ops;
  std::vector<ChangeRecord> changes;
  for (auto &before : *matched) {
    json data = before.data;
    for (const auto &r : remove)
      RemoveField(data, r);
    for (const auto &[field, value] : set)
      SetField(data, field, value);
    if (data == before.data)
      continue; // nothing actually changed: no version bump, no event

    auto v = NextVersion(before);
    if (!v)
      return std::unexpected(v.error());
    Document after;
    after.path = before.path;
    after.id = before.id;
    after.data = std::move(data);
    after.version = *v;
    after.create_time_ms = before.create_time_ms;
    after.update_time_ms = now;
    auto bytes = EncodeChecked(after);
    if (!bytes)
      return std::unexpected(bytes.error());
    ops.push_back(WriteOp::Put(
        keys::DocumentKey(ns, collection.Child(after.id)), std::move(*bytes)));
    changes.push_back(ChangeRecord{after.path, collection.ToString(),
                                   std::move(before), std::move(after)});
  }

  if (changes.size() > std::numeric_limits<uint32_t>::max())
    return std::unexpected(DbError::InvalidArgument(
        "Too many documents to update in a single operation."));
  MutateManyResult result{collection.ToString(),
                          static_cast<uint32_t>(changes.size())};
  if (ops.empty())
    return result;
  if (auto ok = RunGuard(guard, changes); !ok)
    return std::unexpected(ok.error());
  auto committed = Commit(ns, std::move(ops), std::move(changes));
  if (!committed)
    return std::unexpected(committed.error());
  return result;
}

// ----- Indexes ---------------------------------------------------------------

std::expected<CreateIndexResult, DbError>
Database::CreateIndex(const std::string &ns, const IndexDefinition &def) {
  std::lock_guard<std::mutex> lock(_commit_mu);
  {
    auto it = _indexes.find(ns);
    if (it != _indexes.end()) {
      for (const auto &d : it->second)
        if (d.id == def.id)
          return CreateIndexResult{d, false, 0};
      if (it->second.size() >= kMaxIndexesPerNamespace)
        return std::unexpected(DbError::InvalidArgument(
            "A namespace may have at most " +
            std::to_string(kMaxIndexesPerNamespace) + " indexes."));
    }
  }

  // Backfill. Writers are blocked by the commit lock and queries cannot use
  // the index until it is published below, so chunked writes are safe.
  uint64_t indexed = 0;
  std::vector<WriteOp> ops;
  std::optional<DbError> failure;
  auto flush = [&]() -> bool {
    if (ops.empty())
      return true;
    const DBStatus w = _storage.Write(ops);
    ops.clear();
    if (!w.is_ok) {
      failure = StorageError("Index backfill failed", w);
      return false;
    }
    return true;
  };
  const DBStatus s = _storage.Scan(
      keys::NamespacePrefix(ns), [&](std::string_view key, ByteView value) {
        auto path = keys::DocumentPathFromKey(ns, key);
        if (!path) {
          failure =
              DbError::DataCorruption("Malformed document key in storage.");
          return false;
        }
        const std::string collection = path->substr(0, path->rfind('/'));
        if (LastSegment(collection) != def.collection_id)
          return true;
        auto doc = DecodeDocument(value, std::move(*path));
        if (!doc) {
          failure = doc.error();
          return false;
        }
        auto entries = IndexEntries(ns, def, collection, *doc);
        if (!entries.empty())
          ++indexed;
        for (auto &k : entries)
          ops.push_back(
              WriteOp::Put(std::move(k), ByteVector(doc->id.begin(),
                                                    doc->id.end())));
        if (ops.size() >= kBackfillChunk)
          return flush();
        return true;
      });
  if (!failure && !s.is_ok)
    failure = StorageError("Index backfill scan failed", s);
  if (failure)
    return std::unexpected(*failure);
  // The definition is written last: an interrupted backfill leaves only
  // orphaned entries, which queries ignore and a re-create overwrites.
  const std::string def_json = def.ToJson().dump();
  ops.push_back(WriteOp::Put(index_keys::DefinitionKey(ns, def.id),
                             ByteVector(def_json.begin(), def_json.end())));
  if (!flush())
    return std::unexpected(*failure);

  {
    std::unique_lock<std::shared_mutex> ilock(_index_mu);
    _indexes[ns].push_back(def);
  }
  Log(LogLevel::Info, "db",
      "Created index " + def.id + " in [" + ns + "] (" +
          std::to_string(indexed) + " documents)");
  return CreateIndexResult{def, true, indexed};
}

std::expected<void, DbError> Database::DropIndex(const std::string &ns,
                                                 const std::string &index_id) {
  std::lock_guard<std::mutex> lock(_commit_mu);
  {
    std::unique_lock<std::shared_mutex> ilock(_index_mu);
    auto it = _indexes.find(ns);
    const bool found =
        it != _indexes.end() &&
        std::erase_if(it->second, [&](const IndexDefinition &d) {
          return d.id == index_id;
        }) > 0;
    if (!found)
      return std::unexpected(
          DbError::NotFound("Index '" + index_id + "' does not exist."));
    if (it->second.empty())
      _indexes.erase(it);
  }
  // No query can pick the index any more; remove definition, then entries.
  std::vector<WriteOp> ops;
  ops.push_back(WriteOp::Delete(index_keys::DefinitionKey(ns, index_id)));
  std::optional<DbError> failure;
  const DBStatus s = _storage.Scan(
      index_keys::IndexPrefix(ns, index_id),
      [&](std::string_view key, ByteView) {
        ops.push_back(WriteOp::Delete(std::string(key)));
        if (ops.size() >= kBackfillChunk) {
          const DBStatus w = _storage.Write(ops);
          ops.clear();
          if (!w.is_ok) {
            failure = StorageError("Index drop failed", w);
            return false;
          }
        }
        return true;
      });
  if (!failure && !s.is_ok)
    failure = StorageError("Index drop scan failed", s);
  if (!failure && !ops.empty()) {
    const DBStatus w = _storage.Write(ops);
    if (!w.is_ok)
      failure = StorageError("Index drop failed", w);
  }
  if (failure)
    return std::unexpected(*failure);
  return {};
}

std::vector<IndexDefinition> Database::ListIndexes(const std::string &ns) {
  std::shared_lock<std::shared_mutex> lock(_index_mu);
  auto it = _indexes.find(ns);
  return it == _indexes.end() ? std::vector<IndexDefinition>{} : it->second;
}

// ----- Resume ----------------------------------------------------------------

bool Database::CanResumeFrom(uint64_t token) const {
  if (_options.log_retention == 0 || token > _sequence)
    return false;
  if (token + 1 < _log_start)
    return false;
  return _sequence - token <= _options.log_retention;
}

std::expected<std::vector<Document>, DbError>
Database::StateAt(const std::string &ns, const ResourcePath &target,
                  uint64_t token) {
  if (!CanResumeFrom(token))
    return std::unexpected(
        DbError::InvalidArgument("resume_token is outside the commit log."));
  const std::string target_path = target.ToString();
  auto relevant = [&](const std::string &path) {
    if (target.IsDocument())
      return path == target_path;
    const size_t slash = path.rfind('/');
    return slash != std::string::npos && path.compare(0, slash, target_path) == 0 &&
           slash == target_path.size();
  };

  // The earliest before-image after `token` is each document's state at
  // `token` (null = did not exist).
  std::map<std::string, std::optional<Document>> at_token;
  std::optional<DbError> failure;
  const DBStatus s = _storage.Iterate(
      LogPrefix(ns), LogKey(ns, token + 1), false,
      [&](std::string_view, ByteView value) {
        json entry;
        try {
          entry = json::from_msgpack(value.begin(), value.end());
        } catch (const std::exception &e) {
          failure = DbError::DataCorruption(std::string("Corrupt commit log: ") +
                                            e.what());
          return false;
        }
        if (!entry.is_object() || !entry.contains("c") ||
            !entry["c"].is_array()) {
          failure = DbError::DataCorruption("Corrupt commit log record.");
          return false;
        }
        for (const auto &c : entry["c"]) {
          if (!c.is_object() || !c.contains("p") || !c["p"].is_string()) {
            failure = DbError::DataCorruption("Corrupt commit log change.");
            return false;
          }
          const std::string path = c["p"].get<std::string>();
          if (!relevant(path) || at_token.contains(path))
            continue;
          if (c["b"].is_binary()) {
            const auto &bin = c["b"].get_binary();
            auto doc = DecodeDocument(ByteView(bin.data(), bin.size()), path);
            if (!doc) {
              failure = doc.error();
              return false;
            }
            at_token.emplace(path, std::move(*doc));
          } else {
            at_token.emplace(path, std::nullopt);
          }
        }
        return true;
      });
  if (failure)
    return std::unexpected(*failure);
  if (!s.is_ok)
    return std::unexpected(StorageError("Commit log scan failed", s));

  std::vector<Document> current;
  if (target.IsDocument()) {
    auto d = ReadFrom(_storage, ns, target);
    if (!d)
      return std::unexpected(d.error());
    if (*d)
      current.push_back(std::move(**d));
  } else {
    auto docs = ScanDocuments(_storage, ns,
                              keys::CollectionDocumentsPrefix(ns, target));
    if (!docs)
      return std::unexpected(docs.error());
    current = std::move(*docs);
  }

  std::vector<Document> result;
  for (auto &d : current)
    if (!at_token.contains(d.path))
      result.push_back(std::move(d));
  for (auto &[path, doc] : at_token)
    if (doc)
      result.push_back(std::move(*doc));
  return result;
}

} // namespace pod
