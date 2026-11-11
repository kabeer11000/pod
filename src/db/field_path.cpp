#include "db/field_path.h"
#include "db/path.h"
#include <algorithm>

namespace pod {

using json = nlohmann::json;

static constexpr size_t kMaxFieldDepth = 64;

std::expected<FieldPath, DbError> ParseFieldPath(std::string_view raw) {
  if (raw.empty())
    return std::unexpected(
        DbError::InvalidArgument("Field path must not be empty."));
  if (!IsValidUtf8(raw))
    return std::unexpected(
        DbError::InvalidArgument("Field path must be valid UTF-8."));
  FieldPath out;
  size_t pos = 0;
  while (true) {
    const size_t dot = raw.find('.', pos);
    const std::string_view seg = raw.substr(
        pos, dot == std::string_view::npos ? std::string_view::npos
                                           : dot - pos);
    if (seg.empty())
      return std::unexpected(DbError::InvalidArgument(
          "Field path '" + std::string(raw) + "' contains an empty segment."));
    out.emplace_back(seg);
    if (out.size() > kMaxFieldDepth)
      return std::unexpected(DbError::InvalidArgument(
          "Field path '" + std::string(raw) + "' is nested too deeply."));
    if (dot == std::string_view::npos)
      break;
    pos = dot + 1;
  }
  return out;
}

std::string FieldPathToString(const FieldPath &path) {
  std::string s;
  for (size_t i = 0; i < path.size(); ++i) {
    if (i)
      s += '.';
    s += path[i];
  }
  return s;
}

const json *GetField(const json &obj, const FieldPath &path) {
  const json *cur = &obj;
  for (const auto &seg : path) {
    if (!cur->is_object())
      return nullptr;
    auto it = cur->find(seg);
    if (it == cur->end())
      return nullptr;
    cur = &*it;
  }
  return cur;
}

void SetField(json &obj, const FieldPath &path, json value) {
  if (path.empty()) {
    obj = std::move(value);
    return;
  }
  json *cur = &obj;
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    if (!cur->is_object())
      *cur = json::object();
    json &next = (*cur)[path[i]];
    if (!next.is_object())
      next = json::object();
    cur = &next;
  }
  if (!cur->is_object())
    *cur = json::object();
  (*cur)[path.back()] = std::move(value);
}

bool RemoveField(json &obj, const FieldPath &path) {
  if (path.empty())
    return false;
  json *cur = &obj;
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    if (!cur->is_object())
      return false;
    auto it = cur->find(path[i]);
    if (it == cur->end())
      return false;
    cur = &*it;
  }
  if (!cur->is_object())
    return false;
  return cur->erase(path.back()) > 0;
}

json Project(const json &data, const std::vector<FieldPath> &fields) {
  if (fields.empty())
    return data;
  // Overlapping paths ("a" and "a.b") are safe in any order: both copy from
  // the same source, so the nested value written last is always identical.
  json out = json::object();
  for (const FieldPath &f : fields)
    if (const json *v = GetField(data, f))
      SetField(out, f, *v);
  return out;
}

void DeepMerge(json &target, const json &patch) {
  if (!patch.is_object() || !target.is_object()) {
    target = patch;
    return;
  }
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    if (it.value().is_object()) {
      json &slot = target[it.key()];
      if (slot.is_object())
        DeepMerge(slot, it.value());
      else
        slot = it.value();
    } else {
      target[it.key()] = it.value();
    }
  }
}

bool FieldPathsOverlap(const FieldPath &a, const FieldPath &b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

} // namespace pod
