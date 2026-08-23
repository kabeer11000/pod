#include "indexing/codec.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace indexing {

namespace {

constexpr char kNullTag = 0x00;
constexpr char kFalseTag = 0x01;
constexpr char kTrueTag = 0x02;
constexpr char kNumberTag = 0x03;
constexpr char kStringTag = 0x04;

// Order-preserving 8-byte encoding of a double: flip the sign bit for
// non-negative numbers, invert all bits for negative numbers. Plain
// big-endian byte comparison then matches numeric comparison across the
// full range, including +/-Infinity. NaN has no total order and is rejected
// outright rather than silently sorting somewhere arbitrary.
void EncodeOrderedDouble(double v, std::string &out) {
  if (std::isnan(v))
    throw std::invalid_argument(
        "indexing: cannot index NaN - it has no total order");
  if (v == 0.0)
    v = 0.0; // canonicalize -0.0 -> +0.0 so they encode identically

  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(v));
  std::memcpy(&bits, &v, sizeof(v));

  if (bits & (uint64_t(1) << 63))
    bits = ~bits; // negative: invert every bit
  else
    bits |= (uint64_t(1) << 63); // non-negative: flip just the sign bit

  for (int i = 7; i >= 0; --i)
    out.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
}

// Escapes embedded 0x00 bytes as [0x00 0xFF] and appends a [0x00 0x00]
// terminator, so this segment's end is unambiguous when concatenated with
// whatever comes next in a compound key.
void EncodeEscapedString(const std::string &s, std::string &out) {
  for (unsigned char c : s) {
    if (c == 0x00) {
      out.push_back(static_cast<char>(0x00));
      out.push_back(static_cast<char>(0xFF));
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back(static_cast<char>(0x00));
  out.push_back(static_cast<char>(0x00));
}

} // namespace

json ResolveFieldPath(const json &document, const std::string &field_path) {
  const json *cursor = &document;
  size_t start = 0;
  while (true) {
    size_t dot = field_path.find('.', start);
    std::string segment = field_path.substr(
        start, dot == std::string::npos ? std::string::npos : dot - start);
    if (!cursor->is_object() || !cursor->contains(segment))
      return nullptr;
    cursor = &(*cursor)[segment];
    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }
  return *cursor;
}

void EncodeFieldValue(const json &value, OrderByDirection direction,
                      std::string &out) {
  std::string segment;

  if (value.is_null()) {
    segment.push_back(kNullTag);
  } else if (value.is_boolean()) {
    segment.push_back(value.get<bool>() ? kTrueTag : kFalseTag);
  } else if (value.is_number()) {
    segment.push_back(kNumberTag);
    EncodeOrderedDouble(value.get<double>(), segment);
  } else if (value.is_string()) {
    segment.push_back(kStringTag);
    EncodeEscapedString(value.get<std::string>(), segment);
  } else {
    // Array/object: not an indexable scalar in v1 - fall back to its JSON
    // form so backfill doesn't throw, but no ARRAY.* operator can use this.
    segment.push_back(kStringTag);
    EncodeEscapedString(value.dump(), segment);
  }

  if (direction == OrderByDirection::Descending)
    for (char &c : segment)
      c = static_cast<char>(~static_cast<unsigned char>(c));

  out += segment;
}

std::string IndexEntryPrefix(const std::string &index_id) {
  return "idx:" + index_id + ":";
}

std::string EncodeIndexKey(const std::string &index_id,
                           const std::vector<IndexFieldSpec> &fields,
                           const json &document,
                           const std::string &doc_path) {
  std::string key = IndexEntryPrefix(index_id);
  for (const auto &f : fields)
    EncodeFieldValue(ResolveFieldPath(document, f.field), f.direction, key);
  key.push_back('\0');
  key += doc_path;
  return key;
}

} // namespace indexing
