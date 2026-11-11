// Wire protocol (see protocol/defination.ts for the canonical TypeScript
// definition). Requests are parsed and validated by hand rather than through
// NLOHMANN_DEFINE_TYPE_* so that optional fields, `null`, and malformed input
// all produce precise INVALID_ARGUMENT errors instead of exceptions.
#pragma once
#include "db/error.h"
#include "db/field_path.h"
#include "db/index.h"
#include "db/path.h"
#include "db/query.h"
#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pod::wire {

using json = nlohmann::json;

struct ReadRequest {
  ResourcePath path;
};

struct QueryRequest {
  ResourcePath path;
  QuerySpec spec;
  std::optional<std::string> cursor;
};

struct WriteRequest {
  ResourcePath path;
  bool merge = false;
  json data;
};

struct DeleteRequest {
  ResourcePath path;
};

struct MutateManyRequest {
  ResourcePath path;
  std::vector<Filter> where;
  std::vector<FieldPath> remove;
  std::vector<std::pair<FieldPath, json>> set;
};

struct SubscribeRequest {
  std::string subscription_id;
  ResourcePath path;
  QuerySpec spec;
  std::optional<std::string> resume_token;
};

struct UnsubscribeRequest {
  std::string subscription_id;
};

// Client acknowledgement of an EVENT (meta.type == "ACK").
struct ClientEventAck {
  std::string subscription_id;
  std::string resume_token;
};

// Extracts meta.type / meta.request_id. request_id is empty when absent (it
// is not part of a client ACK).
struct RequestMeta {
  std::string type;
  std::string request_id;
};
std::expected<RequestMeta, DbError> ParseMeta(const json &message);

std::expected<ReadRequest, DbError> ParseRead(const json &message);
std::expected<QueryRequest, DbError> ParseQuery(const json &message);
std::expected<WriteRequest, DbError> ParseWrite(const json &message);
std::expected<DeleteRequest, DbError> ParseDelete(const json &message);
std::expected<MutateManyRequest, DbError> ParseMutateMany(const json &message);
std::expected<SubscribeRequest, DbError> ParseSubscribe(const json &message);
std::expected<UnsubscribeRequest, DbError>
ParseUnsubscribe(const json &message);
std::expected<ClientEventAck, DbError> ParseClientAck(const json &message);
std::expected<IndexDefinition, DbError> ParseCreateIndex(const json &message);
// Returns the index id.
std::expected<std::string, DbError> ParseDropIndex(const json &message);

// { type: "ACK", request_id, timestamp, status }
json AckMeta(const std::string &request_id, uint32_t status);
// { meta: { type: "ERROR", ... }, payload: { code, message, details? } }
json ErrorMessage(const std::string &request_id, const DbError &error);
// { path, data, _v } with optional projection applied.
json DocumentJson(const Document &doc, const std::vector<FieldPath> &project);

} // namespace pod::wire
