#include "db/keys.h"

namespace pod::keys {

std::string NamespacePrefix(std::string_view ns) {
  std::string k;
  k.reserve(ns.size() + 3);
  k += 'd';
  k += '\0';
  k += ns;
  k += '\0';
  return k;
}

std::string DocumentKey(std::string_view ns, const ResourcePath &doc) {
  std::string k = NamespacePrefix(ns);
  k += doc.ParentCollection();
  k += '\0';
  k += doc.Id();
  return k;
}

std::string CollectionDocumentsPrefix(std::string_view ns,
                                      const ResourcePath &collection) {
  std::string k = NamespacePrefix(ns);
  k += collection.ToString();
  k += '\0';
  return k;
}

std::string DescendantsPrefix(std::string_view ns, const ResourcePath &path) {
  std::string k = NamespacePrefix(ns);
  k += path.ToString();
  k += '/';
  return k;
}

std::optional<std::string> DocumentPathFromKey(std::string_view ns,
                                               std::string_view key) {
  const std::string prefix = NamespacePrefix(ns);
  if (!key.starts_with(prefix))
    return std::nullopt;
  std::string_view rest = key.substr(prefix.size());
  const size_t sep = rest.find('\0');
  if (sep == std::string_view::npos || sep == 0 || sep + 1 >= rest.size())
    return std::nullopt;
  std::string path(rest.substr(0, sep));
  path += '/';
  path += rest.substr(sep + 1);
  return path;
}

} // namespace pod::keys
