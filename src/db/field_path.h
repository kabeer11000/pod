// Dot-notation field paths ("user.emails", "meta.created_at") and the JSON
// manipulation built on them: lookup, set, remove, projection, deep merge.
#pragma once
#include "db/error.h"
#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pod {

using FieldPath = std::vector<std::string>;

// Splits on '.', rejecting empty paths and empty segments ("a..b", ".a").
std::expected<FieldPath, DbError> ParseFieldPath(std::string_view raw);
std::string FieldPathToString(const FieldPath &path);

// Returns a pointer to the value at `path`, or nullptr when any segment is
// missing or traverses a non-object.
const nlohmann::json *GetField(const nlohmann::json &obj, const FieldPath &path);
// Sets the value at `path`, creating intermediate objects and replacing any
// intermediate non-object value.
void SetField(nlohmann::json &obj, const FieldPath &path, nlohmann::json value);
// Removes the value at `path`. Returns true if something was removed.
bool RemoveField(nlohmann::json &obj, const FieldPath &path);

// Builds a new object containing only the given fields (missing ones are
// skipped). An empty `fields` list returns `data` unchanged.
nlohmann::json Project(const nlohmann::json &data,
                       const std::vector<FieldPath> &fields);

// Recursively merges `patch` into `target`: objects merge key by key, any
// other value (including null and arrays) replaces the existing value.
void DeepMerge(nlohmann::json &target, const nlohmann::json &patch);

// True if `a` equals `b` or one is a dotted prefix of the other.
bool FieldPathsOverlap(const FieldPath &a, const FieldPath &b);

} // namespace pod
