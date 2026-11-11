#include "common/log.h"
#include "test.h"
#include <cstring>

int main(int argc, char **argv) {
  SetMinLogLevel(LogLevel::Warn);
  const char *filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const auto &[name, fn] : podtest::Tests()) {
    if (filter && !std::strstr(name.c_str(), filter))
      continue;
    const int before = podtest::Failures();
    fn();
    ++ran;
    std::cout << (podtest::Failures() == before ? "ok   " : "FAIL ") << name
              << std::endl;
  }
  std::cout << ran << " tests, " << podtest::Checks() << " checks, "
            << podtest::Failures() << " failures" << std::endl;
  return podtest::Failures() == 0 ? 0 : 1;
}
