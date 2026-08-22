#include "common/log.h"
#include "common/task_queue.h"
#include "common/thread_pool.h"
#include "entry/interface.h"
#include "network/ws/actions.h"
#include "network/ws/handlers/interface.h"
#include "network/ws/socket_registry.h"
#include "network/ws/wire_protocol.h"
#include "storage/interface.h"
#include "uwebsockets/App.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;
auto MessagePackResponder(auto &ws, auto &response) {
  if (response.has_value()) {
    auto packed = json::to_msgpack(response.value());
    std::string_view payload(reinterpret_cast<const char *>(packed.data()),
                             packed.size());
    ws->send(payload, uWS::OpCode::BINARY);
  } else {
    auto packed = json::to_msgpack(response.error());
    std::string_view payload(reinterpret_cast<const char *>(packed.data()),
                             packed.size());
    ws->send(payload, uWS::OpCode::BINARY);
  }
};
auto JSONResponder(auto &ws, auto &response) {
  if (response.has_value()) {
    auto payload = json(response.value()).dump();
    ws->send(payload, uWS::OpCode::TEXT);
  } else {
    auto payload = json(response.value()).dump();
    ws->send(payload, uWS::OpCode::TEXT);
  }
};

int entry_standalone(StorageDriver &d) {
  uWS::App app = uWS::App();

  TaskQueue tq;
  ThreadPool tp(tq, std::thread::hardware_concurrency());
  auto gen = boost::uuids::random_generator();

  app.get("/", [](auto *res, auto *req) { res->end("Welcome to Pod DB!"); });
  app.ws<ConnectionState>(
      "/webchannel",
      {.open =
           [&gen](auto *ws) {
             Log(LogLevel::Info, "standalone",
                 "Attempting to open a new connection");
             auto *state = ws->getUserData();
             state->owning_loop = uWS::Loop::get();
             state->conn_id = boost::uuids::to_string(gen());
             std::string conn_id = state->conn_id;
             // Register current socket to the directory
             global_socket_registry().Register(conn_id, ws);
             Log(LogLevel::Info, "standalone",
                 "Connection Registered (" + conn_id + ")");
           },
       .message =
           [&d, &tq](auto *ws, std::string_view _message, uWS::OpCode opCode) {
             std::string message(
                 _message); // Copy string view for defered tasks
             auto *state = ws->getUserData();
             std::string conn_id = state->conn_id;
             uWS::Loop *loop = state->owning_loop;
             Log(LogLevel::Info, "standalone",
                 "Message Recieved (" + message + ")");
             tq.push({
                 .closure =
                     [conn_id, loop, &d, message = std::move(message)] {
                       Log(LogLevel::Info, "ThreadPool",
                           "Executing Closure of Queued Task (" + conn_id +
                               ")");

                       json parsed_message = json::parse(message);
                       const std::string action = parsed_message.at("meta")
                                                      .at("action")
                                                      .get<std::string>();
                       Log(LogLevel::Info, "ThreadPool",
                           "Parsed JSON action of Queued Task (" + conn_id +
                               ")" + "(" + action + ")");

                       if (action == "READ") {
                         ReadRequest parsed = parsed_message;
                         auto response = read_handler(d, parsed);
                         loop->defer([conn_id, response]() {
                           if (auto *ws =
                                   global_socket_registry().Lookup(conn_id))
                             JSONResponder(ws, response);
                         });
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
  app.listen(3000, [](auto *listen_socket) {
    if (listen_socket)
      Log(LogLevel::Info, "standalone", "Pod listening on port 3000");
  });

  app.run();

  std::cout << "Failed to listen on port 3000" << std::endl;

  return 0;
}
