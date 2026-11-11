#include "cluster/frame.h"

namespace pod::cluster {

namespace {

void PutU(std::string &out, uint64_t v, int bytes) {
  for (int i = bytes - 1; i >= 0; --i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint64_t GetU(const char *p, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i)
    v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}

bool KnownType(uint8_t t) {
  return t >= static_cast<uint8_t>(FrameType::Challenge) &&
         t <= static_cast<uint8_t>(FrameType::Pong);
}

} // namespace

std::string EncodeFrame(const Frame &f) {
  const uint64_t length = kFrameHeaderBytes - 4 + f.a.size() + f.b.size();
  std::string out;
  out.reserve(4 + length);
  PutU(out, length, 4);
  PutU(out, static_cast<uint8_t>(f.type), 1);
  PutU(out, f.conn, 8);
  PutU(out, f.code, 2);
  PutU(out, f.binary ? 1 : 0, 1);
  PutU(out, f.a.size(), 4);
  out += f.a;
  out += f.b;
  return out;
}

std::expected<void, std::string> FrameReader::Feed(std::string_view bytes) {
  _buffer.append(bytes);
  while (_buffer.size() - _offset >= 4) {
    const char *p = _buffer.data() + _offset;
    const uint64_t length = GetU(p, 4);
    if (length < kFrameHeaderBytes - 4 || length > kMaxFrameBytes)
      return std::unexpected("invalid frame length " + std::to_string(length));
    if (_buffer.size() - _offset - 4 < length)
      break; // incomplete
    const uint8_t type = static_cast<uint8_t>(p[4]);
    if (!KnownType(type))
      return std::unexpected("unknown frame type " + std::to_string(type));
    const uint8_t binary = static_cast<uint8_t>(p[15]);
    if (binary > 1)
      return std::unexpected("invalid binary flag");
    const uint64_t a_len = GetU(p + 16, 4);
    if (a_len > length - (kFrameHeaderBytes - 4))
      return std::unexpected("field length exceeds frame");
    Frame f;
    f.type = static_cast<FrameType>(type);
    f.conn = GetU(p + 5, 8);
    f.code = static_cast<uint16_t>(GetU(p + 13, 2));
    f.binary = binary == 1;
    const char *body = p + kFrameHeaderBytes;
    f.a.assign(body, a_len);
    f.b.assign(body + a_len, length - (kFrameHeaderBytes - 4) - a_len);
    _frames.push_back(std::move(f));
    _offset += 4 + length;
  }
  // Compact once the consumed prefix dominates the buffer.
  if (_offset > 0 && _offset * 2 >= _buffer.size()) {
    _buffer.erase(0, _offset);
    _offset = 0;
  }
  return {};
}

Frame FrameReader::Pop() {
  Frame f = std::move(_frames.front());
  _frames.pop_front();
  return f;
}

} // namespace pod::cluster
