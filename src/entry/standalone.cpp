#include "common/log.h"
#include "entry/interface.h"
#include "indexing/builder.h"
#include "indexing/engine.h"
#include "indexing/interface.h"
#include "indexing/registry.h"
#include "network/ws/handlers/interface.h"
#include "network/ws/server.h"
#include "network/ws/wire_protocol.h"
#include "storage/interface.h"
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> split_path(const std::string &path) {
  std::vector<std::string> segments;
  std::stringstream ss(path);
  std::string item;
  while (std::getline(ss, item, '/'))
    if (!item.empty())
      segments.push_back(item);
  return segments;
}

std::string join_path(const std::vector<std::string> &segments) {
  std::string path;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i > 0)
      path += '/';
    path += segments[i];
  }
  return path;
}

// Mirrors handlers/pod.cpp's is_reserved_path - the `_sys/...` namespace and
// any ':'-containing segment are reserved for internal bookkeeping and
// can't be targeted by a client-created index either.
bool is_reserved_collection(const std::vector<std::string> &segments) {
  if (!segments.empty() && segments.front().starts_with("_sys"))
    return true;
  for (const auto &segment : segments)
    if (segment.find(':') != std::string::npos)
      return true;
  return false;
}

json error_body(const std::string &code, const std::string &message) {
  return json{{"code", code}, {"message", message}};
}

} // namespace

// Single-node entry point: every meta.type is served directly out of the
// local StorageDriver. The event loop / dispatch machinery itself lives in
// network/ws/server.{h,cpp} and is shared with `entry_pods` (the router).
int entry_standalone(StorageDriver &d) {
  IndexRegistry registry(d);
  IndexEngine engine(d, registry);
  IndexBuilder builder(engine);

  // Startup recovery: anything left BUILDING from a crash/restart gets
  // reset to PENDING by the registry and re-enqueued here. Redoing a
  // backfill from scratch is always safe since index-entry writes are
  // idempotent.
  auto stuck = registry.ReclaimStuckBuilds();
  for (auto &def : stuck) {
    Log(LogLevel::Info, "standalone",
        "Re-enqueuing index (" + def.id + ") stuck BUILDING at startup");
    builder.EnqueueBuild(def);
  }

  TypeDispatchTable handlers;

  handlers["READ"] = [&d](json &request, ResponseCallback respond) {
    ReadRequest parsed = request;
    auto response = read_handler(d, parsed);
    respond(response.has_value() ? json(response.value())
                                 : json(response.error()));
  };
  handlers["WRITE"] = [&d, &engine](json &request, ResponseCallback respond) {
    WriteRequest parsed = request;
    auto response = write_handler(d, parsed, &engine);
    respond(response.has_value() ? json(response.value())
                                 : json(response.error()));
  };
  handlers["DELETE"] = [&d, &engine](json &request, ResponseCallback respond) {
    DeleteRequest parsed = request;
    auto response = delete_handler(d, parsed, &engine);
    respond(response.has_value() ? json(response.value())
                                 : json(response.error()));
  };

  std::vector<HttpRoute> http_routes;

  http_routes.push_back(
      {"POST", "/indexes",
       [&registry, &builder](const HttpRequestContext &ctx) -> std::pair<int, json> {
         if (!ctx.body.contains("collection") || !ctx.body.contains("fields"))
           return {400, error_body("INVALID_BODY",
                                   "Expected {collection, fields}.")};

         std::string collection_raw;
         std::vector<IndexFieldSpec> fields;
         try {
           collection_raw = ctx.body.at("collection").get<std::string>();
           fields = ctx.body.at("fields").get<std::vector<IndexFieldSpec>>();
         } catch (const json::exception &e) {
           return {400, error_body("INVALID_BODY", e.what())};
         }

         auto segments = split_path(collection_raw);
         if (segments.empty() || segments.size() % 2 != 1)
           return {400,
                   error_body("INVALID_COLLECTION",
                              "`collection` must be an odd-length path (a "
                              "collection, not a document).")};
         if (is_reserved_collection(segments))
           return {403,
                   error_body("RESERVED_PATH",
                              "Cannot index a reserved ('_sys' / ':') path.")};
         if (fields.empty())
           return {400,
                   error_body("INVALID_FIELDS", "`fields` must be non-empty.")};

         auto [def, created] = registry.CreateIndex(join_path(segments), fields);
         if (created)
           builder.EnqueueBuild(def);
         return {created ? 202 : 200, json(def)};
       }});

  http_routes.push_back(
      {"GET", "/indexes",
       [&registry](const HttpRequestContext &) -> std::pair<int, json> {
         json indexes = json::array();
         for (auto &def : registry.List())
           indexes.push_back(def);
         return {200, json{{"indexes", indexes}}};
       }});

  http_routes.push_back(
      {"GET", "/indexes/:id",
       [&registry](const HttpRequestContext &ctx) -> std::pair<int, json> {
         if (ctx.params.empty())
           return {400, error_body("MISSING_ID", "Missing index id in path.")};
         auto def = registry.Get(ctx.params[0]);
         if (!def)
           return {404, error_body("NOT_FOUND", "No such index.")};
         return {200, json(*def)};
       }});

  http_routes.push_back(
      {"DELETE", "/indexes/:id",
       [&registry](const HttpRequestContext &ctx) -> std::pair<int, json> {
         if (ctx.params.empty())
           return {400, error_body("MISSING_ID", "Missing index id in path.")};
         if (!registry.Delete(ctx.params[0]))
           return {404, error_body("NOT_FOUND", "No such index.")};
         return {200, json{{"id", ctx.params[0]}, {"status", "DELETED"}}};
       }});

  return run_ws_server(
      {.port = 3000, .route = "/webchannel", .entry_name = "standalone"},
      std::move(handlers), std::move(http_routes));
}
