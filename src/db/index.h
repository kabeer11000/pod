// Secondary indexes ("custom indexing via key-scoping").
//
// An index is declared per namespace for a collection id (the last segment of
// a collection path, so "posts" covers users/a/posts, users/b/posts, ...)
// over an ordered list of fields. Each indexed document contributes keys:
//
//   'i' 0 <ns> 0 <index id> 0 <esc(collection path)> <enc(v1)> ... <esc(doc id)>
//
// and the key's value is the raw document id. enc() is an order-preserving,
// prefix-free ("memcomparable") encoding: comparing encoded bytes agrees with
// CompareValues() except that numbers are compared after rounding to double,
// so distinct integers beyond 2^53 may share a key position. Index scans are
// therefore only a candidate source: every candidate is re-checked against
// the full query, and ties are re-sorted exactly.
//
// A document is indexed only if every indexed field is present (sparse
// index). ARRAY_CONTAINS fields contribute one key per distinct element.
#pragma once
#include "db/document.h"
#include "db/error.h"
#include "db/field_path.h"
#include "db/path.h"
#include "db/query.h"
#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pod {

enum class IndexMode { Value, ArrayContains };

struct IndexField {
  FieldPath path;
  IndexMode mode = IndexMode::Value;
};

struct IndexDefinition {
  std::string id;
  std::string collection_id;
  std::vector<IndexField> fields;

  nlohmann::json ToJson() const;
};

inline constexpr size_t kMaxIndexFields = 8;

// Validates a definition from its wire/JSON form and derives its id.
std::expected<IndexDefinition, DbError>
MakeIndexDefinition(const std::string &collection_id,
                    const nlohmann::json &fields);
std::expected<IndexDefinition, DbError>
IndexDefinitionFromJson(const nlohmann::json &j);

namespace index_keys {
// Storage key of an index definition.
std::string DefinitionKey(std::string_view ns, std::string_view index_id);
std::string DefinitionsPrefix(std::string_view ns);
// All entries of one index.
std::string IndexPrefix(std::string_view ns, std::string_view index_id);
// Entries of one index restricted to one collection path.
std::string CollectionPrefix(std::string_view ns, std::string_view index_id,
                             std::string_view collection_path);
} // namespace index_keys

// Order-preserving encodings.
void EncodeEscaped(std::string &out, std::string_view bytes);
void EncodeIndexValue(std::string &out, const nlohmann::json &value);

// All index keys a document contributes (empty when not indexed).
std::vector<std::string> IndexEntries(std::string_view ns,
                                      const IndexDefinition &def,
                                      std::string_view collection_path,
                                      const Document &doc);

} // namespace pod
