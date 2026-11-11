// The pods router. Clients connect exactly as to a node
//   ws://<host>:<port>/webchannel?ns=<namespace>&t=<token>
// and the router proxies the connection, as a virtual connection over one
// authenticated cluster link per node, to the node that owns the namespace on
// the consistent-hash ring. Namespaces never span nodes, so every operation,
// subscription and index stays local to one node.
//
// Everything runs on the single event-loop thread; the router holds no data.
// When a node link drops, its clients are closed with 1013 (try again later)
// and reconnect, resuming subscriptions with their resume_token.
#include "cluster/link.h"
#include "cluster/ring.h"
#include "common/log.h"
#include "entry/interface.h"
#include "rules/crypto.h"
#include "server/authenticate.h"
#include "uwebsockets/App.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using pod::cluster::Frame;
using pod::cluster::FrameType;
using pod::cluster::Link;

namespace {

constexpr unsigned int kMaxPayloadBytes = 16 * 1024 * 1024;
constexpr unsigned int kMaxBackpressureBytes = 64 * 1024 * 1024;
constexpr int kMinRetryMs = 250;
constexpr int kMaxRetryMs = 10000;

struct RouterConn {
  uint64_t vc = 0;
  size_t node = 0;
  bool alive = true;
  // Close code + reason when the connection is refused (sent after opening:
  // browsers cannot see HTTP upgrade failures).
  int reject_code = 0;
  std::string reject_reason;
  std::string ns;
  std::string token;
};
using WebSocket = uWS::WebSocket<false, true, RouterConn>;

struct NodeLink {
  pod::ClusterNode config;
  Link *link = nullptr; // nullptr while disconnected
  bool ready = false;   // handshake completed
  int retry_ms = kMinRetryMs;
  us_timer_t *retry_timer = nullptr;
  std::unordered_map<uint64_t, WebSocket *> clients;
};

class Router {
public:
  explicit Router(const pod::PodConfig &config) : _config(config) {
    std::vector<std::string> ids;
    for (const auto &n : config.nodes) {
      ids.push_back(n.id);
      auto node = std::make_unique<NodeLink>();
      node->config = n;
      _nodes.push_back(std::move(node));
    }
    _ring = std::make_unique<pod::cluster::Ring>(ids, config.virtual_nodes);
  }

  bool Start(uWS::Loop *loop) {
    _context = Link::CreateContext(loop, nullptr);
    if (!_context)
      return false;
    for (auto &node : _nodes) {
      node->retry_timer = us_create_timer(reinterpret_cast<us_loop_t *>(loop),
                                          0, sizeof(void *) * 2);
      auto **ext = static_cast<void **>(us_timer_ext(node->retry_timer));
      ext[0] = this;
      ext[1] = node.get();
      Connect(*node);
    }
    return true;
  }

  // Index of the node owning `ns`, if its link is up.
  std::optional<size_t> Route(const std::string &ns) const {
    const std::string &id = _ring->Owner(ns);
    for (size_t i = 0; i < _nodes.size(); ++i)
      if (_nodes[i]->config.id == id)
        return _nodes[i]->ready ? std::optional<size_t>(i) : std::nullopt;
    return std::nullopt;
  }

  // Registers a client; false if its node went away since the upgrade.
  bool Attach(WebSocket *ws) {
    RouterConn *c = ws->getUserData();
    NodeLink &node = *_nodes[c->node];
    if (!node.ready)
      return false;
    c->vc = _next_vc++;
    node.clients[c->vc] = ws;
    node.link->Send(Frame{.type = FrameType::Open,
                          .conn = c->vc,
                          .a = c->ns,
                          .b = c->token});
    return true;
  }

  void Forward(WebSocket *ws, std::string_view message, bool binary) {
    RouterConn *c = ws->getUserData();
    NodeLink &node = *_nodes[c->node];
    if (!c->alive || !node.ready)
      return;
    node.link->Send(Frame{.type = FrameType::Msg,
                          .conn = c->vc,
                          .binary = binary,
                          .a = std::string(message)});
  }

  void Detach(WebSocket *ws) {
    RouterConn *c = ws->getUserData();
    c->alive = false;
    NodeLink &node = *_nodes[c->node];
    if (node.clients.erase(c->vc) && node.ready)
      node.link->Send(
          Frame{.type = FrameType::Close, .conn = c->vc, .code = 1000});
  }

  std::string Status() const {
    std::string out = "Pods router\n";
    for (const auto &n : _nodes)
      out += n->config.id + " " + n->config.host + ":" +
             std::to_string(n->config.port) +
             (n->ready ? " up, " : " down, ") +
             std::to_string(n->clients.size()) + " client(s)\n";
    return out;
  }

private:
  void Connect(NodeLink &node) {
    NodeLink *n = &node;
    node.link = Link::Connect(
        _context, node.config.host, node.config.port,
        [this, n](Link &l, Frame &&f) { OnFrame(*n, l, std::move(f)); },
        [this, n](Link &, const std::string &why) { OnClosed(*n, why); });
    if (!node.link)
      ScheduleRetry(node);
  }

  void ScheduleRetry(NodeLink &node) {
    us_timer_set(
        node.retry_timer,
        [](us_timer_t *t) {
          auto **ext = static_cast<void **>(us_timer_ext(t));
          auto *self = static_cast<Router *>(ext[0]);
          self->Connect(*static_cast<NodeLink *>(ext[1]));
        },
        node.retry_ms, 0);
    node.retry_ms = std::min(node.retry_ms * 2, kMaxRetryMs);
  }

  void OnFrame(NodeLink &node, Link &link, Frame &&f) {
    if (!node.ready) {
      if (f.type == FrameType::Challenge && link.nonce.empty() &&
          f.a.size() == 32) {
        link.nonce = pod::cluster::MakeNonce();
        link.Send(Frame{.type = FrameType::Hello,
                        .a = pod::cluster::HandshakeMac(_config.cluster_secret,
                                                        "router", f.a),
                        .b = link.nonce});
        return;
      }
      if (f.type == FrameType::Welcome && !link.nonce.empty() &&
          pod::crypto::ConstantTimeEqual(
              f.a, pod::cluster::HandshakeMac(_config.cluster_secret, "node",
                                              link.nonce))) {
        node.ready = true;
        node.retry_ms = kMinRetryMs;
        Log(LogLevel::Info, "router",
            "Linked to node " + node.config.id + " (" + node.config.host +
                ":" + std::to_string(node.config.port) + ")");
        return;
      }
      link.Close("handshake failed (check POD_CLUSTER_SECRET)");
      return;
    }
    auto it = node.clients.find(f.conn);
    if (it == node.clients.end())
      return; // the client already left
    WebSocket *ws = it->second;
    switch (f.type) {
    case FrameType::Msg:
      if (ws->send(f.a, f.binary ? uWS::OpCode::BINARY : uWS::OpCode::TEXT) ==
          WebSocket::SendStatus::DROPPED)
        Shed(ws, 1013, "Backpressure limit exceeded");
      return;
    case FrameType::Close:
      // The node already forgot this connection; do not echo a CLOSE.
      node.clients.erase(it);
      ws->getUserData()->alive = false;
      ws->end(f.code ? f.code : 1000, f.a);
      return;
    default:
      link.Close("unexpected frame type");
    }
  }

  // Closes a client and tells its node (via the close handler -> Detach).
  void Shed(WebSocket *ws, int code, std::string_view reason) {
    if (!ws->getUserData()->alive)
      return;
    Detach(ws);
    ws->end(code, reason);
  }

  void OnClosed(NodeLink &node, const std::string &why) {
    const bool was_ready = node.ready;
    node.ready = false;
    node.link = nullptr;
    auto clients = std::move(node.clients);
    node.clients.clear();
    for (auto &[vc, ws] : clients) {
      ws->getUserData()->alive = false;
      ws->end(1013, "Database node unavailable, reconnect");
    }
    Log(was_ready ? LogLevel::Warn : LogLevel::Debug, "router",
        "Link to node " + node.config.id + " down (" + why + "), " +
            std::to_string(clients.size()) +
            " client(s) disconnected; retrying in " +
            std::to_string(node.retry_ms) + "ms");
    ScheduleRetry(node);
  }

  const pod::PodConfig &_config;
  std::vector<std::unique_ptr<NodeLink>> _nodes;
  std::unique_ptr<pod::cluster::Ring> _ring;
  us_socket_context_t *_context = nullptr;
  uint64_t _next_vc = 1;
};

} // namespace

int entry_router(const pod::PodConfig &config) {
  uWS::App app = uWS::App();
  uWS::Loop *loop = uWS::Loop::get();
  Router router(config);
  if (!router.Start(loop)) {
    Log(LogLevel::Error, "router", "Failed to create the cluster context");
    return 1;
  }

  app.get("/", [&router](auto *res, auto * /*req*/) {
    res->end(router.Status());
  });

  uWS::App::WebSocketBehavior<RouterConn> behavior;
  behavior.maxPayloadLength = kMaxPayloadBytes;
  behavior.maxBackpressure = kMaxBackpressureBytes;
  behavior.closeOnBackpressureLimit = false;
  behavior.upgrade = [&config, &router](auto *res, auto *req, auto *context) {
    const std::string token(req->getQuery("t"));
    // Without its own POD_AUTH_SECRET the router only checks the namespace
    // and leaves the token to the node; the node always re-authenticates.
    auto auth = pod::AuthenticateConnection(
        config, req->getQuery("ns"),
        config.auth_secret.empty() ? std::string_view() : token);
    RouterConn conn;
    if (!auth) {
      conn.reject_code = auth.error().ws_close_code;
      conn.reject_reason = auth.error().reason;
    } else if (auto node = router.Route(auth->ns)) {
      conn.node = *node;
      conn.ns = auth->ns;
      conn.token = token;
    } else {
      conn.reject_code = 1013;
      conn.reject_reason = "The database node for this namespace is unavailable.";
    }
    res->template upgrade<RouterConn>(
        std::move(conn), req->getHeader("sec-websocket-key"),
        req->getHeader("sec-websocket-protocol"),
        req->getHeader("sec-websocket-extensions"), context);
  };
  behavior.open = [&router](WebSocket *ws) {
    RouterConn *c = ws->getUserData();
    if (c->reject_code) {
      c->alive = false;
      ws->end(c->reject_code, c->reject_reason);
      return;
    }
    if (!router.Attach(ws)) {
      ws->getUserData()->alive = false;
      ws->end(1013, "Database node unavailable, reconnect");
    }
  };
  behavior.message = [&router](WebSocket *ws, std::string_view message,
                               uWS::OpCode op) {
    if (op == uWS::OpCode::TEXT || op == uWS::OpCode::BINARY)
      router.Forward(ws, message, op == uWS::OpCode::BINARY);
  };
  behavior.close = [&router](WebSocket *ws, int, std::string_view) {
    if (ws->getUserData()->alive)
      router.Detach(ws);
  };
  app.ws<RouterConn>("/webchannel", std::move(behavior));

  bool listening = false;
  app.listen(config.port, [&](auto *listen_socket) {
    listening = listen_socket != nullptr;
  });
  if (!listening) {
    Log(LogLevel::Error, "router",
        "Failed to listen on port " + std::to_string(config.port));
    return 1;
  }
  Log(LogLevel::Info, "router",
      "Pods router listening on port " + std::to_string(config.port) +
          " for " + std::to_string(config.nodes.size()) + " node(s)");
  app.run();
  return 0;
}
