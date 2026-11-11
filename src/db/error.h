#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace pod {

// Error returned by every database operation. `status` is an HTTP-style code
// and `code` is a stable machine-readable identifier sent on the wire.
struct DbError {
  uint32_t status;
  std::string code;
  std::string message;
  std::optional<nlohmann::json> details;

  static DbError InvalidArgument(std::string msg) {
    return {400, "INVALID_ARGUMENT", std::move(msg), std::nullopt};
  }
  static DbError InvalidPath(std::string msg) {
    return {400, "INVALID_PATH", std::move(msg), std::nullopt};
  }
  static DbError InvalidCursor(std::string msg) {
    return {400, "INVALID_CURSOR", std::move(msg), std::nullopt};
  }
  static DbError Unauthenticated(std::string msg) {
    return {401, "UNAUTHENTICATED", std::move(msg), std::nullopt};
  }
  static DbError PermissionDenied(std::string msg) {
    return {403, "PERMISSION_DENIED", std::move(msg), std::nullopt};
  }
  static DbError Unavailable(std::string msg) {
    return {503, "UNAVAILABLE", std::move(msg), std::nullopt};
  }
  static DbError NotFound(std::string msg) {
    return {404, "NOT_FOUND", std::move(msg), std::nullopt};
  }
  static DbError AlreadyExists(std::string msg) {
    return {409, "ALREADY_EXISTS", std::move(msg), std::nullopt};
  }
  static DbError TooLarge(std::string msg) {
    return {413, "DOCUMENT_TOO_LARGE", std::move(msg), std::nullopt};
  }
  static DbError Internal(std::string msg) {
    return {500, "INTERNAL", std::move(msg), std::nullopt};
  }
  static DbError DataCorruption(std::string msg) {
    return {500, "DATA_CORRUPTION", std::move(msg), std::nullopt};
  }
};

} // namespace pod
