#pragma once
#include "db/database.h"
#include "realtime/subscriptions.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace pod {

// Identity of the connection a request arrived on. `ns` is the namespace
// (scope) fixed at WebSocket upgrade time; every operation is confined to it.
struct ConnectionInfo {
  std::string conn_id;
  std::string ns;
};

// Decodes, validates and executes one wire message, delivering every
// response through the sink. Must be invoked serially per connection.
class RequestHandler {
public:
  RequestHandler(Database &db, SubscriptionManager &subscriptions,
                 MessageSink sink);

  void Handle(const ConnectionInfo &conn, std::string_view raw,
              WireEncoding encoding);

private:
  // Returns the response to send, or nullopt if the handler already sent
  // everything itself (SUBSCRIBE) or nothing is due (client ACK).
  std::optional<nlohmann::json> Dispatch(const ConnectionInfo &conn,
                                         const nlohmann::json &message,
                                         const std::string &type,
                                         const std::string &request_id,
                                         WireEncoding encoding);

  Database &_db;
  SubscriptionManager &_subs;
  MessageSink _sink;
};

} // namespace pod
