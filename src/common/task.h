#pragma once
#include <chrono>
#include <functional>
#include "string"

struct Task {
    std::function<void()> closure;
    std::string id;
    std::chrono::steady_clock::time_point enqueued_at;
};
