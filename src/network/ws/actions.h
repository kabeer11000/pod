#pragma once
#include "common/strand.h"
#include "rules/auth_context.h"
#include "server/authenticate.h"
#include <optional>
#include <memory>
#include <string>

// Per-socket user data. `auth` (namespace + verified token) is established
// during the HTTP upgrade and fixed for the lifetime of the connection.
struct ConnectionState {
  bool is_alive = true;
  std::string conn_id;
  pod::AuthContext auth;
  // Set when authentication failed: the socket is closed with this code
  // right after opening (browsers cannot see HTTP upgrade failures).
  std::optional<pod::AuthFailure> rejected;
  // Serialises request processing for this connection on the thread pool.
  std::shared_ptr<Strand> strand;
};
