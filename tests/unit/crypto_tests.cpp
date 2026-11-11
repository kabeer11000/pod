// SHA-256, HMAC-SHA256, base64url and HS256 JWT tests.
#include "test.h"
#include "rules/crypto.h"
#include "rules/jwt.h"
#include <nlohmann/json.hpp>
#include <random>
#include <string>

using namespace pod;
using json = nlohmann::json;

namespace {
std::string Hex(const std::array<uint8_t, 32> &h) { return crypto::ToHex(h); }
std::string Rep(char c, size_t n) { return std::string(n, c); }
std::string Range(uint8_t from, uint8_t to) {
  std::string s;
  for (int c = from; c <= to; ++c)
    s += static_cast<char>(c);
  return s;
}
} // namespace

TEST(TestCryptoSha256Vectors) {
  CHECK_EQ(Hex(crypto::Sha256("")),
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_EQ(Hex(crypto::Sha256("abc")),
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_EQ(Hex(crypto::Sha256(
               "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK_EQ(Hex(crypto::Sha256(Rep('a', 1000000))),
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // Padding boundaries (55/56/63/64 bytes) must agree with the streaming path.
  for (size_t n : {55u, 56u, 63u, 64u, 65u, 119u, 120u})
    CHECK_EQ(crypto::Sha256(Rep('x', n)).size(), 32u);
  CHECK(crypto::Sha256(Rep('x', 55)) != crypto::Sha256(Rep('x', 56)));
}

TEST(TestCryptoHmacRfc4231) {
  CHECK_EQ(Hex(crypto::HmacSha256(Rep('\x0b', 20), "Hi There")),
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK_EQ(Hex(crypto::HmacSha256("Jefe", "what do ya want for nothing?")),
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  CHECK_EQ(Hex(crypto::HmacSha256(Rep('\xaa', 20), Rep('\xdd', 50))),
           "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
  CHECK_EQ(Hex(crypto::HmacSha256(Range(0x01, 0x19), Rep('\xcd', 50))),
           "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
  CHECK_EQ(Hex(crypto::HmacSha256(
               Rep('\xaa', 131),
               "Test Using Larger Than Block-Size Key - Hash Key First")),
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  CHECK_EQ(Hex(crypto::HmacSha256(
               Rep('\xaa', 131),
               "This is a test using a larger than block-size key and a larger "
               "than block-size data. The key needs to be hashed before being "
               "used by the HMAC algorithm.")),
           "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST(TestCryptoBase64Url) {
  CHECK_EQ(crypto::Base64UrlEncode(""), "");
  CHECK_EQ(crypto::Base64UrlEncode("f"), "Zg");
  CHECK_EQ(crypto::Base64UrlEncode("fo"), "Zm8");
  CHECK_EQ(crypto::Base64UrlEncode("foo"), "Zm9v");
  CHECK_EQ(crypto::Base64UrlEncode("\xfb\xff"), "-_8");
  CHECK(crypto::Base64UrlDecode("Zg") == std::optional<std::string>("f"));
  CHECK(crypto::Base64UrlDecode("Zg==") == std::optional<std::string>("f"));
  CHECK(crypto::Base64UrlDecode("Zm8=") == std::optional<std::string>("fo"));
  CHECK(crypto::Base64UrlDecode("Zm9v") == std::optional<std::string>("foo"));
  CHECK(crypto::Base64UrlDecode("") == std::optional<std::string>(""));
  CHECK(!crypto::Base64UrlDecode("Zg+v").has_value());  // '+' not url-safe
  CHECK(!crypto::Base64UrlDecode("Zg/v").has_value());  // '/' not url-safe
  CHECK(!crypto::Base64UrlDecode("Z").has_value());     // len % 4 == 1
  CHECK(!crypto::Base64UrlDecode("Zm9vZ").has_value()); // len % 4 == 1
  CHECK(!crypto::Base64UrlDecode("Zh").has_value());    // trailing bits set
  CHECK(!crypto::Base64UrlDecode("Zm9=").has_value());  // trailing bits set
  CHECK(!crypto::Base64UrlDecode("Zg=").has_value());   // incomplete padding
  CHECK(!crypto::Base64UrlDecode("Zm9v=").has_value()); // padding on full group
  CHECK(!crypto::Base64UrlDecode("Z===").has_value());
  CHECK(!crypto::Base64UrlDecode("Zg==Zg==").has_value());

  std::mt19937 rng(7);
  for (size_t len = 0; len <= 70; ++len) {
    for (int rep = 0; rep < 5; ++rep) {
      std::string raw(len, '\0');
      for (auto &c : raw)
        c = static_cast<char>(rng() & 0xFF);
      const std::string enc = crypto::Base64UrlEncode(raw);
      CHECK(enc.find('=') == std::string::npos);
      CHECK(crypto::Base64UrlDecode(enc) == std::optional<std::string>(raw));
      std::string padded = enc;
      while (padded.size() % 4)
        padded += '=';
      CHECK(crypto::Base64UrlDecode(padded) == std::optional<std::string>(raw));
    }
  }
}

TEST(TestCryptoConstantTimeEqualAndHex) {
  CHECK(crypto::ConstantTimeEqual("abc", "abc"));
  CHECK(!crypto::ConstantTimeEqual("abc", "abd"));
  CHECK(!crypto::ConstantTimeEqual("abc", "ab"));
  CHECK(crypto::ConstantTimeEqual("", ""));
  const uint8_t bytes[] = {0x00, 0x0f, 0xa5, 0xff};
  CHECK_EQ(crypto::ToHex(bytes), "000fa5ff");
}

namespace {
const int64_t kNow = 1'800'000'000;

std::string MakeToken(const json &header, const json &payload,
                      std::string_view secret) {
  std::string input = crypto::Base64UrlEncode(header.dump()) + "." +
                      crypto::Base64UrlEncode(payload.dump());
  const auto mac = crypto::HmacSha256(secret, input);
  return input + "." +
         crypto::Base64UrlEncode(std::string_view(
             reinterpret_cast<const char *>(mac.data()), mac.size()));
}
} // namespace

TEST(TestJwtValid) {
  const json claims = {{"sub", "user-1"}, {"exp", kNow + 60}, {"admin", true}};
  const std::string token = auth::SignJwtHs256(claims, "s3cret");
  auto v = auth::VerifyJwtHs256(token, "s3cret", kNow);
  CHECK(v.has_value());
  CHECK_EQ(v->uid, "user-1");
  CHECK(v->expires_at == std::optional<int64_t>(kNow + 60));
  CHECK_EQ(v->claims["admin"], json(true));
  // No exp: valid, expires_at empty.
  auto noexp = auth::VerifyJwtHs256(auth::SignJwtHs256({{"sub", "u"}}, "k"), "k", kNow);
  CHECK(noexp && !noexp->expires_at);
  // Float time claims accepted.
  auto fexp = auth::VerifyJwtHs256(
      auth::SignJwtHs256({{"sub", "u"}, {"exp", double(kNow) + 0.5}}, "k"), "k", kNow);
  CHECK(fexp.has_value());
}

TEST(TestJwtRejections) {
  const json ok_claims = {{"sub", "u"}, {"exp", kNow + 60}};
  const std::string good = auth::SignJwtHs256(ok_claims, "k");
  auto fails = [](const std::string &t, std::string_view secret = "k") {
    return !auth::VerifyJwtHs256(t, secret, kNow).has_value();
  };
  CHECK(!fails(good));
  CHECK(fails(good, ""));            // empty secret
  CHECK(fails(good, "other"));       // wrong secret
  CHECK(fails(""));
  CHECK(fails("a.b"));
  CHECK(fails(good + ".x"));         // 4 parts
  CHECK(fails("..."));

  // Tampered payload with original signature.
  const size_t d1 = good.find('.'), d2 = good.rfind('.');
  const std::string forged_payload = crypto::Base64UrlEncode(
      json({{"sub", "admin"}, {"exp", kNow + 60}}).dump());
  CHECK(fails(good.substr(0, d1 + 1) + forged_payload + good.substr(d2)));
  // Tampered signature.
  std::string badsig = good;
  badsig.back() = badsig.back() == 'A' ? 'B' : 'A';
  CHECK(fails(badsig));

  // Algorithms.
  CHECK(fails(MakeToken({{"alg", "none"}}, ok_claims, "k")));
  CHECK(fails(MakeToken({{"alg", "hs256"}}, ok_claims, "k")));
  CHECK(fails(MakeToken({{"alg", "HS512"}}, ok_claims, "k")));
  CHECK(fails(MakeToken({{"typ", "JWT"}}, ok_claims, "k")));
  CHECK(fails(MakeToken(json::array(), ok_claims, "k")));
  CHECK(!fails(MakeToken({{"alg", "HS256"}}, ok_claims, "k")));
  // Unsigned "none" token with empty signature.
  CHECK(fails(crypto::Base64UrlEncode(R"({"alg":"none"})") + "." +
              crypto::Base64UrlEncode(ok_claims.dump()) + "."));

  // Payload shape and claims.
  CHECK(fails(MakeToken({{"alg", "HS256"}}, json::array(), "k")));
  CHECK(fails(auth::SignJwtHs256({{"exp", kNow + 60}}, "k")));              // no sub
  CHECK(fails(auth::SignJwtHs256({{"sub", ""}}, "k")));                     // empty sub
  CHECK(fails(auth::SignJwtHs256({{"sub", 5}}, "k")));                      // non-string sub
  CHECK(fails(auth::SignJwtHs256({{"sub", "u"}, {"exp", "soon"}}, "k")));   // non-numeric exp
  CHECK(fails(auth::SignJwtHs256({{"sub", "u"}, {"nbf", true}}, "k")));
  CHECK(fails(auth::SignJwtHs256({{"sub", "u"}, {"iat", "x"}}, "k")));

  // Time windows with the default 30 s leeway.
  CHECK(fails(auth::SignJwtHs256({{"sub", "u"}, {"exp", kNow - 31}}, "k")));
  CHECK(!fails(auth::SignJwtHs256({{"sub", "u"}, {"exp", kNow - 29}}, "k")));
  CHECK(fails(auth::SignJwtHs256({{"sub", "u"}, {"nbf", kNow + 31}}, "k")));
  CHECK(!fails(auth::SignJwtHs256({{"sub", "u"}, {"nbf", kNow + 29}}, "k")));
  auto expired = auth::VerifyJwtHs256(
      auth::SignJwtHs256({{"sub", "u"}, {"exp", kNow - 100}}, "k"), "k", kNow);
  CHECK(!expired && expired.error() == "token expired");
  auto zero_leeway = auth::VerifyJwtHs256(
      auth::SignJwtHs256({{"sub", "u"}, {"exp", kNow - 1}}, "k"), "k", kNow, 0);
  CHECK(!zero_leeway.has_value());
  // Extreme exp values do not overflow.
  CHECK(!fails(auth::SignJwtHs256(
      {{"sub", "u"}, {"exp", std::numeric_limits<int64_t>::max()}}, "k")));
  CHECK(fails(auth::SignJwtHs256(
      {{"sub", "u"}, {"exp", std::numeric_limits<uint64_t>::max()}}, "k")));
}
