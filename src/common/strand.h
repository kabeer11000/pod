// A Strand serialises closures posted to it while still executing them on the
// shared ThreadPool. Each WebSocket connection owns one strand, which gives us:
//   * Per-connection FIFO ordering (a WRITE followed by a READ on the same
//     socket is always processed in that order, SUBSCRIBE before UNSUBSCRIBE).
//   * Parallelism across connections (different strands run concurrently).
#pragma once
#include "common/task_queue.h"
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class Strand : public std::enable_shared_from_this<Strand> {
public:
  Strand(TaskQueue &tq, std::string id) : _tq(tq), _id(std::move(id)) {}
  Strand(const Strand &) = delete;
  Strand &operator=(const Strand &) = delete;

  // Enqueue a closure; it will run after every closure posted before it.
  void Post(std::function<void()> fn);

private:
  void Drain();
  void Schedule();

  TaskQueue &_tq;
  std::string _id;
  std::mutex _mu;
  std::deque<std::function<void()>> _pending;
  bool _running = false;
};
