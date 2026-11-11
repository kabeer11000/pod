#include "rules/crypto.h"
#include <algorithm>
#include <cstring>

namespace pod::crypto {

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

class Sha256State {
public:
  void Update(const uint8_t *data, size_t len) {
    _length += len;
    while (len > 0) {
      const size_t take = std::min(len, size_t(64) - _buffered);
      std::memcpy(_buffer + _buffered, data, take);
      _buffered += take;
      data += take;
      len -= take;
      if (_buffered == 64) {
        Compress(_buffer);
        _buffered = 0;
      }
    }
  }

  std::array<uint8_t, 32> Finish() {
    const uint64_t bit_length = _length * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (_buffered != 56)
      Update(&zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i)
      len_bytes[i] = static_cast<uint8_t>(bit_length >> (56 - 8 * i));
    Update(len_bytes, 8);
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j)
        out[static_cast<size_t>(4 * i + j)] =
            static_cast<uint8_t>(_h[i] >> (24 - 8 * j));
    return out;
  }

private:
  void Compress(const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) |
             (uint32_t(block[4 * i + 2]) << 8) | uint32_t(block[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3], e = _h[4], f = _h[5],
             g = _h[6], h = _h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
      const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    _h[0] += a;
    _h[1] += b;
    _h[2] += c;
    _h[3] += d;
    _h[4] += e;
    _h[5] += f;
    _h[6] += g;
    _h[7] += h;
  }

  uint32_t _h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t _buffer[64] = {};
  size_t _buffered = 0;
  uint64_t _length = 0;
};

const uint8_t *Bytes(std::string_view s) {
  return reinterpret_cast<const uint8_t *>(s.data());
}

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int DecodeChar(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '-')
    return 62;
  if (c == '_')
    return 63;
  return -1;
}

} // namespace

std::array<uint8_t, 32> Sha256(std::string_view data) {
  Sha256State s;
  s.Update(Bytes(data), data.size());
  return s.Finish();
}

std::array<uint8_t, 32> HmacSha256(std::string_view key, std::string_view data) {
  uint8_t k[64] = {};
  if (key.size() > 64) {
    const auto hk = Sha256(key);
    std::memcpy(k, hk.data(), hk.size());
  } else if (!key.empty()) {
    std::memcpy(k, key.data(), key.size());
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  Sha256State inner;
  inner.Update(ipad, 64);
  inner.Update(Bytes(data), data.size());
  const auto inner_hash = inner.Finish();
  Sha256State outer;
  outer.Update(opad, 64);
  outer.Update(inner_hash.data(), inner_hash.size());
  return outer.Finish();
}

std::string Base64UrlEncode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() * 4 + 2) / 3);
  size_t i = 0;
  const auto *b = Bytes(bytes);
  for (; i + 3 <= bytes.size(); i += 3) {
    const uint32_t v = (uint32_t(b[i]) << 16) | (uint32_t(b[i + 1]) << 8) | b[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  const size_t rem = bytes.size() - i;
  if (rem == 1) {
    const uint32_t v = uint32_t(b[i]) << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
  } else if (rem == 2) {
    const uint32_t v = (uint32_t(b[i]) << 16) | (uint32_t(b[i + 1]) << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
  }
  return out;
}

std::optional<std::string> Base64UrlDecode(std::string_view text) {
  // Strip padding, which is only valid when it completes a 4-char group.
  size_t len = text.size();
  size_t pad = 0;
  while (len > 0 && text[len - 1] == '=') {
    --len;
    ++pad;
  }
  if (pad > 0) {
    if (pad > 2 || text.size() % 4 != 0)
      return std::nullopt;
  }
  if (len % 4 == 1)
    return std::nullopt;
  if (pad > 0 && (len % 4) + pad != 4)
    return std::nullopt;

  std::string out;
  out.reserve(len * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; ++i) {
    const int v = DecodeChar(text[i]);
    if (v < 0)
      return std::nullopt;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  // Leftover bits (2 or 4) must be zero for a canonical encoding.
  if (bits > 0 && (acc & ((1u << bits) - 1)) != 0)
    return std::nullopt;
  return out;
}

bool ConstantTimeEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff = diff | static_cast<uint8_t>(static_cast<uint8_t>(a[i]) ^
                                       static_cast<uint8_t>(b[i]));
  return diff == 0;
}

std::string ToHex(std::span<const uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out += kHex[b >> 4];
    out += kHex[b & 15];
  }
  return out;
}

} // namespace pod::crypto
