// On-disk document envelope.
//
// Each document is stored as MessagePack of:
//   { "v": <uint32 version>, "c": <create ms>, "u": <update ms>, "d": <data> }
// The version starts at 1 on creation and increases by one on every change;
// it is what the wire protocol exposes as `_v`.
#pragma once
#include "db/error.h"
#include "storage/types.h"
#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <string>

namespace pod {

using DocumentVersion = uint32_t;

// Upper bound on the encoded size of a single document.
inline constexpr size_t kMaxDocumentBytes = 4 * 1024 * 1024;

struct Document {
  std::string path; // full path, e.g. "users/u1"
  std::string id;   // last path segment, e.g. "u1"
  nlohmann::json data = nlohmann::json::object();
  DocumentVersion version = 0;
  int64_t create_time_ms = 0;
  int64_t update_time_ms = 0;
};

ByteVector EncodeDocument(const Document &doc);
// `path`/`id` are not stored in the value; they are supplied by the caller.
std::expected<Document, DbError> DecodeDocument(ByteView bytes,
                                                std::string path);

} // namespace pod
