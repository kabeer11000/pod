#include "cluster/link.h"
#include "rules/crypto.h"
#include "uwebsockets/Loop.h"
#include <random>

namespace pod::cluster {

namespace {

constexpr unsigned int kConnectTimeoutSeconds = 5;
// Silence for this long triggers a PING; silence for twice as long closes.
constexpr unsigned int kIdleSeconds = 15;
// Bytes queued for a peer that is not reading before the link is dropped.
constexpr size_t kMaxBufferedBytes = 512u * 1024 * 1024;

uint64_t g_next_id = 1; // loop thread only

Link *LinkOf(us_socket_t *s) { return *static_cast<Link **>(us_socket_ext(0, s)); }

} // namespace

Link::Link(us_socket_t *s) : _s(s), _id(g_next_id++) {
  *static_cast<Link **>(us_socket_ext(0, s)) = this;
}

us_socket_context_t *Link::CreateContext(uWS::Loop *loop,
                                         AcceptHandler on_accept) {
  us_socket_context_options_t options{};
  auto *ctx = us_create_socket_context(
      0, reinterpret_cast<us_loop_t *>(loop), sizeof(AcceptHandler *), options);
  if (!ctx)
    return nullptr;
  // ponytail: one heap allocation per context, intentionally never freed
  // (contexts live for the whole process).
  *static_cast<AcceptHandler **>(us_socket_context_ext(0, ctx)) =
      new AcceptHandler(std::move(on_accept));
  us_socket_context_on_open(0, ctx, &Link::OnOpen);
  us_socket_context_on_data(0, ctx, &Link::OnData);
  us_socket_context_on_writable(0, ctx, &Link::OnWritable);
  us_socket_context_on_close(0, ctx, &Link::OnClose);
  us_socket_context_on_end(0, ctx, &Link::OnEnd);
  us_socket_context_on_timeout(0, ctx, &Link::OnTimeout);
  us_socket_context_on_connect_error(0, ctx, &Link::OnConnectError);
  return ctx;
}

Link *Link::Connect(us_socket_context_t *context, const std::string &host,
                    int port, FrameHandler on_frame, CloseHandler on_close) {
  us_socket_t *s = us_socket_context_connect(0, context, host.c_str(), port,
                                             nullptr, 0, sizeof(Link *));
  if (!s)
    return nullptr;
  auto *link = new Link(s);
  link->SetHandlers(std::move(on_frame), std::move(on_close));
  us_socket_timeout(0, s, kConnectTimeoutSeconds);
  return link;
}

void Link::SetHandlers(FrameHandler on_frame, CloseHandler on_close) {
  _on_frame = std::move(on_frame);
  _on_close = std::move(on_close);
}

bool Link::Send(const Frame &frame) {
  if (_closed)
    return false;
  return WriteRaw(EncodeFrame(frame));
}

bool Link::WriteRaw(std::string_view bytes) {
  if (!_connected || _out.size() > _out_offset) {
    // Not connected yet or already backed up: preserve order by queueing.
    if (_out.size() - _out_offset + bytes.size() > kMaxBufferedBytes) {
      Close("peer is not draining the link");
      return false;
    }
    _out.append(bytes);
    return true;
  }
  const int written = us_socket_write(0, _s, bytes.data(),
                                      static_cast<int>(bytes.size()), 0);
  if (written < static_cast<int>(bytes.size()))
    _out.append(bytes.substr(written < 0 ? 0 : written));
  return true;
}

void Link::Flush() {
  while (!_closed && _connected && _out.size() > _out_offset) {
    const size_t pending = _out.size() - _out_offset;
    const int chunk = static_cast<int>(std::min<size_t>(pending, 1u << 30));
    const int written =
        us_socket_write(0, _s, _out.data() + _out_offset, chunk, 0);
    if (written <= 0)
      break;
    _out_offset += written;
    if (written < chunk)
      break;
  }
  if (_out_offset == _out.size()) {
    _out.clear();
    _out_offset = 0;
  } else if (_out_offset > (64u << 20)) {
    _out.erase(0, _out_offset);
    _out_offset = 0;
  }
}

void Link::Close(const std::string &why) {
  if (_closed)
    return;
  if (!_connected) {
    // Still connecting: this closes without emitting any further event.
    us_socket_close_connecting(0, _s);
    Finish(why);
    return;
  }
  // Emits OnClose synchronously, which calls Finish.
  us_socket_close(0, _s, 0, const_cast<char *>(why.c_str()));
}

void Link::Finish(const std::string &why) {
  if (_closed)
    return;
  _closed = true;
  if (_on_close)
    _on_close(*this, why);
  // Callers further up the stack may still hold `this`; free it later.
  uWS::Loop::get()->defer([self = this] { delete self; });
}

us_socket_t *Link::OnOpen(us_socket_t *s, int is_client, char *, int) {
  Link *link;
  if (is_client) {
    link = LinkOf(s);
  } else {
    link = new Link(s);
    auto *accept = *static_cast<AcceptHandler **>(
        us_socket_context_ext(0, us_socket_context(0, s)));
    if (!accept || !*accept) {
      link->_connected = true;
      link->Close("not accepting connections");
      return s;
    }
    link->_connected = true;
    (*accept)(*link);
  }
  link->_connected = true;
  if (link->_closed)
    return s;
  us_socket_timeout(0, s, kIdleSeconds);
  link->Flush();
  return s;
}

us_socket_t *Link::OnData(us_socket_t *s, char *data, int length) {
  Link *link = LinkOf(s);
  if (link->_closed)
    return s;
  link->_awaiting_pong = false;
  us_socket_timeout(0, s, kIdleSeconds);
  auto ok = link->_reader.Feed(std::string_view(data, length));
  // Deliver every frame decoded before any error, in order.
  while (!link->_closed && link->_reader.HasFrame()) {
    Frame f = link->_reader.Pop();
    if (f.type == FrameType::Ping) {
      link->Send(Frame{.type = FrameType::Pong});
      continue;
    }
    if (f.type == FrameType::Pong)
      continue;
    if (link->_on_frame)
      link->_on_frame(*link, std::move(f));
  }
  if (!ok && !link->_closed)
    link->Close("protocol error: " + ok.error());
  return s;
}

us_socket_t *Link::OnWritable(us_socket_t *s) {
  LinkOf(s)->Flush();
  return s;
}

us_socket_t *Link::OnClose(us_socket_t *s, int, void *) {
  LinkOf(s)->Finish("connection closed");
  return s;
}

us_socket_t *Link::OnEnd(us_socket_t *s) {
  // The peer half-closed; we have nothing more to say either.
  return us_socket_close(0, s, 0, nullptr);
}

us_socket_t *Link::OnTimeout(us_socket_t *s) {
  Link *link = LinkOf(s);
  if (!link->_connected) {
    link->Close("connect timed out");
    return s;
  }
  if (link->_awaiting_pong) {
    link->Close("peer stopped responding");
    return s;
  }
  link->_awaiting_pong = true;
  link->Send(Frame{.type = FrameType::Ping});
  if (!link->_closed)
    us_socket_timeout(0, s, kIdleSeconds);
  return s;
}

us_socket_t *Link::OnConnectError(us_socket_t *s, int) {
  // uSockets closes the socket after this without emitting OnClose.
  LinkOf(s)->Finish("connection failed");
  return s;
}

std::string MakeNonce() {
  std::random_device rd;
  std::string nonce(32, '\0');
  for (auto &c : nonce)
    c = static_cast<char>(rd() & 0xff);
  return nonce;
}

std::string HandshakeMac(std::string_view secret, std::string_view role,
                         std::string_view nonce) {
  const auto mac = crypto::HmacSha256(
      secret, std::string("pod-cluster-v1:") + std::string(role) + ":" +
                  std::string(nonce));
  return std::string(reinterpret_cast<const char *>(mac.data()), mac.size());
}

} // namespace pod::cluster
