// Where responses and events for a connection id go. A connection is either
// a client WebSocket or a virtual connection multiplexed over a cluster link;
// both register here. Only used on the event-loop thread.
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pod {

struct Outbound {
  // Sends one frame. Returns false if the connection had to be closed
  // (e.g. backpressure limit), in which case nothing else should be sent.
  std::function<bool(std::string_view payload, bool binary)> send;
  // Closes the connection with a WebSocket close code and reason.
  std::function<void(int code, std::string_view reason)> close;
};

class OutboundRegistry {
public:
  void Register(const std::string &conn_id, Outbound out) {
    _map[conn_id] = std::move(out);
  }
  void Remove(const std::string &conn_id) { _map.erase(conn_id); }
  // Returns false if the connection no longer exists.
  bool Send(const std::string &conn_id, std::string_view payload,
            bool binary) {
    auto it = _map.find(conn_id);
    if (it == _map.end())
      return false;
    // Copy: the callback may remove its own entry (closing the connection).
    auto send = it->second.send;
    return send(payload, binary);
  }
  void Close(const std::string &conn_id, int code, std::string_view reason) {
    auto it = _map.find(conn_id);
    if (it == _map.end())
      return;
    auto close = it->second.close; // may remove its own entry
    close(code, reason);
  }
  size_t Size() const { return _map.size(); }

private:
  std::unordered_map<std::string, Outbound> _map;
};

} // namespace pod
