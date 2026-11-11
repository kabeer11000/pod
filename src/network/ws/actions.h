#pragma once
#include "common/strand.h"
#include <memory>
#include <string>
#include <uwebsockets/Loop.h>

// Per-socket user data. `ns` is chosen during the HTTP upgrade and is fixed
// for the lifetime of the connection.
struct ConnectionState {
  bool is_alive = true;
  std::string conn_id;
  std::string ns;
  uWS::Loop *owning_loop = nullptr;
  // Serialises request processing for this connection on the thread pool.
  std::shared_ptr<Strand> strand;
};
