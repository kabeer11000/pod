#include "network/ws/socket_registry.h"
#include <uwebsockets/WebSocket.h>

void SocketRegistry::Register(std::string& connection_id, _ws* ws)  {
    // CAS + Sleeplock on _mu
    std::lock_guard<std::mutex> lock(_mu);
    _sockets[connection_id] = ws;
    // Automatically cleans up / frees lock
};
_ws* SocketRegistry::Lookup(const std::string& connection_id) {
    std::lock_guard<std::mutex> lock(_mu);
    auto iter = _sockets.find(connection_id);
    return iter != _sockets.end() ? iter->second : nullptr;
};
void SocketRegistry::Remove(std::string& connection_id) {
    std::lock_guard<std::mutex> lock(_mu);
    _sockets.erase(connection_id);
};

SocketRegistry& global_socket_registry() {
    static SocketRegistry s;
    return s;
};
