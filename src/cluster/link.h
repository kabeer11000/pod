// A framed, keepalive'd TCP cluster link on top of uSockets. Everything here
// runs on the event-loop thread.
//
// Lifetime: a Link is owned by its socket. After the close handler has run the
// Link is freed on the next loop iteration, so holders must forget their
// pointer inside the close handler.
#pragma once
#include "cluster/frame.h"
#include <cstdint>
#include <functional>
#include <libusockets.h>
#include <string>
#include <string_view>

namespace uWS {
struct Loop;
}

namespace pod::cluster {

class Link {
public:
  using FrameHandler = std::function<void(Link &, Frame &&)>;
  using CloseHandler = std::function<void(Link &, const std::string &why)>;
  using AcceptHandler = std::function<void(Link &)>;

  // A socket context for links. `on_accept` is called for every inbound
  // connection (it must set the handlers); pass nullptr for outbound-only.
  static us_socket_context_t *CreateContext(uWS::Loop *loop,
                                            AcceptHandler on_accept);
  // Starts an outbound connection; the close handler fires if it fails.
  // Returns nullptr if the attempt could not even be started.
  static Link *Connect(us_socket_context_t *context, const std::string &host,
                       int port, FrameHandler on_frame, CloseHandler on_close);

  void SetHandlers(FrameHandler on_frame, CloseHandler on_close);
  // Queues a frame. Returns false if the link is closed or was closed because
  // the peer stopped draining it.
  bool Send(const Frame &frame);
  void Close(const std::string &why);
  bool IsOpen() const { return !_closed; }
  bool IsConnected() const { return _connected && !_closed; }
  uint64_t Id() const { return _id; }
  // Free-form state for the owner (e.g. handshake progress).
  bool authenticated = false;
  std::string nonce;

  // Internal: uSockets callbacks.
  static us_socket_t *OnOpen(us_socket_t *s, int is_client, char *, int);
  static us_socket_t *OnData(us_socket_t *s, char *data, int length);
  static us_socket_t *OnWritable(us_socket_t *s);
  static us_socket_t *OnClose(us_socket_t *s, int, void *);
  static us_socket_t *OnEnd(us_socket_t *s);
  static us_socket_t *OnTimeout(us_socket_t *s);
  static us_socket_t *OnConnectError(us_socket_t *s, int);

private:
  explicit Link(us_socket_t *s);
  void Flush();
  void Finish(const std::string &why); // runs the close handler once, frees
  bool WriteRaw(std::string_view bytes);

  us_socket_t *_s;
  uint64_t _id;
  bool _connected = false;
  bool _closed = false;
  bool _awaiting_pong = false;
  FrameReader _reader;
  std::string _out; // bytes the kernel did not accept yet
  size_t _out_offset = 0;
  FrameHandler _on_frame;
  CloseHandler _on_close;
};

// Handshake helpers (mutual HMAC challenge-response over the cluster secret).
std::string MakeNonce();
std::string HandshakeMac(std::string_view secret, std::string_view role,
                         std::string_view nonce);

} // namespace pod::cluster
