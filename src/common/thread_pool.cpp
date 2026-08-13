#include "common/thread_pool.h"
#include "common/task_queue.h"
#include <iostream>
#include <vector>

ThreadPool::ThreadPool(TaskQueue &tq, int thread_count) : _tq(tq) {
  for (int i = 0; i <= thread_count; i += 1)
    _threads.emplace_back([&] { Run(); });
};
void ThreadPool::Run() {
  Task t;
  while (_tq.pop(t)) {
    std::cout << "[log][thread_pool]: Processing Task (" << t.id << ")";
    t.closure();
  }
};
