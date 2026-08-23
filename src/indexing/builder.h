#pragma once
// Runs index backfills on a single dedicated background thread, deliberately
// kept separate from the request-serving ThreadPool so a large collection
// scan can never starve live query/write latency (the shared task_queue.h
// header already anticipates exactly this split: "one for incoming events
// and real-time tasks and one for longer running tasks such as indexing").
#include "indexing/engine.h"
#include "indexing/interface.h"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

class IndexBuilder {
public:
  explicit IndexBuilder(IndexEngine &engine);
  ~IndexBuilder();

  IndexBuilder(const IndexBuilder &) = delete;
  IndexBuilder &operator=(const IndexBuilder &) = delete;

  // Enqueues a backfill job and returns immediately; jobs run one at a time,
  // in submission order, on the dedicated worker thread.
  void EnqueueBuild(IndexDefinition def);

private:
  void Run();

  IndexEngine &_engine;
  std::mutex _mu;
  std::condition_variable _cv;
  std::queue<IndexDefinition> _queue;
  bool _stop = false;
  std::thread _thread;
};
