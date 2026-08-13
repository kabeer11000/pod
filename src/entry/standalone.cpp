#include "entry/interface.h"
#include "common/task_queue.h"
#include "common/thread_pool.h"
#include "network/ws/actions.h"
#include "network/ws/handlers/interface.h"
#include "network/ws/socket_registry.h"
#include "storage/driver_leveldb.h"
#include "storage/interface.h"
#include "uwebsockets/App.h"
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;
auto msgPackResponder(auto &ws, auto &response) {
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

int entry_standalone(StorageDriver &d) {
  uWS::App app = uWS::App();

  TaskQueue tq;
  ThreadPool tp(tq, std::thread::hardware_concurrency());

  app.get("/", [](auto *res, auto *req) { res->end("Welcome to Pod DB!"); });
  app.ws<ConnectionState>(
      "/webchannel", {
          .open = [](auto* ws) {
              auto *state = ws->getUserData();
              std::string conn_id = state->conn_id;
              // Register current socket to the directory
              global_socket_registry().Register(conn_id, ws);
          },
          .message = [&d, &tq](auto *ws, std::string_view _message,
                                           uWS::OpCode opCode) {
        std::string message(_message); // Copy string view for defered tasks
        auto *state = ws->getUserData();
        std::string conn_id = state->conn_id;
        uWS::Loop *loop = state->owning_loop;

        tq.push({
            .closure =
                [conn_id, loop, &d, message = std::move(message), &ws] {
                  json parsed_message = json::parse(message);
                  const std::string action =
                      parsed_message.at("meta").at("action").get<std::string>();
                  if (action == "insert") {
                    InsertRequest parsed = parsed_message;
                    auto response = insert_handler(d, parsed);
                    loop->defer([conn_id, response]() {
                      if (auto *ws = global_socket_registry().Lookup(conn_id))
                        msgPackResponder(ws, response);
                    });
                  } else if (action == "get") {
                    GetRequest parsed = parsed_message;
                    auto response = get_handler(d, parsed);
                    loop->defer([conn_id, response]() {
                      if (auto *ws = global_socket_registry().Lookup(conn_id))
                        msgPackResponder(ws, response);
                    });
                  } else {
                    PodHandlerError err{.reason = "Unknown action: " + action};
                    auto packed = json::to_msgpack(err);
                    std::string payload(
                        reinterpret_cast<const char *>(packed.data()),
                        packed.size());
                    loop->defer([conn_id, payload]() {
                      if (auto *ws = global_socket_registry().Lookup(conn_id))
                        ws->send(payload, uWS::OpCode::BINARY);
                    });
                  }
                },
            .id = conn_id,
            .enqueued_at = std::chrono::steady_clock::now(),
        });
      }});
  app.listen(3000, [](auto *listen_socket) {
    if (listen_socket) {
      std::cerr << "Listening on port " << 3000 << std::endl;
    }
  });

  app.run();

  std::cout << "Failed to listen on port 3000" << std::endl;

  return 0;
}
