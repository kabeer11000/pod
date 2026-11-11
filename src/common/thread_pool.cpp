#include "common/thread_pool.h"
#include "common/log.h"
#include "common/task_queue.h"
#include <exception>
#include <vector>

ThreadPool::ThreadPool(TaskQueue &tq, unsigned int thread_count) : _tq(tq) {
  // std::thread::hardware_concurrency() may legitimately return 0.
  if (thread_count == 0)
    thread_count = 1;
  _threads.reserve(thread_count);
  for (unsigned int i = 0; i < thread_count; i += 1)
    _threads.emplace_back([this] { Run(); });
}

ThreadPool::~ThreadPool() { Shutdown(); }

void ThreadPool::Shutdown() {
  _tq.close();
  for (auto &t : _threads)
    if (t.joinable())
      t.join();
  _threads.clear();
}

void ThreadPool::Run() {
  Task t;
  while (_tq.pop(t)) {
    // A throwing task must never take down the worker (std::terminate).
    // Handlers are expected to catch their own errors; this is a last resort.
    try {
      t.closure();
    } catch (const std::exception &e) {
      Log(LogLevel::Error, "ThreadPool",
          "Task (" + t.id + ") threw: " + std::string(e.what()));
    } catch (...) {
      Log(LogLevel::Error, "ThreadPool",
          "Task (" + t.id + ") threw an unknown exception");
    }
  }
}
