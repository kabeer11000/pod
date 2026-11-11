// Self-contained cryptographic primitives used for token verification.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pod::crypto {

// SHA-256 (FIPS 180-4).
std::array<uint8_t, 32> Sha256(std::string_view data);
// HMAC-SHA256 (RFC 2104); keys longer than 64 bytes are hashed first.
std::array<uint8_t, 32> HmacSha256(std::string_view key, std::string_view data);
// base64url (RFC 4648 §5) without padding.
std::string Base64UrlEncode(std::string_view bytes);
// Accepts input with or without '=' padding. Rejects characters outside the
// base64url alphabet, impossible lengths and non-zero trailing bits.
std::optional<std::string> Base64UrlDecode(std::string_view text);
// Compares without data-dependent timing (length mismatch returns early).
bool ConstantTimeEqual(std::string_view a, std::string_view b);
std::string ToHex(std::span<const uint8_t> bytes);

} // namespace pod::crypto
