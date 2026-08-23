#include "network/ws/server.h"
#include "common/log.h"
#include "common/task_queue.h"
#include "common/thread_pool.h"
#include "common/timestamp.h"
#include "network/ws/actions.h"
#include "network/ws/socket_registry.h"
#include "network/ws/wire_protocol.h"
#include "uwebsockets/App.h"
#include <atomic>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace {

json make_error_envelope(const std::string &request_id, uint32_t status,
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

void send_json(auto *ws, const json &envelope) {
  ws->send(envelope.dump(), uWS::OpCode::TEXT);
}

// Registers one HTTP route against `app`. The request body (if any) is
// buffered on the event loop thread via onData; once fully received, the
// actual handler call is pushed onto `tq` and runs on a worker thread -
// mirroring exactly how WS messages are dispatched, so no HTTP handler can
// block the event loop on storage I/O. The response is written back via
// `loop->defer`, guarded by an `alive` flag set false in onAborted so a
// response is never written to a connection the client already dropped.
void register_http_route(uWS::App &app, TaskQueue &tq,
                         const std::string &entry_name, HttpRoute route) {
  auto handler_fn = [&tq, entry_name, route](auto *res, auto *req) {
    std::vector<std::string> params;
    for (unsigned short i = 0; i < 8; ++i) {
      auto p = req->getParameter(i);
      if (p.empty())
        break;
      params.emplace_back(p);
    }

    auto alive = std::make_shared<std::atomic<bool>>(true);
    res->onAborted([alive] { *alive = false; });

    auto buffer = std::make_shared<std::string>();
    res->onData([res, &tq, entry_name, route, params, alive,
                buffer](std::string_view chunk, bool last) {
      buffer->append(chunk);
      if (!last)
        return;

      uWS::Loop *loop = uWS::Loop::get();
      std::string body_str = std::move(*buffer);

      tq.push({
          .closure =
              [loop, res, alive, route, params, entry_name,
               body_str = std::move(body_str)] {
                int status;
                json response;
                try {
                  HttpRequestContext ctx;
                  ctx.params = params;
                  ctx.body = body_str.empty() ? json::object()
                                              : json::parse(body_str);
                  std::tie(status, response) = route.handler(ctx);
                } catch (const json::exception &e) {
                  Log(LogLevel::Error, entry_name,
                      "HTTP handler (" + route.pattern +
                          ") got malformed JSON body: " + e.what());
                  status = 400;
                  response = {{"code", "INVALID_JSON"},
                              {"message", "Malformed JSON body."}};
                } catch (const std::exception &e) {
                  Log(LogLevel::Error, entry_name,
                      "HTTP handler (" + route.pattern +
                          ") threw: " + e.what());
                  status = 500;
                  response = {{"code", "INTERNAL_ERROR"},
                              {"message", "Handler failed to process the "
                                          "request."}};
                }

                loop->defer([res, alive, status, response]() {
                  if (!*alive)
                    return;
                  res->cork([res, status, response]() {
                    res->writeStatus(std::to_string(status))
                        ->writeHeader("Content-Type", "application/json")
                        ->end(response.dump());
                  });
                });
              },
          .id = route.pattern,
          .enqueued_at = std::chrono::steady_clock::now(),
      });
    });
  };

  if (route.method == "GET")
    app.get(route.pattern, handler_fn);
  else if (route.method == "POST")
    app.post(route.pattern, handler_fn);
  else if (route.method == "DELETE")
    app.del(route.pattern, handler_fn);
}

} // namespace

int run_ws_server(const ServerConfig &config, TypeDispatchTable handlers,
                  std::vector<HttpRoute> http_routes) {
  uWS::App app = uWS::App();

  TaskQueue tq;
  ThreadPool tp(tq, std::thread::hardware_concurrency());
  auto gen = boost::uuids::random_generator();
  const std::string entry_name = config.entry_name;

  app.get("/", [](auto *res, auto *req) { res->end("Welcome to Pod DB!"); });
  for (auto &route : http_routes)
    register_http_route(app, tq, entry_name, route);
  app.ws<ConnectionState>(
      config.route,
      {.open =
           [&gen, entry_name](auto *ws) {
             Log(LogLevel::Info, entry_name,
                 "Attempting to open a new connection");
             auto *state = ws->getUserData();
             state->owning_loop = uWS::Loop::get();
             state->conn_id = boost::uuids::to_string(gen());
             std::string conn_id = state->conn_id;
             // Register current socket to the directory
             global_socket_registry().Register(conn_id, ws);
             Log(LogLevel::Info, entry_name,
                 "Connection Registered (" + conn_id + ")");
           },
       .message =
           [&tq, &handlers, entry_name](auto *ws, std::string_view _message,
                                        uWS::OpCode opCode) {
             std::string message(
                 _message); // Copy string view for defered tasks
             auto *state = ws->getUserData();
             std::string conn_id = state->conn_id;
             uWS::Loop *loop = state->owning_loop;
             Log(LogLevel::Info, entry_name,
                 "Message Recieved (" + message + ")");
             tq.push({
                 .closure =
                     [conn_id, loop, &handlers, entry_name,
                      message = std::move(message)] {
                       Log(LogLevel::Info, "ThreadPool",
                           "Executing Closure of Queued Task (" + conn_id +
                               ")");

                       json parsed_message =
                           json::parse(message, nullptr, false);
                       if (parsed_message.is_discarded()) {
                         Log(LogLevel::Error, "ThreadPool",
                             "Malformed JSON from " + conn_id);
                         loop->defer([conn_id]() {
                           if (auto *ws =
                                   global_socket_registry().Lookup(conn_id))
                             send_json(ws,
                                       make_error_envelope(
                                           "", 400, "INVALID_JSON",
                                           "Malformed JSON syntax."));
                         });
                         return;
                       }

                       std::string request_id;
                       std::string type;
                       try {
                         request_id = parsed_message.at("meta")
                                          .at("request_id")
                                          .get<std::string>();
                         type = parsed_message.at("meta")
                                    .at("type")
                                    .get<std::string>();
                       } catch (const json::exception &e) {
                         Log(LogLevel::Error, "ThreadPool",
                             "Missing meta.type/meta.request_id from " +
                                 conn_id);
                         loop->defer([conn_id]() {
                           if (auto *ws =
                                   global_socket_registry().Lookup(conn_id))
                             send_json(ws, make_error_envelope(
                                               "", 400, "INVALID_ENVELOPE",
                                               "Missing meta.type or "
                                               "meta.request_id."));
                         });
                         return;
                       }
                       Log(LogLevel::Info, "ThreadPool",
                           "Parsed JSON meta.type of Queued Task (" + conn_id +
                               ")" + "(" + type + ")");

                       auto it = handlers.find(type);
                       if (it == handlers.end()) {
                         Log(LogLevel::Error, entry_name,
                             "Unsupported meta.type (" + type + ") from " +
                                 conn_id);
                         loop->defer([conn_id, request_id]() {
                           if (auto *ws =
                                   global_socket_registry().Lookup(conn_id))
                             send_json(
                                 ws, make_error_envelope(
                                         request_id, 400, "UNSUPPORTED_TYPE",
                                         "This entry point does not support "
                                         "this meta.type."));
                         });
                         return;
                       }

                       ResponseCallback respond = [conn_id, loop](json response) {
                         loop->defer([conn_id, response]() {
                           if (auto *ws =
                                   global_socket_registry().Lookup(conn_id))
                             send_json(ws, response);
                         });
                       };

                       try {
                         it->second(parsed_message, respond);
                       } catch (const std::exception &e) {
                         // Only catches a handler that throws before
                         // returning (e.g. while kicking off an async
                         // proxy send) - once it has handed off to
                         // `respond` asynchronously, a later failure is the
                         // handler's own responsibility to report through
                         // `respond` itself.
                         Log(LogLevel::Error, entry_name,
                             "Handler threw while processing (" + type +
                                 ") from " + conn_id + ": " + e.what());
                         respond(make_error_envelope(
                             request_id, 500, "INTERNAL_ERROR",
                             "Handler failed to process the request."));
                       }
                     },
                 .id = conn_id,
                 .enqueued_at = std::chrono::steady_clock::now(),
             });
           },
       .close =
           [](auto *ws, int, std::string_view) {
             global_socket_registry().Remove(ws->getUserData()->conn_id);
           }});

  app.listen(config.port, [&config, entry_name](auto *listen_socket) {
    if (listen_socket)
      Log(LogLevel::Info, entry_name,
          "Pod listening on port " + std::to_string(config.port));
  });

  app.run();

  std::cout << "Failed to listen on port " << config.port << std::endl;

  return 0;
}
