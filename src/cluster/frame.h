// Framing for router <-> node cluster links (raw TCP).
//
// Every frame: [u32 length][u8 type][u64 conn][u16 code][u8 binary]
//              [u32 a_length][a bytes][b bytes]
// All integers big-endian; `length` counts everything after itself. Fields a
// type does not use are zero / empty.
//
//   CHALLENGE  node -> router   a = node nonce
//   HELLO      router -> node   a = HMAC(secret, "router" + node nonce),
//                               b = router nonce
//   WELCOME    node -> router   a = HMAC(secret, "node" + router nonce)
//   OPEN       router -> node   conn, a = namespace, b = token
//   CLOSE      both             conn, code (WebSocket close code), a = reason
//   MSG        both             conn, binary, a = wire protocol frame
//   PING/PONG  both             keepalive
#pragma once
#include <cstdint>
#include <deque>
#include <expected>
#include <string>
#include <string_view>

namespace pod::cluster {

enum class FrameType : uint8_t {
  Challenge = 1,
  Hello = 2,
  Welcome = 3,
  Open = 4,
  Close = 5,
  Msg = 6,
  Ping = 7,
  Pong = 8,
};

struct Frame {
  FrameType type{};
  uint64_t conn = 0;
  uint16_t code = 0;
  bool binary = false;
  std::string a = {};
  std::string b = {};
};

// Largest accepted frame (after the length prefix). Client frames are capped
// at 16 MiB by the WebSocket layer; responses (e.g. large snapshots) may be
// bigger.
constexpr uint32_t kMaxFrameBytes = 256u * 1024 * 1024;
constexpr size_t kFrameHeaderBytes = 4 + 1 + 8 + 2 + 1 + 4;

std::string EncodeFrame(const Frame &f);

// Incremental decoder for a byte stream.
class FrameReader {
public:
  // Appends bytes and decodes every complete frame. On error the stream is
  // unusable and the link must be closed.
  std::expected<void, std::string> Feed(std::string_view bytes);
  bool HasFrame() const { return !_frames.empty(); }
  Frame Pop();

private:
  std::string _buffer;
  size_t _offset = 0; // start of the undecoded bytes in _buffer
  std::deque<Frame> _frames;
};

} // namespace pod::cluster
