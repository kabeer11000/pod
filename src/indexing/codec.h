#pragma once
// Order-preserving physical key encoding for secondary index entries.
//
// Physical key layout for one index entry:
//   "idx:" + index_id + ":" + <field segments...> + '\0' + doc_path
//
// `doc_path` is appended unescaped purely as a uniqueness tie-breaker (two
// documents with identical indexed values must not collide on one key) and
// as a stable, reproducible suffix so re-indexing the same document is
// idempotent (same content -> same key -> overwrite, not a duplicate). It is
// never parsed back out of the key - the LevelDB value carries doc_path for
// that - so it needs no escaping.
#include "indexing/interface.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace indexing {

// Resolves dot-notation field paths (e.g. "user.emails") against a document,
// matching the projection/where semantics in
// documentation/StructuredQueryFormat.txt. Returns `null` if any segment of
// the path is missing or the document isn't shaped like an object there.
json ResolveFieldPath(const json &document, const std::string &field_path);

// Appends the order-preserving encoding of one field's value onto `out`.
//
// Byte layout per field is [type_tag][payload], where type_tag establishes a
// fixed cross-type total order (Null < False < True < Number < String) for
// documents where the field is missing or a different type across the
// collection:
//   - Number: 8-byte order-preserving IEEE-754 double encoding. Integers are
//     promoted to double (values outside the 53-bit exact range lose
//     precision - a documented v1 limitation). -0.0 is canonicalized to
//     +0.0 first so the two compare equal, as they must.
//   - String: UTF-8 bytes with embedded 0x00 escaped as [0x00 0xFF],
//     followed by a [0x00 0x00] terminator, so concatenating multiple field
//     segments back to back can never be ambiguous at the boundary (e.g.
//     ("a","bc") vs ("ab","c")) without needing to be parsed apart again.
//   - Null / False / True: type tag only, fixed width, no payload.
//   - Array / Object: not indexable as scalars in v1; encoded as their JSON
//     string form so building at least doesn't fail, but no ARRAY.* query
//     operator can use this index against such a field.
//
// If `direction` is Descending, every byte of the *completed* segment (tag +
// payload + terminator, in that order) is bit-inverted, so plain ascending
// byte-order iteration yields descending order for that field. Throws
// std::invalid_argument for NaN, which has no total order.
void EncodeFieldValue(const json &value, OrderByDirection direction,
                      std::string &out);

// The shared prefix of every physical entry belonging to one index -
// "idx:<index_id>:" - usable both to build EncodeIndexKey and to
// ScanPrefix() an index's entries directly (e.g. to delete all of them).
std::string IndexEntryPrefix(const std::string &index_id);

// Builds the full physical LevelDB key for `document`'s entry in the index
// described by `index_id`/`fields`.
std::string EncodeIndexKey(const std::string &index_id,
                           const std::vector<IndexFieldSpec> &fields,
                           const json &document, const std::string &doc_path);

} // namespace indexing
