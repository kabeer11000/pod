#include "db/path.h"
#include <cstdint>

namespace pod {

std::string ResourcePath::ToString() const {
  std::string out;
  size_t total = 0;
  for (const auto &s : _segments)
    total += s.size() + 1;
  out.reserve(total);
  for (size_t i = 0; i < _segments.size(); ++i) {
    if (i)
      out += '/';
    out += _segments[i];
  }
  return out;
}

std::string ResourcePath::ParentCollection() const {
  std::string out;
  for (size_t i = 0; i + 1 < _segments.size(); ++i) {
    if (i)
      out += '/';
    out += _segments[i];
  }
  return out;
}

ResourcePath ResourcePath::Child(const std::string &id) const {
  auto segs = _segments;
  segs.push_back(id);
  return ResourcePath(std::move(segs));
}

bool IsValidUtf8(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const auto c = static_cast<uint8_t>(s[i]);
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > n)
      return false;
    for (size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

static std::expected<void, DbError> ValidateSegment(std::string_view seg) {
  if (seg.empty())
    return std::unexpected(
        DbError::InvalidPath("Path contains an empty segment."));
  if (seg.size() > kMaxSegmentBytes)
    return std::unexpected(DbError::InvalidPath(
        "Path segment exceeds " + std::to_string(kMaxSegmentBytes) +
        " bytes."));
  if (seg == "." || seg == "..")
    return std::unexpected(
        DbError::InvalidPath("Path segments may not be '.' or '..'."));
  if (seg.size() >= 4 && seg.starts_with("__") && seg.ends_with("__"))
    return std::unexpected(DbError::InvalidPath(
        "Path segments of the form __name__ are reserved."));
  if (seg.find('\0') != std::string_view::npos)
    return std::unexpected(
        DbError::InvalidPath("Path segments may not contain NUL bytes."));
  if (!IsValidUtf8(seg))
    return std::unexpected(
        DbError::InvalidPath("Path segments must be valid UTF-8."));
  return {};
}

std::expected<ResourcePath, DbError> ParsePath(std::string_view raw) {
  size_t begin = 0, end = raw.size();
  while (begin < end && raw[begin] == '/')
    ++begin;
  while (end > begin && raw[end - 1] == '/')
    --end;
  const std::string_view body = raw.substr(begin, end - begin);
  if (body.empty())
    return std::unexpected(DbError::InvalidPath("Path must not be empty."));

  std::vector<std::string> segments;
  size_t pos = 0;
  while (true) {
    const size_t slash = body.find('/', pos);
    const std::string_view seg = body.substr(
        pos, slash == std::string_view::npos ? std::string_view::npos
                                             : slash - pos);
    if (auto ok = ValidateSegment(seg); !ok)
      return std::unexpected(ok.error());
    segments.emplace_back(seg);
    if (segments.size() > kMaxPathSegments)
      return std::unexpected(DbError::InvalidPath(
          "Path exceeds " + std::to_string(kMaxPathSegments) + " segments."));
    if (slash == std::string_view::npos)
      break;
    pos = slash + 1;
  }
  return ResourcePath(std::move(segments));
}

std::expected<ResourcePath, DbError> ParseDocumentPath(std::string_view raw) {
  auto p = ParsePath(raw);
  if (!p)
    return p;
  if (!p->IsDocument())
    return std::unexpected(DbError::InvalidPath(
        "Expected a document path (even number of segments), got a "
        "collection path."));
  return p;
}

std::expected<ResourcePath, DbError>
ParseCollectionPath(std::string_view raw) {
  auto p = ParsePath(raw);
  if (!p)
    return p;
  if (!p->IsCollection())
    return std::unexpected(DbError::InvalidPath(
        "Expected a collection path (odd number of segments), got a "
        "document path."));
  return p;
}

std::expected<std::string, DbError> ValidateNamespace(std::string_view ns) {
  if (ns.empty())
    return std::unexpected(
        DbError::InvalidArgument("Namespace must not be empty."));
  if (ns.size() > kMaxNamespaceBytes)
    return std::unexpected(DbError::InvalidArgument(
        "Namespace exceeds " + std::to_string(kMaxNamespaceBytes) +
        " characters."));
  for (char c : ns) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok)
      return std::unexpected(DbError::InvalidArgument(
          "Namespace may only contain letters, digits, '_' and '-'."));
  }
  return std::string(ns);
}

} // namespace pod
