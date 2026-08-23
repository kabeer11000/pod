#include "cluster/outbound_map.h"
#include "cluster/ring.h"
#include "cluster/upstream_transport.h"
#include "common/log.h"
#include "common/timestamp.h"
#include "entry/interface.h"
#include "network/ws/server.h"
#include "network/ws/wire_protocol.h"
#include "storage/interface.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>

namespace {

json error_envelope(const std::string &request_id, uint32_t status,
                    const std::string &code, const std::string &message) {
  return json(ErrorResponse{
      .meta =
          AckMetaGeneric{
              .type = "ACK",
              .request_id = request_id,
              .timestamp = current_iso8601_timestamp(),
              .status = status,
          },
      .payload = ErrorPayload{.code = code, .message = message}});
}

} // namespace

// Router entry point: forwards each request to whichever Pod node
// ShardRing says owns it, rather than serving it out of local storage.
//
// The event loop / dispatch machinery is entirely shared with
// entry_standalone via network/ws/server.{h,cpp}; only the TypeHandler
// closures differ - standalone's call into local storage-backed handlers
// and respond synchronously, this one sends upstream and responds only once
// that reply arrives (TypeHandler's callback-style contract exists
// specifically to support this).
//
// Node membership is a static seed list for now, and there is no real
// upstream transport yet (see cluster/upstream_transport.h) - both are
// deliberately out of scope here, same as resharding/data-migration once
// membership can actually change. What's real: shard-key routing via
// ShardRing, and the OutboundMap bookkeeping that correlates an upstream
// reply back to the original client - the boilerplate a real transport
// plugs into without touching this dispatch logic at all.
int entry_pods(StorageDriver &d) {
  ShardRing ring(d);
  // TODO: replace with real cluster membership/discovery once it exists.
  ring.AddNode("pod-1");
  ring.AddNode("pod-2");
  ring.AddNode("pod-3");

  UnavailableUpstreamTransport transport;
  auto gen = boost::uuids::random_generator();

  TypeDispatchTable handlers;

  // READ/QUERY/WRITE/DELETE/MUTATE_MANY all share the same request ->
  // single ACK/ERROR response shape, so one routing closure handles all of
  // them: resolve the owning node from the request's `payload.path` (the
  // shard key), rewrite the request under a fresh id, register it in the
  // OutboundMap, and hand it to the transport. The OutboundMap entry is
  // what lets the eventual (or, today, immediate synthetic) reply find its
  // way back to this specific caller.
  TypeHandler proxy = [&ring, &transport, &gen](json &request,
                                                ResponseCallback respond) {
    std::string original_request_id;
    std::string shard_key;
    try {
      original_request_id =
          request.at("meta").at("request_id").get<std::string>();
      shard_key = request.at("payload").at("path").get<std::string>();
    } catch (const json::exception &e) {
      respond(error_envelope("", 400, "INVALID_ENVELOPE",
                             std::string("Missing meta.request_id or "
                                         "payload.path: ") +
                                 e.what()));
      return;
    }

    auto owner = ring.Owner(shard_key);
    if (!owner) {
      respond(error_envelope(original_request_id, 503, "NO_UPSTREAM_NODE",
                             "Shard ring has no nodes to route to."));
      return;
    }

    const std::string upstream_request_id = boost::uuids::to_string(gen());
    json outbound = request;
    outbound["meta"]["request_id"] = upstream_request_id;

    global_outbound_map().Track(
        upstream_request_id,
        PendingProxyRequest{
            .original_request_id = original_request_id,
            .respond = respond,
            .enqueued_at = std::chrono::steady_clock::now(),
        });

    Log(LogLevel::Info, "pods",
        "Routing request (" + original_request_id + ") for shard key (" +
            shard_key + ") to node (" + *owner + ") as (" +
            upstream_request_id + ")");

    transport.Send(*owner, outbound, [upstream_request_id](json upstream_response) {
      // Whatever eventually delivers upstream replies (today: the stub
      // transport, calling back immediately; later: an outbound
      // connection's own message handler, matching by request_id) only
      // needs to know the upstream_request_id - this lookup is the whole
      // point of OutboundMap.
      auto pending = global_outbound_map().Resolve(upstream_request_id);
      if (!pending)
        return; // already resolved (or never tracked) - nothing to deliver
      upstream_response["meta"]["request_id"] = pending->original_request_id;
      pending->respond(upstream_response);
    });
  };

  for (const char *type : {"READ", "QUERY", "WRITE", "DELETE", "MUTATE_MANY"})
    handlers[type] = proxy;

  // SUBSCRIBE/UNSUBSCRIBE use a fundamentally different (long-lived,
  // server-push) pattern than the one-shot request/ack shape above - a real
  // implementation needs to relay EVENT pushes from the upstream connection
  // for as long as the subscription lives, not just correlate one reply.
  // Left stubbed until that streaming proxy model exists.
  TypeHandler not_implemented = [](json &request, ResponseCallback respond) {
    std::string request_id;
    try {
      request_id = request.at("meta").at("request_id").get<std::string>();
    } catch (const json::exception &) {
    }
    respond(error_envelope(request_id, 501, "NOT_IMPLEMENTED",
                          "Pod router does not yet proxy subscriptions to "
                          "upstream nodes."));
  };
  handlers["SUBSCRIBE"] = not_implemented;
  handlers["UNSUBSCRIBE"] = not_implemented;

  // Same HTTP route shapes as entry_standalone's index API, stubbed the
  // same way: index metadata (`_sys/indexes`) is inherently per-node today,
  // so "create an index" on the router would need to fan out to every node
  // owning a shard of the collection once clustering is real.
  HttpHandler not_implemented_http =
      [](const HttpRequestContext &) -> std::pair<int, json> {
    return {501, json{{"code", "NOT_IMPLEMENTED"},
                      {"message", "Pod router does not yet proxy index "
                                  "management to upstream nodes."}}};
  };
  std::vector<HttpRoute> http_routes = {
      {"POST", "/indexes", not_implemented_http},
      {"GET", "/indexes", not_implemented_http},
      {"GET", "/indexes/:id", not_implemented_http},
      {"DELETE", "/indexes/:id", not_implemented_http},
  };

  return run_ws_server(
      {.port = 3001, .route = "/webchannel", .entry_name = "pods"},
      std::move(handlers), std::move(http_routes));
}
