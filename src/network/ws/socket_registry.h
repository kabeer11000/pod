#pragma once
#include "network/ws/actions.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <uwebsockets/WebSocket.h>

using _ws = uWS::WebSocket<false, true, ConnectionState>;

// Maps connection ids to live sockets. Sockets must only be *used* on their
// owning loop thread (i.e. inside loop->defer callbacks).
class SocketRegistry {
private:
  std::mutex _mu;
  std::unordered_map<std::string, _ws *> _sockets;

public:
  void Register(const std::string &connection_id, _ws *ws);
  _ws *Lookup(const std::string &connection_id);
  void Remove(const std::string &connection_id);
};
SocketRegistry &global_socket_registry();
