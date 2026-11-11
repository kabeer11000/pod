// Minimal self-registering test harness (no external framework).
//
//   TEST(MyTest) { CHECK(1 + 1 == 2); CHECK_EQ(a, b); }
//
// Run all tests: ./pod_tests      Run a subset: ./pod_tests <name-substring>
#pragma once
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace podtest {

inline int &Failures() {
  static int f = 0;
  return f;
}
inline int &Checks() {
  static int c = 0;
  return c;
}
inline std::vector<std::pair<std::string, std::function<void()>>> &Tests() {
  static std::vector<std::pair<std::string, std::function<void()>>> t;
  return t;
}
struct Registrar {
  Registrar(const char *name, std::function<void()> fn) {
    Tests().emplace_back(name, std::move(fn));
  }
};

} // namespace podtest

#define TEST(name)                                                             \
  static void name();                                                          \
  static podtest::Registrar name##_registrar(#name, name);                     \
  static void name()

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++podtest::Checks();                                                       \
    if (!(cond)) {                                                             \
      ++podtest::Failures();                                                   \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond        \
                << std::endl;                                                  \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    ++podtest::Checks();                                                       \
    if (!((a) == (b))) {                                                       \
      ++podtest::Failures();                                                   \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b \
                << std::endl;                                                  \
    }                                                                          \
  } while (0)
