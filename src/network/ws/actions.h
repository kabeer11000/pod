#pragma once
#include "string"
#include <uwebsockets/Loop.h>

struct ConnectionState {
  bool is_alive = true;
  std::string conn_id;
  uWS::Loop* owning_loop;
};
