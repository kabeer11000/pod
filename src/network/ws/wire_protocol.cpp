#include "network/ws/wire_protocol.h"
#include "common/timestamp.h"
#include <limits>

namespace pod::wire {

namespace {

// Treats a missing key and an explicit `null` identically.
const json *Optional(const json &obj, const char *key) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null())
    return nullptr;
  return &*it;
}

std::expected<const json *, DbError> Payload(const json &message) {
  const json *p = message.is_object() ? Optional(message, "payload") : nullptr;
  if (!p || !p->is_object())
    return std::unexpected(
        DbError::InvalidArgument("'payload' must be an object."));
  return p;
}

std::expected<std::string, DbError> RequiredString(const json &obj,
                                                   const char *key) {
  const json *v = Optional(obj, key);
  if (!v || !v->is_string())
    return std::unexpected(DbError::InvalidArgument(
        std::string("'") + key + "' must be a string."));
  return v->get<std::string>();
}

std::expected<std::optional<std::string>, DbError>
OptionalString(const json &obj, const char *key) {
  const json *v = Optional(obj, key);
  if (!v)
    return std::optional<std::string>{};
  if (!v->is_string())
    return std::unexpected(DbError::InvalidArgument(
        std::string("'") + key + "' must be a string."));
  return std::optional<std::string>{v->get<std::string>()};
}

// Accepts either an array of objects or (for convenience) a single object.
std::expected<std::vector<const json *>, DbError>
ObjectList(const json &obj, const char *key) {
  std::vector<const json *> out;
  const json *v = Optional(obj, key);
  if (!v)
    return out;
  if (v->is_object()) {
    out.push_back(v);
    return out;
  }
  if (!v->is_array())
    return std::unexpected(DbError::InvalidArgument(
        std::string("'") + key + "' must be an array of objects."));
  for (const auto &e : *v) {
    if (!e.is_object())
      return std::unexpected(DbError::InvalidArgument(
          std::string("'") + key + "' must be an array of objects."));
    out.push_back(&e);
  }
  return out;
}

std::expected<std::vector<Filter>, DbError> ParseWhere(const json &payload) {
  auto items = ObjectList(payload, "where");
  if (!items)
    return std::unexpected(items.error());
  std::vector<Filter> out;
  for (const json *item : *items) {
    auto field = RequiredString(*item, "field");
    if (!field)
      return std::unexpected(DbError::InvalidArgument(
          "where." + field.error().message));
    auto op = RequiredString(*item, "op");
    if (!op)
      return std::unexpected(DbError::InvalidArgument(
          "where." + op.error().message));
    // `value` may legitimately be null (match null fields), so read it raw.
    auto vit = item->find("value");
    if (vit == item->end())
      return std::unexpected(
          DbError::InvalidArgument("where.'value' is required."));
    auto f = MakeFilter(*field, *op, *vit);
    if (!f)
      return std::unexpected(f.error());
    out.push_back(std::move(*f));
  }
  return out;
}

std::expected<std::vector<OrderBy>, DbError>
ParseOrderBy(const json &payload) {
  auto items = ObjectList(payload, "orderBy");
  if (!items)
    return std::unexpected(items.error());
  std::vector<OrderBy> out;
  for (const json *item : *items) {
    auto field = RequiredString(*item, "field");
    if (!field)
      return std::unexpected(DbError::InvalidArgument(
          "orderBy." + field.error().message));
    auto dir = OptionalString(*item, "direction");
    if (!dir)
      return std::unexpected(dir.error());
    auto o = MakeOrderBy(*field, dir->value_or("ASCENDING"));
    if (!o)
      return std::unexpected(o.error());
    out.push_back(std::move(*o));
  }
  return out;
}

std::expected<std::vector<FieldPath>, DbError>
ParseFieldPathList(const json &payload, const char *key) {
  std::vector<FieldPath> out;
  const json *v = Optional(payload, key);
  if (!v)
    return out;
  if (!v->is_array())
    return std::unexpected(DbError::InvalidArgument(
        std::string("'") + key + "' must be an array of field paths."));
  for (const auto &e : *v) {
    if (!e.is_string())
      return std::unexpected(DbError::InvalidArgument(
          std::string("'") + key + "' must be an array of field paths."));
    auto fp = ParseFieldPath(e.get<std::string>());
    if (!fp)
      return std::unexpected(fp.error());
    out.push_back(std::move(*fp));
  }
  return out;
}

std::expected<std::optional<uint32_t>, DbError> ParseLimit(const json &payload) {
  const json *v = Optional(payload, "limit");
  if (!v)
    return std::optional<uint32_t>{};
  bool ok = false;
  uint64_t n = 0;
  if (v->is_number_unsigned()) {
    n = v->get<uint64_t>();
    ok = true;
  } else if (v->is_number_integer()) {
    ok = v->get<int64_t>() > 0;
    if (ok)
      n = static_cast<uint64_t>(v->get<int64_t>());
  } else if (v->is_number_float()) {
    // JavaScript clients may send integral numbers as doubles (e.g. 10.0).
    const double d = v->get<double>();
    ok = d >= 1 && d <= std::numeric_limits<uint32_t>::max() &&
         static_cast<double>(static_cast<uint64_t>(d)) == d;
    if (ok)
      n = static_cast<uint64_t>(d);
  }
  if (!ok || n == 0 || n > std::numeric_limits<uint32_t>::max())
    return std::unexpected(DbError::InvalidArgument(
        "'limit' must be a positive integer no greater than 4294967295."));
  return std::optional<uint32_t>{static_cast<uint32_t>(n)};
}

std::expected<QuerySpec, DbError> ParseSpec(const json &payload) {
  QuerySpec spec;
  auto where = ParseWhere(payload);
  if (!where)
    return std::unexpected(where.error());
  spec.where = std::move(*where);
  auto order = ParseOrderBy(payload);
  if (!order)
    return std::unexpected(order.error());
  spec.order_by = std::move(*order);
  auto limit = ParseLimit(payload);
  if (!limit)
    return std::unexpected(limit.error());
  spec.limit = *limit;
  auto project = ParseFieldPathList(payload, "project");
  if (!project)
    return std::unexpected(project.error());
  spec.project = std::move(*project);
  return spec;
}

template <auto ParseFn>
std::expected<ResourcePath, DbError> PathField(const json &payload) {
  auto raw = RequiredString(payload, "path");
  if (!raw)
    return std::unexpected(raw.error());
  return ParseFn(*raw);
}

} // namespace

std::expected<RequestMeta, DbError> ParseMeta(const json &message) {
  if (!message.is_object())
    return std::unexpected(
        DbError::InvalidArgument("Message must be an object."));
  const json *meta = Optional(message, "meta");
  if (!meta || !meta->is_object())
    return std::unexpected(
        DbError::InvalidArgument("'meta' must be an object."));
  RequestMeta out;
  if (const json *rid = Optional(*meta, "request_id"); rid && rid->is_string())
    out.request_id = rid->get<std::string>();
  auto type = RequiredString(*meta, "type");
  if (!type)
    return std::unexpected(
        DbError::InvalidArgument("'meta.type' must be a string."));
  out.type = std::move(*type);
  if (out.type != "ACK" && out.request_id.empty())
    return std::unexpected(DbError::InvalidArgument(
        "'meta.request_id' must be a non-empty string."));
  return out;
}

std::expected<ReadRequest, DbError> ParseRead(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto path = PathField<ParseDocumentPath>(**p);
  if (!path)
    return std::unexpected(path.error());
  return ReadRequest{std::move(*path)};
}

std::expected<QueryRequest, DbError> ParseQuery(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto path = PathField<ParseCollectionPath>(**p);
  if (!path)
    return std::unexpected(path.error());
  auto spec = ParseSpec(**p);
  if (!spec)
    return std::unexpected(spec.error());
  auto cursor = OptionalString(**p, "cursor");
  if (!cursor)
    return std::unexpected(cursor.error());
  if (*cursor && (*cursor)->empty())
    return std::unexpected(
        DbError::InvalidCursor("'cursor' must not be empty."));
  return QueryRequest{std::move(*path), std::move(*spec), std::move(*cursor)};
}

std::expected<WriteRequest, DbError> ParseWrite(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto path = PathField<ParseDocumentPath>(**p);
  if (!path)
    return std::unexpected(path.error());
  WriteRequest req;
  req.path = std::move(*path);
  if (const json *m = Optional(**p, "merge")) {
    if (!m->is_boolean())
      return std::unexpected(
          DbError::InvalidArgument("'merge' must be a boolean."));
    req.merge = m->get<bool>();
  }
  const json *data = Optional(**p, "data");
  if (!data || !data->is_object())
    return std::unexpected(
        DbError::InvalidArgument("'data' must be an object."));
  req.data = *data;
  return req;
}

std::expected<DeleteRequest, DbError> ParseDelete(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto path = PathField<ParsePath>(**p);
  if (!path)
    return std::unexpected(path.error());
  return DeleteRequest{std::move(*path)};
}

std::expected<MutateManyRequest, DbError>
ParseMutateMany(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto path = PathField<ParseCollectionPath>(**p);
  if (!path)
    return std::unexpected(path.error());
  MutateManyRequest req;
  req.path = std::move(*path);
  auto where = ParseWhere(**p);
  if (!where)
    return std::unexpected(where.error());
  req.where = std::move(*where);
  auto remove = ParseFieldPathList(**p, "remove");
  if (!remove)
    return std::unexpected(remove.error());
  req.remove = std::move(*remove);
  if (const json *set = Optional(**p, "set")) {
    if (!set->is_object())
      return std::unexpected(DbError::InvalidArgument(
          "'set' must be an object of field path -> value."));
    for (auto it = set->begin(); it != set->end(); ++it) {
      auto fp = ParseFieldPath(it.key());
      if (!fp)
        return std::unexpected(fp.error());
      req.set.emplace_back(std::move(*fp), it.value());
    }
  }
  return req;
}

std::expected<SubscribeRequest, DbError> ParseSubscribe(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto sid = RequiredString(**p, "subscription_id");
  if (!sid)
    return std::unexpected(sid.error());
  auto path = PathField<ParsePath>(**p);
  if (!path)
    return std::unexpected(path.error());
  auto spec = ParseSpec(**p);
  if (!spec)
    return std::unexpected(spec.error());
  auto token = OptionalString(**p, "resume_token");
  if (!token)
    return std::unexpected(token.error());
  return SubscribeRequest{std::move(*sid), std::move(*path), std::move(*spec),
                          std::move(*token)};
}

std::expected<UnsubscribeRequest, DbError>
ParseUnsubscribe(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto sid = RequiredString(**p, "subscription_id");
  if (!sid)
    return std::unexpected(sid.error());
  return UnsubscribeRequest{std::move(*sid)};
}

std::expected<ClientEventAck, DbError> ParseClientAck(const json &message) {
  const json &meta = message.at("meta");
  auto sid = RequiredString(meta, "subscription_id");
  if (!sid)
    return std::unexpected(DbError::InvalidArgument(
        "'meta.subscription_id' must be a string."));
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto token = RequiredString(**p, "resume_token");
  if (!token)
    return std::unexpected(token.error());
  return ClientEventAck{std::move(*sid), std::move(*token)};
}

std::expected<IndexDefinition, DbError> ParseCreateIndex(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto collection = RequiredString(**p, "collection");
  if (!collection)
    return std::unexpected(collection.error());
  const json *fields = Optional(**p, "fields");
  return MakeIndexDefinition(*collection, fields ? *fields : json(nullptr));
}

std::expected<std::string, DbError> ParseDropIndex(const json &message) {
  auto p = Payload(message);
  if (!p)
    return std::unexpected(p.error());
  auto id = RequiredString(**p, "id");
  if (!id)
    return std::unexpected(id.error());
  return *id;
}

json AckMeta(const std::string &request_id, uint32_t status) {
  return json{{"type", "ACK"},
              {"request_id", request_id},
              {"timestamp", current_iso8601_timestamp()},
              {"status", status}};
}

json ErrorMessage(const std::string &request_id, const DbError &error) {
  json payload{{"code", error.code}, {"message", error.message}};
  if (error.details)
    payload["details"] = *error.details;
  return json{{"meta",
               {{"type", "ERROR"},
                {"request_id", request_id},
                {"timestamp", current_iso8601_timestamp()},
                {"status", error.status}}},
              {"payload", std::move(payload)}};
}

json DocumentJson(const Document &doc, const std::vector<FieldPath> &project) {
  return json{{"path", doc.path},
              {"data", Project(doc.data, project)},
              {"_v", doc.version}};
}

} // namespace pod::wire
