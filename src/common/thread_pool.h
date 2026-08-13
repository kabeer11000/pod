#pragma once
#include "common/task_queue.h"
#include <thread>
#include <vector>

class ThreadPool {
private:
  TaskQueue &_tq;
  std::vector<std::thread> _threads;

public:
  ThreadPool(TaskQueue &tq, int thread_count);
  void Run();
};
