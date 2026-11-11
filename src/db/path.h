// Resource paths and namespaces.
//
// A path is a '/'-separated list of segments alternating collection / document:
//   users                 -> collection (odd number of segments)
//   users/u1              -> document   (even number of segments)
//   users/u1/posts        -> sub-collection of users/u1
//   users/u1/posts/p1     -> document inside that sub-collection
//
// A namespace (the "scope" of a connection) isolates an entire tree of
// collections: the same path in two namespaces refers to different data.
#pragma once
#include "db/error.h"
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace pod {

inline constexpr size_t kMaxPathSegments = 100;
inline constexpr size_t kMaxSegmentBytes = 1500;
inline constexpr size_t kMaxNamespaceBytes = 64;
inline constexpr std::string_view kFallbackNamespace = "default";

class ResourcePath {
public:
  ResourcePath() = default;
  explicit ResourcePath(std::vector<std::string> segments)
      : _segments(std::move(segments)) {}

  const std::vector<std::string> &Segments() const { return _segments; }
  bool IsDocument() const { return !_segments.empty() && _segments.size() % 2 == 0; }
  bool IsCollection() const { return _segments.size() % 2 == 1; }

  // "users/u1/posts/p1"
  std::string ToString() const;
  // Last segment: the document id or the collection id.
  const std::string &Id() const { return _segments.back(); }
  // Document only: path of the collection holding it ("users/u1/posts").
  std::string ParentCollection() const;
  // Collection only: path of a child document with the given id.
  ResourcePath Child(const std::string &id) const;

private:
  std::vector<std::string> _segments;
};

// Parses and validates a raw path. Leading and trailing '/' are ignored;
// empty interior segments ("a//b"), "." / "..", NUL bytes, invalid UTF-8,
// and reserved "__name__"-style segments are rejected.
std::expected<ResourcePath, DbError> ParsePath(std::string_view raw);
std::expected<ResourcePath, DbError> ParseDocumentPath(std::string_view raw);
std::expected<ResourcePath, DbError> ParseCollectionPath(std::string_view raw);

// Validates a namespace identifier: 1-64 chars of [A-Za-z0-9_-].
std::expected<std::string, DbError> ValidateNamespace(std::string_view ns);

// True if `s` is well-formed UTF-8.
bool IsValidUtf8(std::string_view s);

} // namespace pod
