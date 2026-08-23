#pragma once
// Shared WebSocket server core.
//
// Both entry points (`standalone`, the single-node server, and `pods`, the
// router) boot the identical uWS event loop / TaskQueue / ThreadPool
// machinery and only differ in *what happens* for a given `meta.type` -
// standalone dispatches straight into local storage-backed handlers, pods
// will eventually proxy the request to whichever node owns it. This module
// owns the shared plumbing so neither entry point re-implements it.
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

// Delivers the full response envelope (ACK or ERROR) to send back over the
// wire. May be called synchronously (before the handler that received it
// returns) or asynchronously, from any thread, arbitrarily later - e.g. once
// an upstream node's reply arrives. Always safe to call exactly once.
using ResponseCallback = std::function<void(json response)>;

// Receives the raw parsed request envelope (`{meta, payload}`) and a
// `respond` callback to deliver the eventual response through. Invoked on a
// worker thread pulled off the TaskQueue - never on the uWS event loop
// thread - so it is safe to block here (storage I/O...). The handler itself
// may return immediately after calling `respond` synchronously (as every
// `entry_standalone` handler does), or it may hold on to `respond` and call
// it later from elsewhere (as `entry_pods`'s router handler does: it sends
// the request upstream and calls `respond` only once that reply arrives).
using TypeHandler =
    std::function<void(json &request, ResponseCallback respond)>;
using TypeDispatchTable = std::unordered_map<std::string, TypeHandler>;

// One HTTP request, reduced to what a handler actually needs: the parsed
// JSON body (an empty object for a bodyless request) and positional route
// parameters (e.g. `/indexes/:id` -> params[0] is the id). Handlers never
// see the raw uWS request/response objects - those are only valid
// synchronously on the event-loop thread, whereas handlers run on a worker
// thread, same as WS TypeHandlers.
struct HttpRequestContext {
  json body;
  std::vector<std::string> params;
};

// Returns {http_status_code, json_response_body}. Runs on a worker thread
// pulled off the TaskQueue - never inline on the uWS event loop thread - so
// it is safe to block here on storage I/O, exactly like a WS TypeHandler.
using HttpHandler = std::function<std::pair<int, json>(const HttpRequestContext &)>;

struct HttpRoute {
  std::string method;  // "GET" | "POST" | "DELETE"
  std::string pattern; // uWS pattern, e.g. "/indexes/:id"
  HttpHandler handler;
};

struct ServerConfig {
  int port = 3000;
  std::string route = "/webchannel";
  // Label used only for logging so both entry points remain distinguishable.
  std::string entry_name = "server";
};

// Boots the WebSocket + HTTP event loop and runs it until the listen socket
// fails. Every inbound WS message is parsed, its `meta.type` looked up in
// `ws_handlers`, and (if found) the matching handler invoked on a worker
// thread; the resulting JSON is deferred back onto the owning event loop and
// sent to the originating socket. Unknown types and malformed JSON get a
// structured ERROR response rather than being silently dropped.
//
// Each entry in `http_routes` is registered against the same uWS App/port
// (uWS natively serves HTTP and WS off one listener); the request body (if
// any) is buffered on the event loop thread, then the handler itself runs on
// a worker thread and its response is deferred back - the same
// off-the-event-loop discipline as WS messages, so a slow index-registry
// read/write can never stall other connections on this node.
int run_ws_server(const ServerConfig &config, TypeDispatchTable ws_handlers,
                  std::vector<HttpRoute> http_routes = {});
