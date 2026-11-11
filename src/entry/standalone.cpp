// A Pod database node. Clients connect over WebSockets at
//   ws://<host>:<port>/webchannel?ns=<namespace>&t=<token>
// and, when POD_INTERNAL_PORT is set, routers connect over cluster links
// (src/cluster/frame.h) carrying virtual client connections.
//
// Threading model:
//   * One uWS event loop thread owns every socket and the OutboundRegistry.
//   * Requests run on a ThreadPool; each connection (real or virtual) has a
//     Strand so its requests run strictly in arrival order.
//   * Responses and events are serialised on the worker and handed back to
//     the loop thread with loop->defer (FIFO), preserving per-connection order.
#include "cluster/link.h"
#include "common/log.h"
#include "common/strand.h"
#include "common/task_queue.h"
#include "common/thread_pool.h"
#include "db/database.h"
#include "entry/interface.h"
#include "network/ws/actions.h"
#include "network/ws/handlers/interface.h"
#include "realtime/subscriptions.h"
#include "rules/crypto.h"
#include "rules/rules.h"
#include "server/authenticate.h"
#include "server/outbound.h"
#include "uwebsockets/App.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

using json = nlohmann::json;
using pod::cluster::Frame;
using pod::cluster::FrameType;
using pod::cluster::Link;

namespace {

// Incoming frames larger than this close the connection.
constexpr unsigned int kMaxPayloadBytes = 16 * 1024 * 1024;
// Outgoing bytes buffered per socket before we give up on a slow consumer.
constexpr unsigned int kMaxBackpressureBytes = 64 * 1024 * 1024;

using WebSocket = uWS::WebSocket<false, true, ConnectionState>;

// Everything a node needs to serve one connection, real or virtual.
struct NodeServices {
  TaskQueue &tq;
  pod::SubscriptionManager &subscriptions;
  pod::RequestHandler &handler;
  pod::OutboundRegistry &outbound;

  std::shared_ptr<Strand> Open(const std::string &conn_id, pod::Outbound out) {
    outbound.Register(conn_id, std::move(out));
    subscriptions.AddConnection(conn_id);
    return std::make_shared<Strand>(tq, conn_id);
  }
  void Closed(const std::string &conn_id) {
    outbound.Remove(conn_id);
    subscriptions.RemoveConnection(conn_id);
  }
  void Dispatch(const std::shared_ptr<Strand> &strand,
                pod::ConnectionInfo conn, std::string message, bool binary) {
    const auto encoding =
        binary ? pod::WireEncoding::MsgPack : pod::WireEncoding::Json;
    strand->Post([&handler = handler, conn = std::move(conn),
                  msg = std::move(message), encoding] {
      handler.Handle(conn, msg, encoding);
    });
  }
};

// Router links: authenticates routers, then serves their virtual connections.
class ClusterListener {
public:
  ClusterListener(const pod::PodConfig &config, NodeServices &services)
      : _config(config), _services(services) {}

  bool Listen(uWS::Loop *loop) {
    _context = Link::CreateContext(loop, [this](Link &link) { Accept(link); });
    if (!_context)
      return false;
    return us_socket_context_listen(0, _context, nullptr,
                                    _config.internal_port, 0,
                                    sizeof(Link *)) != nullptr;
  }

private:
  struct VirtualConn {
    std::string conn_id;
    pod::AuthContext auth;
    std::shared_ptr<Strand> strand;
  };
  using ConnMap = std::unordered_map<uint64_t, VirtualConn>;

  void Accept(Link &link) {
    link.nonce = pod::cluster::MakeNonce();
    _links[link.Id()];
    link.SetHandlers([this](Link &l, Frame &&f) { OnFrame(l, std::move(f)); },
                     [this](Link &l, const std::string &why) {
                       OnClosed(l, why);
                     });
    link.Send(Frame{.type = FrameType::Challenge, .a = link.nonce});
  }

  void OnFrame(Link &link, Frame &&f) {
    if (!link.authenticated) {
      if (f.type != FrameType::Hello ||
          !pod::crypto::ConstantTimeEqual(
              f.a, pod::cluster::HandshakeMac(_config.cluster_secret, "router",
                                              link.nonce)) ||
          f.b.size() != 32) {
        Log(LogLevel::Warn, "cluster",
            "Rejected link " + std::to_string(link.Id()) +
                ": handshake failed");
        link.Close("handshake failed");
        return;
      }
      link.authenticated = true;
      link.Send(Frame{.type = FrameType::Welcome,
                      .a = pod::cluster::HandshakeMac(_config.cluster_secret,
                                                      "node", f.b)});
      Log(LogLevel::Info, "cluster",
          "Router link " + std::to_string(link.Id()) + " established");
      return;
    }
    ConnMap &conns = _links[link.Id()];
    switch (f.type) {
    case FrameType::Open: {
      if (conns.contains(f.conn)) {
        link.Close("duplicate connection id");
        return;
      }
      auto auth = pod::AuthenticateConnection(_config, f.a, f.b);
      if (!auth) {
        link.Send(Frame{.type = FrameType::Close,
                        .conn = f.conn,
                        .code = static_cast<uint16_t>(auth.error().ws_close_code),
                        .a = auth.error().reason});
        return;
      }
      const uint64_t vc = f.conn;
      const std::string conn_id =
          "link" + std::to_string(link.Id()) + ":" + std::to_string(vc);
      Link *l = &link;
      auto strand = _services.Open(
          conn_id,
          pod::Outbound{
              .send =
                  [l, vc](std::string_view payload, bool binary) {
                    return l->Send(Frame{.type = FrameType::Msg,
                                         .conn = vc,
                                         .binary = binary,
                                         .a = std::string(payload)});
                  },
              .close =
                  [this, l, vc](int code, std::string_view reason) {
                    l->Send(Frame{.type = FrameType::Close,
                                  .conn = vc,
                                  .code = static_cast<uint16_t>(code),
                                  .a = std::string(reason)});
                    Forget(*l, vc);
                  }});
      conns.emplace(vc, VirtualConn{conn_id, std::move(*auth), strand});
      return;
    }
    case FrameType::Msg: {
      auto it = conns.find(f.conn);
      if (it == conns.end())
        return; // e.g. its OPEN was rejected; the router is closing it
      _services.Dispatch(it->second.strand,
                         pod::ConnectionInfo{it->second.conn_id,
                                             it->second.auth},
                         std::move(f.a), f.binary);
      return;
    }
    case FrameType::Close:
      Forget(link, f.conn);
      return;
    default:
      link.Close("unexpected frame type");
    }
  }

  void Forget(Link &link, uint64_t vc) {
    auto links = _links.find(link.Id());
    if (links == _links.end())
      return;
    auto it = links->second.find(vc);
    if (it == links->second.end())
      return;
    _services.Closed(it->second.conn_id);
    links->second.erase(it);
  }

  void OnClosed(Link &link, const std::string &why) {
    auto it = _links.find(link.Id());
    if (it == _links.end())
      return;
    for (auto &[vc, conn] : it->second)
      _services.Closed(conn.conn_id);
    if (link.authenticated)
      Log(LogLevel::Warn, "cluster",
          "Router link " + std::to_string(link.Id()) + " closed (" + why +
              "), dropped " + std::to_string(it->second.size()) +
              " connection(s)");
    _links.erase(it);
  }

  const pod::PodConfig &_config;
  NodeServices &_services;
  us_socket_context_t *_context = nullptr;
  std::unordered_map<uint64_t, ConnMap> _links; // by link id
};

} // namespace

int entry_standalone(StorageDriver &d, const pod::PodConfig &config) {
  std::shared_ptr<const pod::rules::RuleSet> rules;
  if (!config.rules_path.empty()) {
    auto loaded = pod::rules::RuleSet::LoadFile(config.rules_path);
    if (!loaded) {
      Log(LogLevel::Error, "standalone",
          "Invalid rules (" + config.rules_path + "): " + loaded.error());
      return 1;
    }
    rules = *loaded;
    Log(LogLevel::Info, "standalone",
        "Loaded " + std::to_string(rules->Rules().size()) + " rule(s) from " +
            config.rules_path);
  } else {
    Log(LogLevel::Warn, "standalone",
        "No POD_RULES_PATH: running in open mode, every client may read and "
        "write everything in its namespace");
  }

  uWS::App app = uWS::App();
  uWS::Loop *loop = uWS::Loop::get();
  pod::OutboundRegistry outbound; // loop thread only

  // Serialise off the loop thread, then hop onto it to write the frame.
  pod::MessageSink sink = [loop, &outbound](const std::string &conn_id,
                                            json message,
                                            pod::WireEncoding encoding) {
    std::string payload;
    const bool binary = encoding == pod::WireEncoding::MsgPack;
    if (!binary) {
      payload = message.dump(-1, ' ', false, json::error_handler_t::replace);
    } else {
      const auto bytes = json::to_msgpack(message);
      payload.assign(bytes.begin(), bytes.end());
    }
    loop->defer([&outbound, conn_id, payload = std::move(payload), binary] {
      outbound.Send(conn_id, payload, binary);
    });
  };

  // Declaration order matters: the ThreadPool is declared last so it is
  // destroyed (and joined) first, while everything its tasks use is alive.
  TaskQueue tq;
  pod::Database db(d, pod::DatabaseOptions{.log_retention =
                                               config.log_retention});
  pod::SubscriptionManager subscriptions(db, sink);
  db.SetCommitListener([&subscriptions](const pod::CommitInfo &commit) {
    subscriptions.OnCommit(commit);
  });
  pod::rules::Authorizer authorizer(db, rules);
  pod::RequestHandler handler(db, subscriptions, authorizer, sink);
  NodeServices services{tq, subscriptions, handler, outbound};
  ClusterListener cluster(config, services);
  ThreadPool tp(tq, config.threads ? config.threads
                                   : std::thread::hardware_concurrency());
  auto gen = boost::uuids::random_generator();

  app.get("/", [](auto *res, auto * /*req*/) {
    res->end("Welcome to Pod DB!");
  });

  uWS::App::WebSocketBehavior<ConnectionState> behavior;
  behavior.maxPayloadLength = kMaxPayloadBytes;
  behavior.maxBackpressure = kMaxBackpressureBytes;
  behavior.closeOnBackpressureLimit = false; // handled explicitly in send
  behavior.upgrade = [&config](auto *res, auto *req, auto *context) {
    // Namespace and identity are fixed once, at connection time.
    auto auth = pod::AuthenticateConnection(config, req->getQuery("ns"),
                                            req->getQuery("t"));
    ConnectionState state;
    if (auth)
      state.auth = std::move(*auth);
    else
      state.rejected = std::move(auth.error());
    res->template upgrade<ConnectionState>(
        std::move(state), req->getHeader("sec-websocket-key"),
        req->getHeader("sec-websocket-protocol"),
        req->getHeader("sec-websocket-extensions"), context);
  };
  behavior.open = [&gen, &services](WebSocket *ws) {
    auto *state = ws->getUserData();
    if (state->rejected) {
      state->is_alive = false;
      ws->end(state->rejected->ws_close_code, state->rejected->reason);
      return;
    }
    state->conn_id = boost::uuids::to_string(gen());
    state->strand = services.Open(
        state->conn_id,
        pod::Outbound{
            .send =
                [ws](std::string_view payload, bool binary) {
                  auto *st = ws->getUserData();
                  if (!st->is_alive)
                    return false;
                  if (ws->send(payload, binary ? uWS::OpCode::BINARY
                                               : uWS::OpCode::TEXT) ==
                      WebSocket::SendStatus::DROPPED) {
                    // Never silently lose a response or event: drop the
                    // connection so the client resubscribes (resume_token).
                    Log(LogLevel::Warn, "standalone",
                        "Backpressure limit exceeded, closing (" +
                            st->conn_id + ")");
                    st->is_alive = false;
                    ws->end(1013, "Backpressure limit exceeded");
                    return false;
                  }
                  return true;
                },
            .close =
                [ws](int code, std::string_view reason) {
                  auto *st = ws->getUserData();
                  if (!st->is_alive)
                    return;
                  st->is_alive = false;
                  ws->end(code, reason);
                }});
    Log(LogLevel::Info, "standalone",
        "Connection opened (" + state->conn_id + ") namespace [" +
            state->auth.ns + "]" +
            (state->auth.token ? " uid [" + state->auth.token->uid + "]"
                               : std::string(" anonymous")));
  };
  behavior.message = [&services](WebSocket *ws, std::string_view message,
                                 uWS::OpCode op) {
    if (op != uWS::OpCode::TEXT && op != uWS::OpCode::BINARY)
      return;
    auto *state = ws->getUserData();
    if (!state->is_alive)
      return;
    services.Dispatch(state->strand,
                      pod::ConnectionInfo{state->conn_id, state->auth},
                      std::string(message), op == uWS::OpCode::BINARY);
  };
  behavior.close = [&services](WebSocket *ws, int /*code*/,
                               std::string_view /*reason*/) {
    auto *state = ws->getUserData();
    state->is_alive = false;
    if (state->rejected)
      return;
    services.Closed(state->conn_id);
    Log(LogLevel::Info, "standalone",
        "Connection closed (" + state->conn_id + ")");
  };
  app.ws<ConnectionState>("/webchannel", std::move(behavior));

  bool listening = false;
  app.listen(config.port, [&](auto *listen_socket) {
    listening = listen_socket != nullptr;
    if (listening)
      Log(LogLevel::Info, "standalone",
          "Pod listening on port " + std::to_string(config.port) +
              " (default namespace: " + config.default_namespace + ")");
  });
  if (!listening) {
    Log(LogLevel::Error, "standalone",
        "Failed to listen on port " + std::to_string(config.port));
    return 1;
  }
  if (config.internal_port) {
    if (!cluster.Listen(loop)) {
      Log(LogLevel::Error, "standalone",
          "Failed to listen for cluster links on port " +
              std::to_string(config.internal_port));
      return 1;
    }
    Log(LogLevel::Info, "standalone",
        "Accepting router links on port " +
            std::to_string(config.internal_port));
  }

  app.run();
  return 0;
}
