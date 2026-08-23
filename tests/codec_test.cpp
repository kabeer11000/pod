// Minimal assert-style test harness for indexing/codec.{h,cpp} - the order-
// preserving encoding scheme underpinning every secondary index. This is the
// one piece of the indexing feature where a wrong-but-plausible-looking
// implementation fails silently (wrong/missing query results, not a crash),
// so it gets exercised in isolation before anything is built on top of it.
#include "indexing/codec.h"
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace {

int failures = 0;

void expect(bool cond, const std::string &what) {
  if (!cond) {
    std::cerr << "FAILED: " << what << "\n";
    failures++;
  } else {
    std::cout << "ok: " << what << "\n";
  }
}

std::string encode(const json &value,
                   OrderByDirection dir = OrderByDirection::Ascending) {
  std::string out;
  indexing::EncodeFieldValue(value, dir, out);
  return out;
}

} // namespace

int main() {
  // -0.0 and +0.0 must compare equal.
  expect(encode(-0.0) == encode(0.0), "-0.0 encodes identically to +0.0");

  // Ascending numeric ordering, including negatives.
  expect(encode(-5.0) < encode(-1.0), "-5 < -1");
  expect(encode(-1.0) < encode(0.0), "-1 < 0");
  expect(encode(0.0) < encode(1.5), "0 < 1.5");
  expect(encode(1.5) < encode(100.0), "1.5 < 100");
  expect(encode(-100.0) < encode(-1.5), "-100 < -1.5");

  // Infinities sort at the extremes.
  double neg_inf = -std::numeric_limits<double>::infinity();
  double pos_inf = std::numeric_limits<double>::infinity();
  expect(encode(neg_inf) < encode(-1e300), "-inf < -1e300");
  expect(encode(1e300) < encode(pos_inf), "1e300 < +inf");

  // NaN has no total order and must be rejected, not silently ordered.
  {
    bool threw = false;
    try {
      encode(std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    expect(threw, "NaN throws std::invalid_argument");
  }

  // Descending inverts the effective order.
  expect(encode(5.0, OrderByDirection::Descending) <
             encode(1.0, OrderByDirection::Descending),
         "descending: 5 sorts before 1");

  // String ordering, including prefix relationships surviving the
  // terminator (e.g. "app" must still sort before "apple").
  expect(encode(json("app")) < encode(json("apple")), "'app' < 'apple'");
  expect(encode(json("apple")) < encode(json("banana")),
         "'apple' < 'banana'");
  expect(encode(json("a")) < encode(json("b")), "'a' < 'b'");

  // Multi-field concatenation must not be ambiguous across field
  // boundaries: ("a","bc") must not collide with ("ab","c").
  {
    std::string k1, k2;
    indexing::EncodeFieldValue(json("a"), OrderByDirection::Ascending, k1);
    indexing::EncodeFieldValue(json("bc"), OrderByDirection::Ascending, k1);
    indexing::EncodeFieldValue(json("ab"), OrderByDirection::Ascending, k2);
    indexing::EncodeFieldValue(json("c"), OrderByDirection::Ascending, k2);
    expect(k1 != k2, "('a','bc') != ('ab','c') compound encoding");
  }

  // Cross-type ordering: null < false < true < number < string.
  {
    std::string n = encode(json(nullptr));
    std::string f = encode(json(false));
    std::string t = encode(json(true));
    std::string num = encode(json(1.0));
    std::string s = encode(json("x"));
    expect(n < f && f < t && t < num && num < s,
           "type ordering: null < false < true < number < string");
  }

  // An embedded NUL byte in a string must be escaped, not collide with a
  // shorter string's terminator.
  expect(encode(json(std::string("a\0b", 3))) != encode(json("a")),
         "embedded NUL string differs from truncated string");
  expect(encode(json(std::string("a\0", 2))) < encode(json("ab")),
         "'a\\0' sorts before 'ab' (terminator byte < 'b')");

  // Integers and their double equivalent must encode identically (both are
  // promoted to double before encoding).
  expect(encode(json(5)) == encode(json(5.0)),
         "integer 5 encodes identically to double 5.0");

  std::cout << (failures == 0 ? "ALL PASSED\n"
                              : (std::to_string(failures) + " FAILURES\n"));
  return failures == 0 ? 0 : 1;
}
