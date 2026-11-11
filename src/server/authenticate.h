// Connection-time authentication shared by WebSocket clients and virtual
// connections arriving through a cluster router.
#pragma once
#include "rules/auth_context.h"
#include "server/config.h"
#include <expected>
#include <string>
#include <string_view>

namespace pod {

// Connections are refused by closing the WebSocket with ws_close_code:
// 4400 bad namespace, 4401 missing/invalid token, 4403 token not valid for
// the namespace. Clients must not reconnect automatically on these.
struct AuthFailure {
  int ws_close_code;
  std::string reason;
};

// Resolves the namespace (falling back to the default) and verifies the
// optional HS256 token. A token may restrict namespaces through its "ns"
// claim (a string or an array of strings).
std::expected<AuthContext, AuthFailure>
AuthenticateConnection(const PodConfig &config, std::string_view ns_param,
                       std::string_view token);

} // namespace pod
