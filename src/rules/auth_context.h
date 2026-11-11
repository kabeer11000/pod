#pragma once
#include <string>

struct AuthContext {
  // The namespace / tenant the request is scoped to. (`namespace` is a C++
  // keyword, hence `ns`.)
  std::string ns;
};
