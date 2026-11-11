#include "common/strand.h"
#include "common/log.h"
#include <chrono>
#include <exception>

// Maximum closures executed per pool task before yielding the worker back to
// the pool, so a single chatty connection cannot starve the others.
static constexpr int kMaxBatch = 32;

void Strand::Post(std::function<void()> fn) {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(_mu);
    _pending.push_back(std::move(fn));
    if (!_running) {
      _running = true;
      schedule = true;
    }
  }
  if (schedule)
    Schedule();
}

void Strand::Schedule() {
  _tq.push(Task{
      .closure = [self = shared_from_this()] { self->Drain(); },
      .id = _id,
      .enqueued_at = std::chrono::steady_clock::now(),
  });
}

void Strand::Drain() {
  for (int executed = 0; executed < kMaxBatch; ++executed) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(_mu);
      if (_pending.empty()) {
        _running = false;
        return;
      }
      fn = std::move(_pending.front());
      _pending.pop_front();
    }
    try {
      fn();
    } catch (const std::exception &e) {
      Log(LogLevel::Error, "Strand",
          "Closure on (" + _id + ") threw: " + std::string(e.what()));
    } catch (...) {
      Log(LogLevel::Error, "Strand",
          "Closure on (" + _id + ") threw an unknown exception");
    }
  }
  // Batch exhausted: keep _running = true and requeue so ordering is kept.
  {
    std::lock_guard<std::mutex> lock(_mu);
    if (_pending.empty()) {
      _running = false;
      return;
    }
  }
  Schedule();
}
