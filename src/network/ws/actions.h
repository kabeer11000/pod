#pragma once
#include "storage/types.h"
#include <nlohmann/json_fwd.hpp>
#include <uwebsockets/Loop.h>
#include <uwebsockets/WebSocket.h>

struct ConnectionState {
  bool is_alive = true;
};

struct Task {
  ByteVector message;
  // For sending data to the specific socket connection
  uWS::WebSocket<false, true, ConnectionState> *ws;
  // For hooking into the uWS event loop and calling ->defer()
  uWS::Loop *loop;
}
