// Standalone Pod: a single database node serving the wire protocol over
// WebSockets at ws://<host>:<port>/webchannel?ns=<namespace>
//
// Threading model:
//   * One uWS event loop thread owns every socket.
//   * Requests are executed on a ThreadPool; each connection has a Strand so
//     its requests run strictly in arrival order.
//   * Responses and events are serialised on the worker and handed back to
//     the loop thread with loop->defer (FIFO), preserving per-connection order.
#include "common/log.h"
#include "common/strand.h"
#include "common/task_queue.h"
#include "common/thread_pool.h"
#include "db/database.h"
#include "db/path.h"
#include "entry/interface.h"
#include "network/ws/actions.h"
#include "network/ws/handlers/interface.h"
#include "network/ws/socket_registry.h"
#include "realtime/subscriptions.h"
#include "storage/interface.h"
#include "uwebsockets/App.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {

// Incoming frames larger than this close the connection.
constexpr unsigned int kMaxPayloadBytes = 16 * 1024 * 1024;
// Outgoing bytes buffered per socket before we give up on a slow consumer.
constexpr unsigned int kMaxBackpressureBytes = 64 * 1024 * 1024;

std::string GetEnv(const char *name) {
  const char *v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

template <typename T> T ParseNumber(const std::string &raw, const char *name) {
  T value{};
  auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
  if (ec != std::errc() || ptr != raw.data() + raw.size())
    throw std::invalid_argument(std::string(name) + " must be a number.");
  return value;
}

} // namespace

PodConfig LoadConfigFromEnv() {
  PodConfig c;
  if (auto v = GetEnv("POD_PORT"); !v.empty()) {
    c.port = ParseNumber<int>(v, "POD_PORT");
    if (c.port < 1 || c.port > 65535)
      throw std::invalid_argument("POD_PORT must be between 1 and 65535.");
  }
  if (auto v = GetEnv("POD_STORAGE_PATH"); !v.empty())
    c.storage_path = v;
  if (auto v = GetEnv("DEFAULT_NAMESPACE"); !v.empty()) {
    auto ns = pod::ValidateNamespace(v);
    if (!ns)
      throw std::invalid_argument("DEFAULT_NAMESPACE is invalid: " +
                                  ns.error().message);
    c.default_namespace = *ns;
  } else {
    c.default_namespace = std::string(pod::kFallbackNamespace);
  }
  if (auto v = GetEnv("POD_THREADS"); !v.empty())
    c.threads = ParseNumber<unsigned int>(v, "POD_THREADS");
  if (auto v = GetEnv("POD_SYNC_WRITES"); !v.empty())
    c.sync_writes = (v == "1" || v == "true");
  if (auto v = GetEnv("POD_LOG_LEVEL"); !v.empty()) {
    LogLevel level;
    if (!ParseLogLevel(v, level))
      throw std::invalid_argument(
          "POD_LOG_LEVEL must be one of debug, info, warn, error.");
    SetMinLogLevel(level);
  }
  return c;
}

int entry_standalone(StorageDriver &d, const PodConfig &config) {
  uWS::App app = uWS::App();
  uWS::Loop *loop = uWS::Loop::get();

  // Serialise off the loop thread, then hop onto it to write the frame.
  pod::MessageSink sink = [loop](const std::string &conn_id, json message,
                                 pod::WireEncoding encoding) {
    std::string payload;
    uWS::OpCode op;
    if (encoding == pod::WireEncoding::Json) {
      payload = message.dump(-1, ' ', false, json::error_handler_t::replace);
      op = uWS::OpCode::TEXT;
    } else {
      const auto bytes = json::to_msgpack(message);
      payload.assign(bytes.begin(), bytes.end());
      op = uWS::OpCode::BINARY;
    }
    loop->defer([conn_id, payload = std::move(payload), op]() {
      _ws *ws = global_socket_registry().Lookup(conn_id);
      if (!ws || !ws->getUserData()->is_alive)
        return;
      if (ws->send(payload, op) == _ws::SendStatus::DROPPED) {
        // Never silently lose a response or event: drop the connection so the
        // client reconnects and resubscribes (with its resume_token).
        Log(LogLevel::Warn, "standalone",
            "Backpressure limit exceeded, closing (" + conn_id + ")");
        ws->getUserData()->is_alive = false;
        ws->end(1013, "Backpressure limit exceeded");
      }
    });
  };

  // Declaration order matters: the ThreadPool is declared last so it is
  // destroyed (and joined) first, while everything its tasks use is alive.
  TaskQueue tq;
  pod::Database db(d);
  pod::SubscriptionManager subscriptions(db, sink);
  db.SetCommitListener([&subscriptions](const pod::CommitInfo &commit) {
    subscriptions.OnCommit(commit);
  });
  pod::RequestHandler handler(db, subscriptions, sink);
  ThreadPool tp(tq, config.threads ? config.threads
                                   : std::thread::hardware_concurrency());
  auto gen = boost::uuids::random_generator();

  app.get("/", [](auto *res, auto * /*req*/) {
    res->end("Welcome to Pod DB!");
  });

  uWS::App::WebSocketBehavior<ConnectionState> behavior;
  behavior.maxPayloadLength = kMaxPayloadBytes;
  behavior.maxBackpressure = kMaxBackpressureBytes;
  behavior.closeOnBackpressureLimit = false; // handled explicitly in the sink
  behavior.upgrade = [&config](auto *res, auto *req, auto *context) {
    // The namespace (scope) is selected once, at connection time.
    const std::string_view requested = req->getQuery("ns");
    const std::string candidate = requested.empty()
                                      ? config.default_namespace
                                      : std::string(requested);
    auto ns = pod::ValidateNamespace(candidate);
    if (!ns) {
      res->writeStatus("400 Bad Request")->end(ns.error().message);
      return;
    }
    ConnectionState state;
    state.ns = *ns;
    res->template upgrade<ConnectionState>(
        std::move(state), req->getHeader("sec-websocket-key"),
        req->getHeader("sec-websocket-protocol"),
        req->getHeader("sec-websocket-extensions"), context);
  };
  behavior.open = [&gen, &tq, &subscriptions](auto *ws) {
    auto *state = ws->getUserData();
    state->owning_loop = uWS::Loop::get();
    state->conn_id = boost::uuids::to_string(gen());
    state->strand = std::make_shared<Strand>(tq, state->conn_id);
    global_socket_registry().Register(state->conn_id, ws);
    subscriptions.AddConnection(state->conn_id);
    Log(LogLevel::Info, "standalone",
        "Connection opened (" + state->conn_id + ") namespace [" + state->ns +
            "]");
  };
  behavior.message = [&handler](auto *ws, std::string_view message,
                                uWS::OpCode op) {
    if (op != uWS::OpCode::TEXT && op != uWS::OpCode::BINARY)
      return;
    auto *state = ws->getUserData();
    const auto encoding = op == uWS::OpCode::BINARY
                              ? pod::WireEncoding::MsgPack
                              : pod::WireEncoding::Json;
    state->strand->Post([&handler,
                         conn = pod::ConnectionInfo{state->conn_id, state->ns},
                         msg = std::string(message), encoding] {
      handler.Handle(conn, msg, encoding);
    });
  };
  behavior.close = [&subscriptions](auto *ws, int /*code*/,
                                    std::string_view /*reason*/) {
    auto *state = ws->getUserData();
    state->is_alive = false;
    global_socket_registry().Remove(state->conn_id);
    subscriptions.RemoveConnection(state->conn_id);
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

  app.run();
  return 0;
}
