#pragma once
#include "common/task_queue.h"
#include <thread>
#include <vector>

class ThreadPool {
private:
  TaskQueue &_tq;
  std::vector<std::thread> _threads;
  void Run();

public:
  ThreadPool(TaskQueue &tq, unsigned int thread_count);
  // Closes the queue, lets workers drain remaining tasks and joins them.
  ~ThreadPool();
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  void Shutdown();
};
