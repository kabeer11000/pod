#include "db/document.h"
#include <exception>
#include <limits>

namespace pod {

using json = nlohmann::json;

ByteVector EncodeDocument(const Document &doc) {
  json envelope = json::object();
  envelope["v"] = doc.version;
  envelope["c"] = doc.create_time_ms;
  envelope["u"] = doc.update_time_ms;
  envelope["d"] = doc.data;
  return json::to_msgpack(envelope);
}

std::expected<Document, DbError> DecodeDocument(ByteView bytes,
                                                std::string path) {
  json envelope;
  try {
    envelope = json::from_msgpack(bytes.begin(), bytes.end());
  } catch (const std::exception &e) {
    return std::unexpected(DbError::DataCorruption(
        "Stored document '" + path + "' is not valid MessagePack: " +
        e.what()));
  }
  if (!envelope.is_object() || !envelope.contains("v") ||
      !envelope.contains("d") || !envelope["v"].is_number_unsigned() ||
      !envelope["d"].is_object())
    return std::unexpected(DbError::DataCorruption(
        "Stored document '" + path + "' has an unexpected format."));

  const auto v = envelope["v"].get<uint64_t>();
  if (v == 0 || v > std::numeric_limits<DocumentVersion>::max())
    return std::unexpected(DbError::DataCorruption(
        "Stored document '" + path + "' has an invalid version."));

  Document doc;
  doc.version = static_cast<DocumentVersion>(v);
  if (envelope.contains("c") && envelope["c"].is_number_integer())
    doc.create_time_ms = envelope["c"].get<int64_t>();
  if (envelope.contains("u") && envelope["u"].is_number_integer())
    doc.update_time_ms = envelope["u"].get<int64_t>();
  doc.data = std::move(envelope["d"]);
  const size_t slash = path.rfind('/');
  doc.id = slash == std::string::npos ? path : path.substr(slash + 1);
  doc.path = std::move(path);
  return doc;
}

} // namespace pod
