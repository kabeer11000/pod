// Request handlers for a Pod (single database node).
#include "common/log.h"
#include "common/timestamp.h"
#include "network/ws/handlers/interface.h"
#include "network/ws/wire_protocol.h"
#include <exception>

namespace pod {

using json = nlohmann::json;
using rules::Op;

RequestHandler::RequestHandler(Database &db, SubscriptionManager &subscriptions,
                               const rules::Authorizer &authorizer,
                               MessageSink sink)
    : _db(db), _subs(subscriptions), _authz(authorizer),
      _sink(std::move(sink)) {}

void RequestHandler::Handle(const ConnectionInfo &conn, std::string_view raw,
                            WireEncoding encoding) {
  json message;
  try {
    message = encoding == WireEncoding::Json
                  ? json::parse(raw)
                  : json::from_msgpack(raw.begin(), raw.end());
  } catch (const std::exception &e) {
    Log(LogLevel::Warn, "pod",
        "Malformed message on (" + conn.conn_id + "): " + e.what());
    _sink(conn.conn_id,
          wire::ErrorMessage("", DbError{400, "MALFORMED_REQUEST",
                                         "Message could not be decoded.",
                                         std::nullopt}),
          encoding);
    return;
  }

  auto meta = wire::ParseMeta(message);
  if (!meta) {
    // Echo whatever request_id we can find so the client can correlate.
    std::string rid;
    if (message.is_object() && message.contains("meta") &&
        message["meta"].is_object() &&
        message["meta"].contains("request_id") &&
        message["meta"]["request_id"].is_string())
      rid = message["meta"]["request_id"].get<std::string>();
    _sink(conn.conn_id, wire::ErrorMessage(rid, meta.error()), encoding);
    return;
  }

  Log(LogLevel::Debug, "pod",
      "(" + conn.conn_id + ") [" + conn.auth.ns + "] " + meta->type + " " +
          meta->request_id);

  std::optional<json> response;
  try {
    response =
        Dispatch(conn, message, meta->type, meta->request_id, encoding);
  } catch (const std::exception &e) {
    Log(LogLevel::Error, "pod",
        "Unhandled error processing " + meta->type + ": " + e.what());
    response = wire::ErrorMessage(meta->request_id,
                                  DbError::Internal("Internal server error."));
  }
  if (response)
    _sink(conn.conn_id, std::move(*response), encoding);
}

std::optional<json> RequestHandler::Dispatch(const ConnectionInfo &conn,
                                             const json &message,
                                             const std::string &type,
                                             const std::string &request_id,
                                             WireEncoding encoding) {
  auto error = [&](const DbError &e) {
    return std::optional<json>(wire::ErrorMessage(request_id, e));
  };
  const std::string &ns = conn.auth.ns;

  if (type == "ACK") {
    // Client acknowledgement of an EVENT; the protocol defines no reply.
    auto ack = wire::ParseClientAck(message);
    if (!ack) {
      Log(LogLevel::Warn, "pod",
          "Invalid client ACK on (" + conn.conn_id +
              "): " + ack.error().message);
      return std::nullopt;
    }
    auto ok = _subs.Acknowledge(conn.conn_id, ack->subscription_id,
                                ack->resume_token);
    if (!ok)
      Log(LogLevel::Warn, "pod",
          "Client ACK on (" + conn.conn_id + ") rejected: " +
              ok.error().message);
    return std::nullopt;
  }

  // An expired token must be refreshed by reconnecting.
  if (conn.auth.Expired(current_unix_millis() / 1000))
    return error(DbError::Unauthenticated(
        "The connection's token has expired; reconnect with a new token."));

  const ChangeGuard guard = _authz.MakeGuard(conn.auth);
  const ChangeGuard *guard_ptr = guard ? &guard : nullptr;

  if (type == "READ") {
    auto req = wire::ParseRead(message);
    if (!req)
      return error(req.error());
    auto doc = _db.Read(ns, req->path);
    if (!doc)
      return error(doc.error());
    const std::string path = req->path.ToString();
    if (!_authz.Allows(conn.auth, Op::Get, path, *doc ? &**doc : nullptr,
                       nullptr))
      return error(DbError::PermissionDenied("Missing permission to get '" +
                                             path + "'."));
    json document;
    if (*doc)
      document = wire::DocumentJson(**doc, {});
    else
      document = json{{"path", path}, {"data", nullptr}, {"_v", nullptr}};
    // A missing document is not an error: status 404 with data = null.
    return json{{"meta", wire::AckMeta(request_id, *doc ? 200 : 404)},
                {"payload", {{"document", std::move(document)}}}};
  }

  if (type == "QUERY") {
    auto req = wire::ParseQuery(message);
    if (!req)
      return error(req.error());
    // Security rules act as an additional filter: results are exactly the
    // query over the documents this client may list.
    const Visibility visible = _authz.MakeVisibility(conn.auth, Op::List);
    auto page = _db.Query(ns, req->path, req->spec, req->cursor,
                          visible ? &visible : nullptr);
    if (!page)
      return error(page.error());
    json docs = json::array();
    for (const auto &d : page->documents)
      docs.push_back(wire::DocumentJson(d, req->spec.project));
    return json{{"meta", wire::AckMeta(request_id, 200)},
                {"payload",
                 {{"documents", std::move(docs)},
                  {"has_more", page->has_more},
                  {"next_cursor", page->next_cursor
                                      ? json(*page->next_cursor)
                                      : json(nullptr)},
                  {"plan",
                   {{"index", page->index_used ? json(*page->index_used)
                                               : json(nullptr)},
                    {"documents_scanned", page->documents_scanned}}}}}};
  }

  if (type == "WRITE") {
    auto req = wire::ParseWrite(message);
    if (!req)
      return error(req.error());
    auto result = _db.Write(ns, req->path, req->data, req->merge, guard_ptr);
    if (!result)
      return error(result.error());
    return json{
        {"meta", wire::AckMeta(request_id, result->created ? 201 : 200)},
        {"payload", {{"path", result->path}, {"_v", result->version}}}};
  }

  if (type == "DELETE") {
    auto req = wire::ParseDelete(message);
    if (!req)
      return error(req.error());
    auto result = _db.Delete(ns, req->path, guard_ptr);
    if (!result)
      return error(result.error());
    // Deleting a missing document is still subject to the delete rule.
    if (!result->is_collection && result->affected_count == 0 &&
        !_authz.Allows(conn.auth, Op::Delete, result->path, nullptr, nullptr))
      return error(DbError::PermissionDenied(
          "Missing permission to delete '" + result->path + "'."));
    return json{{"meta", wire::AckMeta(request_id, 200)},
                {"payload",
                 {{"target", result->is_collection ? "COLLECTION" : "DOCUMENT"},
                  {"path", result->path},
                  {"affected_count", result->affected_count}}}};
  }

  if (type == "MUTATE_MANY") {
    auto req = wire::ParseMutateMany(message);
    if (!req)
      return error(req.error());
    auto result = _db.MutateMany(ns, req->path, req->where, req->remove,
                                 req->set, guard_ptr);
    if (!result)
      return error(result.error());
    return json{{"meta", wire::AckMeta(request_id, 200)},
                {"payload",
                 {{"path", result->path},
                  {"affected_count", result->affected_count}}}};
  }

  if (type == "CREATE_INDEX" || type == "DROP_INDEX" ||
      type == "LIST_INDEXES") {
    if (!_authz.IsAdmin(conn.auth))
      return error(DbError::PermissionDenied(
          "Index management requires an admin token."));
    if (type == "CREATE_INDEX") {
      auto req = wire::ParseCreateIndex(message);
      if (!req)
        return error(req.error());
      auto result = _db.CreateIndex(ns, *req);
      if (!result)
        return error(result.error());
      return json{
          {"meta", wire::AckMeta(request_id, result->created ? 201 : 200)},
          {"payload",
           {{"index", result->index.ToJson()},
            {"documents_indexed", result->documents_indexed}}}};
    }
    if (type == "DROP_INDEX") {
      auto id = wire::ParseDropIndex(message);
      if (!id)
        return error(id.error());
      auto ok = _db.DropIndex(ns, *id);
      if (!ok)
        return error(ok.error());
      return json{{"meta", wire::AckMeta(request_id, 200)},
                  {"payload", {{"id", *id}, {"status", "DROPPED"}}}};
    }
    json list = json::array();
    for (const auto &d : _db.ListIndexes(ns))
      list.push_back(d.ToJson());
    return json{{"meta", wire::AckMeta(request_id, 200)},
                {"payload", {{"indexes", std::move(list)}}}};
  }

  if (type == "SUBSCRIBE") {
    auto req = wire::ParseSubscribe(message);
    if (!req)
      return error(req.error());
    const bool is_document = req->path.IsDocument();
    if (is_document) {
      // A document listener requires permission to get the document now.
      auto doc = _db.Read(ns, req->path);
      if (!doc)
        return error(doc.error());
      const std::string path = req->path.ToString();
      if (!_authz.Allows(conn.auth, Op::Get, path, *doc ? &**doc : nullptr,
                         nullptr))
        return error(DbError::PermissionDenied(
            "Missing permission to get '" + path + "'."));
    }
    json ack{{"meta", wire::AckMeta(request_id, 200)},
             {"payload",
              {{"subscription_id", req->subscription_id},
               {"status", "SUBSCRIBED"}}}};
    auto ok = _subs.Subscribe(
        conn.conn_id, ns, encoding, std::move(ack),
        SubscribeParams{std::move(req->subscription_id), std::move(req->path),
                        std::move(req->spec), std::move(req->resume_token),
                        _authz.MakeVisibility(conn.auth, is_document
                                                             ? Op::Get
                                                             : Op::List)});
    if (!ok)
      return error(ok.error());
    return std::nullopt; // ACK and events were sent by the manager in order
  }

  if (type == "UNSUBSCRIBE") {
    auto req = wire::ParseUnsubscribe(message);
    if (!req)
      return error(req.error());
    auto ok = _subs.Unsubscribe(conn.conn_id, req->subscription_id);
    if (!ok)
      return error(ok.error());
    return json{{"meta", wire::AckMeta(request_id, 200)},
                {"payload",
                 {{"subscription_id", req->subscription_id},
                  {"status", "UNSUBSCRIBED"}}}};
  }

  return error(DbError{400, "UNKNOWN_TYPE",
                       "Unknown request type '" + type + "'.", std::nullopt});
}

} // namespace pod
