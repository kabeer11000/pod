#include "./pod/handlers.h"
#include "./pod/interface.h"
#include "./routines/leveldb/interface.h"
#include "expected"
#include "uwebsockets/App.h"
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
int main() {
  auto db_instance = getLevelDBInstance();
  uWS::App app = uWS::App();

  auto welcomeHandler = [](auto *res, auto *req) {
    res->end("Welcome to Pod DB!");
  };
  app.get("/", welcomeHandler);

  app.ws<ConnectionState>(
      "/webchannel",
      {.message = [](auto *ws, std::string_view message, uWS::OpCode opCode) {
        json parsed_message = json::parse(message);
        const std::string action =
            parsed_message.at("meta").at("action").get<std::string>();
        if (action == "insert") {
          InsertRequest parsed = parsed_message;
          auto response = insert_handler(parsed);
          response.has_value()
              ? ws->send(json(response.value()).dump(), opCode)
              : ws->send(json(response.error()).dump(), opCode);
        } else if (action == "get") {
          GetRequest parsed = parsed_message;
          auto response = get_handler(parsed);
          response.has_value()
              ? ws->send(json(response.value()).dump(), opCode)
              : ws->send(json(response.error()).dump(), opCode);
        } else {
          PodHandlerError err{.reason = "Unknown action: " + action};
          ws->send(json(err).dump(), opCode);
        }
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
