// HS256 JSON Web Tokens (RFC 7519 / RFC 7515 compact serialization).
#pragma once
#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace pod::auth {

struct VerifiedToken {
  std::string uid;                   // "sub" claim (required, non-empty)
  nlohmann::json claims;             // full payload object
  std::optional<int64_t> expires_at; // "exp" seconds since epoch, if present
};

// Verifies a compact JWS HS256 token. now_s = current unix seconds.
std::expected<VerifiedToken, std::string>
VerifyJwtHs256(std::string_view token, std::string_view secret, int64_t now_s,
               int64_t leeway_s = 30);

// Signs `claims` with header {"alg":"HS256","typ":"JWT"}.
std::string SignJwtHs256(const nlohmann::json &claims, std::string_view secret);

} // namespace pod::auth
