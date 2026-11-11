#pragma once
#include "rules/jwt.h"
#include <cstdint>
#include <optional>
#include <string>

namespace pod {

// Who is making requests on a connection, fixed at connect time.
struct AuthContext {
  // The namespace / tenant the connection is scoped to.
  std::string ns;
  // Verified token; nullopt = anonymous.
  std::optional<auth::VerifiedToken> token;

  bool Expired(int64_t now_s) const {
    return token && token->expires_at && now_s > *token->expires_at;
  }
};

} // namespace pod
