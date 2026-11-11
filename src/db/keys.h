// Storage key layout.
//
// Every document lives under a key scoped by its namespace:
//
//   'd' 0x00 <namespace> 0x00 <collection path> 0x00 <document id>
//
// e.g. users/u1/posts/p1 in namespace "acme" is stored at
//   "d\0acme\0users/u1/posts\0p1"
//
// Because segments can never contain '/' or NUL, this gives us three cheap,
// exact prefix scans:
//   * direct documents of a collection:   d\0ns\0users\0
//   * everything nested under a collection: d\0ns\0users/
//   * everything nested under a document:   d\0ns\0users/u1/
// and namespaces can never observe each other's keys.
//
// System metadata lives under the 's' 0x00 prefix, which no document key uses.
#pragma once
#include "db/path.h"
#include <optional>
#include <string>
#include <string_view>

namespace pod::keys {

inline constexpr std::string_view kCommitSequenceKey{"s\0commit_seq", 12};

std::string NamespacePrefix(std::string_view ns);
std::string DocumentKey(std::string_view ns, const ResourcePath &doc);
// Prefix matching the documents directly inside a collection.
std::string CollectionDocumentsPrefix(std::string_view ns,
                                      const ResourcePath &collection);
// Prefix matching every document in sub-collections nested (at any depth)
// under the given collection or document.
std::string DescendantsPrefix(std::string_view ns, const ResourcePath &path);

// Reverses DocumentKey(): returns the document path ("users/u1") for a key
// under the given namespace, or nullopt if the key is malformed.
std::optional<std::string> DocumentPathFromKey(std::string_view ns,
                                               std::string_view key);

} // namespace pod::keys
