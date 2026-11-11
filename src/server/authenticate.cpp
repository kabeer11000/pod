#include "server/authenticate.h"
#include "common/timestamp.h"
#include "db/path.h"
#include "rules/jwt.h"

namespace pod {

std::expected<AuthContext, AuthFailure>
AuthenticateConnection(const PodConfig &config, std::string_view ns_param,
                       std::string_view token) {
  const std::string candidate = ns_param.empty() ? config.default_namespace
                                                 : std::string(ns_param);
  auto ns = ValidateNamespace(candidate);
  if (!ns)
    return std::unexpected(AuthFailure{4400, ns.error().message});

  AuthContext ctx;
  ctx.ns = *ns;
  if (token.empty()) {
    if (config.require_auth)
      return std::unexpected(
          AuthFailure{4401, "This server requires a token."});
    return ctx;
  }
  if (config.auth_secret.empty())
    return std::unexpected(AuthFailure{4401, "Token authentication is not configured on this server."});

  auto verified = auth::VerifyJwtHs256(token, config.auth_secret,
                                       current_unix_millis() / 1000);
  if (!verified)
    return std::unexpected(
        AuthFailure{4401, "Invalid token: " + verified.error()});

  const auto &claims = verified->claims;
  if (claims.contains("ns") && !claims["ns"].is_null()) {
    const auto &allowed = claims["ns"];
    bool ok = false;
    if (allowed.is_string()) {
      ok = allowed.get<std::string>() == ctx.ns;
    } else if (allowed.is_array()) {
      for (const auto &a : allowed)
        if (a.is_string() && a.get<std::string>() == ctx.ns)
          ok = true;
    } else {
      return std::unexpected(
          AuthFailure{4401, "Invalid token: malformed 'ns' claim."});
    }
    if (!ok)
      return std::unexpected(AuthFailure{4403, "Token is not valid for namespace '" + ctx.ns + "'."});
  }
  ctx.token = std::move(*verified);
  return ctx;
}

} // namespace pod
