// Cluster link framing, the consistent-hash ring and configuration parsing.
#include "cluster/frame.h"
#include "cluster/link.h"
#include "cluster/ring.h"
#include "server/config.h"
#include "test.h"
#include <cstdlib>
#include <map>
#include <random>
#include <stdexcept>

using namespace pod;
using namespace pod::cluster;

namespace {

bool SameFrame(const Frame &x, const Frame &y) {
  return x.type == y.type && x.conn == y.conn && x.code == y.code &&
         x.binary == y.binary && x.a == y.a && x.b == y.b;
}

std::vector<Frame> SampleFrames() {
  std::string binary_payload;
  for (int i = 0; i < 300; ++i)
    binary_payload.push_back(static_cast<char>(i & 0xff));
  return {
      Frame{.type = FrameType::Challenge, .a = std::string(32, 'n')},
      Frame{.type = FrameType::Hello, .a = std::string(32, 'm'), .b = "nonce"},
      Frame{.type = FrameType::Open, .conn = 0xfedcba9876543210ull, .a = "ns",
            .b = "tok.en.sig"},
      Frame{.type = FrameType::Msg, .conn = 7, .binary = true,
            .a = binary_payload},
      Frame{.type = FrameType::Msg, .conn = 7, .a = R"({"meta":{}})"},
      Frame{.type = FrameType::Close, .conn = 1, .code = 4403, .a = "no"},
      Frame{.type = FrameType::Ping},
      Frame{.type = FrameType::Pong},
      Frame{.type = FrameType::Msg, .conn = 2}, // empty payload
  };
}

std::vector<Frame> Drain(FrameReader &r) {
  std::vector<Frame> out;
  while (r.HasFrame())
    out.push_back(r.Pop());
  return out;
}

void SetEnv(const char *k, const char *v) {
  if (v)
    setenv(k, v, 1);
  else
    unsetenv(k);
}

const char *kEnvVars[] = {"POD_ROLE", "POD_PORT", "POD_NODES",
                          "POD_CLUSTER_SECRET", "POD_INTERNAL_PORT",
                          "POD_VIRTUAL_NODES", "POD_REQUIRE_AUTH",
                          "POD_AUTH_SECRET", "DEFAULT_NAMESPACE"};

void ClearEnv() {
  for (const char *k : kEnvVars)
    unsetenv(k);
}

bool ConfigThrows() {
  try {
    LoadConfigFromEnv();
    return false;
  } catch (const std::invalid_argument &) {
    return true;
  }
}

} // namespace

TEST(TestFrameRoundTrip) {
  const auto frames = SampleFrames();
  std::string stream;
  for (const auto &f : frames)
    stream += EncodeFrame(f);

  // Whole stream at once.
  FrameReader whole;
  CHECK(whole.Feed(stream).has_value());
  auto got = Drain(whole);
  CHECK_EQ(got.size(), frames.size());
  for (size_t i = 0; i < got.size() && i < frames.size(); ++i)
    CHECK(SameFrame(got[i], frames[i]));

  // Byte by byte, and in random chunk sizes.
  FrameReader bytes;
  std::vector<Frame> got_bytes;
  for (char c : stream) {
    CHECK(bytes.Feed(std::string_view(&c, 1)).has_value());
    for (auto &f : Drain(bytes))
      got_bytes.push_back(std::move(f));
  }
  CHECK_EQ(got_bytes.size(), frames.size());

  std::mt19937 rng(7);
  for (int round = 0; round < 50; ++round) {
    FrameReader chunked;
    std::vector<Frame> out;
    size_t pos = 0;
    while (pos < stream.size()) {
      const size_t n = std::min<size_t>(stream.size() - pos, 1 + rng() % 97);
      CHECK(chunked.Feed(std::string_view(stream).substr(pos, n)).has_value());
      pos += n;
      for (auto &f : Drain(chunked))
        out.push_back(std::move(f));
    }
    bool same = out.size() == frames.size();
    for (size_t i = 0; same && i < out.size(); ++i)
      same = SameFrame(out[i], frames[i]);
    CHECK(same);
  }
}

TEST(TestFrameRejectsGarbage) {
  // Length smaller than the fixed header.
  FrameReader short_len;
  CHECK(!short_len.Feed(std::string("\0\0\0\x03" "abc", 7)));
  // Length over the cap.
  FrameReader huge;
  CHECK(!huge.Feed(std::string("\xff\xff\xff\xff", 4)));
  // Unknown type.
  std::string bad = EncodeFrame(Frame{.type = FrameType::Ping});
  bad[4] = 99;
  FrameReader unknown;
  CHECK(!unknown.Feed(bad));
  // Field length beyond the frame.
  std::string overflow = EncodeFrame(Frame{.type = FrameType::Msg, .a = "x"});
  overflow[19] = 50;
  FrameReader over;
  CHECK(!over.Feed(overflow));
  // Binary flag must be 0/1.
  std::string flag = EncodeFrame(Frame{.type = FrameType::Msg});
  flag[15] = 2;
  FrameReader bad_flag;
  CHECK(!bad_flag.Feed(flag));
  // An incomplete frame is not an error.
  const std::string ok = EncodeFrame(Frame{.type = FrameType::Msg, .a = "abc"});
  FrameReader partial;
  CHECK(partial.Feed(ok.substr(0, ok.size() - 1)).has_value());
  CHECK(!partial.HasFrame());
  CHECK(partial.Feed(ok.substr(ok.size() - 1)).has_value());
  CHECK(partial.HasFrame() && partial.Pop().a == "abc");
}

TEST(TestHandshakeMac) {
  const std::string nonce(32, 'x');
  CHECK_EQ(HandshakeMac("secret", "router", nonce).size(), 32u);
  CHECK(HandshakeMac("secret", "router", nonce) ==
        HandshakeMac("secret", "router", nonce));
  // Role-bound (a node cannot replay a router's answer) and secret-bound.
  CHECK(HandshakeMac("secret", "router", nonce) !=
        HandshakeMac("secret", "node", nonce));
  CHECK(HandshakeMac("secret", "router", nonce) !=
        HandshakeMac("secreT", "router", nonce));
  CHECK(MakeNonce().size() == 32 && MakeNonce() != MakeNonce());
}

TEST(TestRingDistributionAndStability) {
  const Ring three({"a", "b", "c"}, 256);
  const Ring same({"a", "b", "c"}, 256);
  const Ring four({"a", "b", "c", "d"}, 256);
  std::map<std::string, int> counts;
  int moved = 0;
  int moved_elsewhere = 0;
  const int keys = 30000;
  for (int i = 0; i < keys; ++i) {
    const std::string key = "ns-" + std::to_string(i);
    const std::string &owner = three.Owner(key);
    ++counts[owner];
    CHECK_EQ(owner, same.Owner(key)); // deterministic
    const std::string &after = four.Owner(key);
    if (after != owner) {
      ++moved;
      if (after != "d")
        ++moved_elsewhere;
    }
  }
  for (const auto &[node, n] : counts) {
    CHECK(n > keys / 3 * 0.8);
    CHECK(n < keys / 3 * 1.2);
  }
  // Adding a node only moves keys onto the new node, about 1/4 of them.
  CHECK_EQ(moved_elsewhere, 0);
  CHECK(moved > keys / 4 * 0.8);
  CHECK(moved < keys / 4 * 1.2);
  // A single node owns everything.
  const Ring one({"solo"}, 1);
  CHECK_EQ(one.Owner("anything"), std::string("solo"));
  CHECK(!one.Empty());
  CHECK(Ring({}, 16).Empty());
}

TEST(TestConfigFromEnv) {
  ClearEnv();
  auto c = LoadConfigFromEnv();
  CHECK_EQ(c.role, std::string("standalone"));
  CHECK(c.nodes.empty() && c.internal_port == 0);

  SetEnv("POD_ROLE", "router");
  CHECK(ConfigThrows()); // needs POD_NODES and a secret
  SetEnv("POD_NODES", "n1=10.0.0.1:4000,n2=db-2.internal:4001");
  CHECK(ConfigThrows()); // still no secret
  SetEnv("POD_CLUSTER_SECRET", "short");
  CHECK(ConfigThrows());
  SetEnv("POD_CLUSTER_SECRET", "0123456789abcdef");
  c = LoadConfigFromEnv();
  CHECK_EQ(c.nodes.size(), 2u);
  CHECK(c.nodes.size() == 2 && c.nodes[1].id == "n2" &&
        c.nodes[1].host == "db-2.internal" && c.nodes[1].port == 4001);

  for (const char *bad : {"n1=host", "=h:1", "n1=:1", "n1=h:0", "n1=h:x",
                          "n1=h:1,n1=h:2"}) {
    SetEnv("POD_NODES", bad);
    CHECK(ConfigThrows());
  }
  SetEnv("POD_NODES", "n1=h:1");
  SetEnv("POD_VIRTUAL_NODES", "0");
  CHECK(ConfigThrows());
  SetEnv("POD_VIRTUAL_NODES", "64");
  CHECK_EQ(LoadConfigFromEnv().virtual_nodes, 64u);

  ClearEnv();
  SetEnv("POD_INTERNAL_PORT", "4000");
  CHECK(ConfigThrows()); // node links need the secret too
  SetEnv("POD_CLUSTER_SECRET", "0123456789abcdef");
  CHECK_EQ(LoadConfigFromEnv().internal_port, 4000);
  SetEnv("POD_PORT", "70000");
  CHECK(ConfigThrows());
  ClearEnv();
  SetEnv("POD_REQUIRE_AUTH", "true");
  CHECK(ConfigThrows()); // nothing to verify tokens with
  SetEnv("POD_AUTH_SECRET", "s");
  CHECK(LoadConfigFromEnv().require_auth);
  ClearEnv();
  SetEnv("POD_ROLE", "primary");
  CHECK(ConfigThrows());
  ClearEnv();
}
