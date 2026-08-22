#pragma once
#include <cstdint>
#include <nlohmann/detail/macro_scope.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/* ==========================================================================
   Base Envelopes & Discriminators
   ========================================================================== */
using DocumentVersion = uint32_t;
struct RequestMetaGeneric {
  // READ | QUERY | WRITE | DELETE | MUTATE_MANY | SUBSCRIBE | ACK | UNSUBSCRIBE
  // (ACK is client sending a acknoledgement for the subscription EVENT
  // response)
  std::string type;
  std::string request_id;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestMetaGeneric, type, request_id)

struct AckMetaGeneric {
  std::string type = "ACK";
  std::string request_id;
  std::string timestamp;
  uint32_t status;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AckMetaGeneric, type, request_id, timestamp,
                                   type)
struct ErrorPayload {
  std::string code;
  std::string message;
  std::optional<nlohmann::json> details;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ErrorPayload, code, message, details)
struct ErrorResponse {
  AckMetaGeneric meta;
  ErrorPayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ErrorResponse, meta, payload)

/* ==========================================================================
   CRUD & Mutation Operations
   ========================================================================== */

// ========= Read Handler ========= //
struct ReadRequestPayload {
  std::string path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadRequestPayload, path)
struct ReadRequest {
  RequestMetaGeneric meta;
  ReadRequestPayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadRequest, meta, payload)

struct ReadResponsePayloadDocument {
  std::string path;
  std::optional<nlohmann::json> data;
  std::optional<DocumentVersion> _v;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadResponsePayloadDocument, path, data, _v);

struct ReadResponsePayload {
  ReadResponsePayloadDocument document;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadResponsePayload, document);
// meta.status = 404, and document.data = null but no error is thrown on
// document not found
struct ReadResponse {
  AckMetaGeneric meta;
  ReadResponsePayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadResponse, meta, payload)

// ========= Query Handler ========= //
struct WhereFilterItem {
  std::string field;
  std::string op;
  nlohmann::json value;
};
enum class OrderByDirection { Ascending, Descending };
NLOHMANN_JSON_SERIALIZE_ENUM(OrderByDirection,
                             {
                                 {OrderByDirection::Ascending, "ASCENDING"},
                                 {OrderByDirection::Descending, "DESCENDING"},
                             })
struct OrderByClause {
  std::string field;
  OrderByDirection direction;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OrderByClause, field, direction)
using Cursor = std::string;
struct QueryRequestPayload {
  std::string path;
  std::optional<std::vector<std::string>> project;
  std::optional<WhereFilterItem> where;
  std::optional<OrderByClause> orderBy;
  std::optional<uint32_t> limit;
  std::optional<Cursor> cursor;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueryRequestPayload, path, project, where,
                                   orderBy, limit, cursor);

struct QueryRequest {
  RequestMetaGeneric meta;
  QueryRequestPayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueryRequest, meta, payload);
struct QueryResponsePayloadDocumentsItem {
  std::string path;
  nlohmann::json data;
  std::optional<DocumentVersion> _v;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueryResponsePayloadDocumentsItem, path,
                                   data, _v);
struct QueryResponsePayload {
  std::vector<QueryResponsePayloadDocumentsItem> documents;
  bool has_more;
  std::optional<Cursor> next_cursor;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueryResponsePayload, documents, has_more,
                                   next_cursor);
struct QueryResponse {
  AckMetaGeneric meta;
  QueryResponsePayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueryResponse, meta, payload);

// ========= Write Handler ========= //
struct WriteRequestPayload {
  std::string path;
  bool merge;
  nlohmann::json data;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WriteRequestPayload, path, merge, data);
struct WriteRequest {
  RequestMetaGeneric meta;
  WriteRequestPayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WriteRequest, meta, payload);
struct WriteResponsePayload {
  std::string path;
  std::optional<DocumentVersion> _v;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WriteResponsePayload, path, _v);
struct WriteResponse {
  AckMetaGeneric meta;
  WriteResponsePayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WriteResponse, meta, payload);

// ========= Delete Handler ========= //
struct DeleteRequestPayload {
  std::string path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeleteRequestPayload, path);
struct DeleteRequest {
  RequestMetaGeneric meta;
  DeleteRequestPayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeleteRequest, meta, payload);
enum class CollectionOrDocument { Collection, Document };
NLOHMANN_JSON_SERIALIZE_ENUM(CollectionOrDocument,
                             {
                                 {CollectionOrDocument::Collection,
                                  "COLLECTION"},
                                 {CollectionOrDocument::Document, "DOCUMENT"},
                             })

struct DeleteResponsePayload {
  CollectionOrDocument target;
  std::string path;
  uint32_t affected_count;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeleteResponsePayload, target, path,
                                   affected_count);
struct DeleteResponse {
  AckMetaGeneric meta;
  DeleteResponsePayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeleteResponse, meta, payload);

// ========= MutateMany Handler ========= //
struct MutateManyRequestPayload {
    std::string path;
    std::optional<WhereFilterItem> where;
    std::optional<std::vector<std::string>> remove; // field paths to delete
    std::optional<std::pair<std::string, nlohmann::json>> set; // field paths to update value to
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MutateManyRequestPayload, path, where, remove, set);
struct MutateManyRequest {
    RequestMetaGeneric meta;
    MutateManyRequestPayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MutateManyRequest, meta, payload);
struct MutateManyResponsePayload {
    std::string path;
    uint32_t affected_count;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MutateManyResponsePayload, path, affected_count);
struct MutateManyResponse {
    AckMetaGeneric meta;
    MutateManyResponsePayload payload;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MutateManyResponse, meta, payload);
// ========= Subscription Handlers ========= //
