#include "rules/jwt.h"
#include "rules/crypto.h"
#include <cmath>
#include <exception>
#include <limits>

namespace pod::auth {

using json = nlohmann::json;

namespace {

std::optional<json> DecodeJsonPart(std::string_view part) {
  auto raw = crypto::Base64UrlDecode(part);
  if (!raw)
    return std::nullopt;
  try {
    return json::parse(*raw);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

// Reads a numeric time claim as whole seconds (fractions truncated toward
// negative infinity so a fractional exp never extends validity).
std::expected<std::optional<int64_t>, std::string>
TimeClaim(const json &claims, const char *name) {
  auto it = claims.find(name);
  if (it == claims.end())
    return std::optional<int64_t>{};
  if (it->is_number_unsigned()) {
    const uint64_t v = it->get<uint64_t>();
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return std::unexpected(std::string("claim '") + name + "' is out of range");
    return std::optional<int64_t>{static_cast<int64_t>(v)};
  }
  if (it->is_number_integer())
    return std::optional<int64_t>{it->get<int64_t>()};
  if (it->is_number_float()) {
    const double d = std::floor(it->get<double>());
    if (!std::isfinite(d) || d < -9.2e18 || d > 9.2e18)
      return std::unexpected(std::string("claim '") + name + "' is out of range");
    return std::optional<int64_t>{static_cast<int64_t>(d)};
  }
  return std::unexpected(std::string("claim '") + name + "' must be a number");
}

int64_t SatAdd(int64_t a, int64_t b) {
  if (b > 0 && a > std::numeric_limits<int64_t>::max() - b)
    return std::numeric_limits<int64_t>::max();
  if (b < 0 && a < std::numeric_limits<int64_t>::min() - b)
    return std::numeric_limits<int64_t>::min();
  return a + b;
}

} // namespace

std::expected<VerifiedToken, std::string>
VerifyJwtHs256(std::string_view token, std::string_view secret, int64_t now_s,
               int64_t leeway_s) {
  if (secret.empty())
    return std::unexpected("no token secret configured");
  const size_t d1 = token.find('.');
  if (d1 == std::string_view::npos)
    return std::unexpected("malformed token");
  const size_t d2 = token.find('.', d1 + 1);
  if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos)
    return std::unexpected("malformed token");
  const std::string_view header_b64 = token.substr(0, d1);
  const std::string_view payload_b64 = token.substr(d1 + 1, d2 - d1 - 1);
  const std::string_view sig_b64 = token.substr(d2 + 1);

  auto header = DecodeJsonPart(header_b64);
  if (!header || !header->is_object())
    return std::unexpected("malformed token header");
  auto alg = header->find("alg");
  if (alg == header->end() || !alg->is_string() || alg->get<std::string>() != "HS256")
    return std::unexpected("unsupported token algorithm");

  auto signature = crypto::Base64UrlDecode(sig_b64);
  if (!signature)
    return std::unexpected("malformed token signature");
  const auto expected = crypto::HmacSha256(secret, token.substr(0, d2));
  const std::string_view expected_view(
      reinterpret_cast<const char *>(expected.data()), expected.size());
  if (!crypto::ConstantTimeEqual(*signature, expected_view))
    return std::unexpected("invalid signature");

  auto payload = DecodeJsonPart(payload_b64);
  if (!payload || !payload->is_object())
    return std::unexpected("malformed token payload");

  auto exp = TimeClaim(*payload, "exp");
  if (!exp)
    return std::unexpected(exp.error());
  auto nbf = TimeClaim(*payload, "nbf");
  if (!nbf)
    return std::unexpected(nbf.error());
  auto iat = TimeClaim(*payload, "iat");
  if (!iat)
    return std::unexpected(iat.error());
  if (*exp && now_s > SatAdd(**exp, leeway_s))
    return std::unexpected("token expired");
  if (*nbf && SatAdd(now_s, leeway_s) < **nbf)
    return std::unexpected("token not yet valid");

  auto sub = payload->find("sub");
  if (sub == payload->end() || !sub->is_string() || sub->get<std::string>().empty())
    return std::unexpected("token has no subject");

  VerifiedToken out;
  out.uid = sub->get<std::string>();
  out.expires_at = *exp;
  out.claims = std::move(*payload);
  return out;
}

std::string SignJwtHs256(const json &claims, std::string_view secret) {
  const json header = {{"alg", "HS256"}, {"typ", "JWT"}};
  std::string signing_input = crypto::Base64UrlEncode(header.dump()) + "." +
                              crypto::Base64UrlEncode(claims.dump());
  const auto mac = crypto::HmacSha256(secret, signing_input);
  signing_input += ".";
  signing_input += crypto::Base64UrlEncode(std::string_view(
      reinterpret_cast<const char *>(mac.data()), mac.size()));
  return signing_input;
}

} // namespace pod::auth
