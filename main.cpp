#include "./pod/interface.h"
#include "./routines/leveldb/interface.h"
#include "uwebsockets/App.h"
#include <iostream>
#include <leveldb/db.h>
#include <leveldb/options.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
int main() {
  auto db_instance = getDatabaseInstance();
  uWS::App app = uWS::App();

  auto welcomeHandler = [](auto *res, auto *req) {
    res->end("Welcome to Pod DB!");
  };
  app.get("/", welcomeHandler);

  app.ws<ConnectionState>("/webchannel",
                          {

                              .message = [](auto *ws, std::string_view message,
                                            uWS::OpCode opCode) {
                                                db_instance.Put(leveldb::WriteOptions(), const Slice &key, const Slice &value)
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
