#include "common/log.h"
#include "common/timestamp.h"
#include "network/ws/handlers/interface.h"
#include "network/ws/wire_protocol.h"
#include "sstream"
#include "storage/interface.h"
#include "string"
#include <boost/uuid/time_generator_v7.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <expected>
#include <leveldb/options.h>
#include <nlohmann/detail/value_t.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;
std::vector<std::string> splitPath(const std::string &path) {
  std::vector<std::string> segments;
  std::stringstream ss(path);
  std::string item;

  while (std::getline(ss, item, '/')) {
    if (!item.empty())
      segments.push_back(item);
  }
  return segments;
}
std::string joinPath(const std::vector<std::string> &segments) {
  int total_size = 0;
  for (const auto &segment : segments)
    total_size += segment.size() + 1; // Acconting for "/" in between

  std::string path;
  // Single static heap allocation
  path.reserve(total_size);
  for (size_t i = 0; i < segments.size(); ++i) {
    path += segments[i];
    if (i < segments.size() - 1)
      path += '/';
  }
  return path;
}
// Insert handler
// std::expected<InsertResponse, PodHandlerError>
// insert_handler(StorageDriver &d, InsertRequest &packet) {
//   std::vector<std::string> path = splitPath(packet.path);
//   // This validates if a path is correct or not.
//   // ['users', 'user'] means user is an ID (document) since array is even. if
//   // ['users', 'user', 'music'] was the path, this means users is a
//   collection,
//   // user is a single document and music is a collection inside user hence we
//   // will need to append data to end of the path to facilitate insertion
//   if (path.size() % 2 == 0) {
//     boost::uuids::time_generator_v7 gen;
//     // Generate the UUID
//     boost::uuids::uuid uuid = gen();
//     // This is a heap allocation on to_string can be optimized but I am not
//     that
//     // smart with C++ as of now
//     path.push_back(boost::uuids::to_string(uuid));
//     Log(LogLevel::Info, "pod/insert_handler",
//         "Recieved `path` for a collection instead of a document. DocumentId("
//         +
//             boost::uuids::to_string(uuid) + ")");
//   }
//   Log(LogLevel::Info, "pod/insert_handler",
//       "Recieved `path` for a document. Attempting to save MessagePack "
//       "contents");

//   // Were saving text based documents in MessagePack binary format
//   std::vector<uint8_t> binary_data = json::to_msgpack(packet.document_data);
//   Log(LogLevel::Info, "pod/insert_handler",
//       "Successfully converted content to MessagePack buffer");

//   const auto write_path = joinPath(path);
//   Log(LogLevel::Info, "pod/insert_handler",
//       "Joining path to get final writePath (" + write_path + ")");
//   const auto db_status = d.Put(write_path, binary_data);
//   if (!db_status.is_ok) {
//     PodHandlerError err;
//     err.reason = "Database write failed: " + db_status.message;
//     Log(LogLevel::Error, "pod/insert_handler",
//         "Database write failed: " + db_status.message);
//     // std::unexpected routes this to the error channel of std::expected
//     return std::unexpected(err);
//   }
//   Log(LogLevel::Info, "pod/insert_handler",
//       "Successfully saved document in database");

//   InsertResponse response = {
//       .meta = {.request_id = packet.meta.request_id,
//                .action = "insert-ack"}, // Echo back the request metadata
//                                         // (important for the client!)
//       .error = 0,                       // 0 means no error
//       .status = 200,                    // HTTP-style success code
//       .path = write_path // Tell the client exactly where it was saved
//   };
//   Log(LogLevel::Info, "pod/insert_handler",
//       "Returning InsertResponse with insert-ack");
//   return response;
// };

// Get handler
std::expected<ReadResponse, ErrorResponse> read_handler(StorageDriver &d,
                                                        ReadRequest &packet) {
  std::vector<std::string> path = splitPath(packet.payload.path);
  /* Invalid document path */
  if (path.size() % 2 == 0)
    return std::unexpected(ErrorResponse{
        .meta =
            AckMetaGeneric{
                .type = "ACK",
                .request_id = packet.meta.request_id,
                .timestamp = current_iso8601_timestamp(),
                .status = 400,
            },
        .payload = ErrorPayload{
            .code = "INVALID_PATH",
            .message = "Got collection path for a document level read.",
        }});
  // Were saving text based documents in MessagePack binary format
  const auto read_path = joinPath(path);
  const auto [db_status, value] = d.Get(read_path);
  if (!db_status.is_ok)
    return std::unexpected(
        ErrorResponse{.meta =
                          AckMetaGeneric{
                              .type = "ACK",
                              .request_id = packet.meta.request_id,
                              .timestamp = current_iso8601_timestamp(),
                              .status = 401,
                          },
                      .payload = ErrorPayload{
                          .code = "INTERNAL_READ_FAILED",
                          .message = db_status.message,
                      }});

  if (db_status.is_not_found)
    return ReadResponse{.meta =
                            AckMetaGeneric{
                                .type = "ACK",
                                .request_id = packet.meta.request_id,
                                .timestamp = current_iso8601_timestamp(),
                                .status = 404,
                            },
                        .payload = ReadResponsePayload{}};

  auto data = json::from_msgpack(value);
  auto r = ReadResponse{.meta =
                            AckMetaGeneric{
                                .type = "ACK",
                                .request_id = packet.meta.request_id,
                                .timestamp = current_iso8601_timestamp(),
                                .status = 200,
                            },
                        .payload = ReadResponsePayload{.document = {
                                                           .path = read_path,
                                                           .data = data,
                                                       }}};
  return r;
};