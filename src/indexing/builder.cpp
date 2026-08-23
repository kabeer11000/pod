#include "indexing/builder.h"

IndexBuilder::IndexBuilder(IndexEngine &engine)
    : _engine(engine), _thread([this] { Run(); }) {}

IndexBuilder::~IndexBuilder() {
  {
    std::lock_guard<std::mutex> lock(_mu);
    _stop = true;
  }
  _cv.notify_all();
  if (_thread.joinable())
    _thread.join();
}

void IndexBuilder::EnqueueBuild(IndexDefinition def) {
  {
    std::lock_guard<std::mutex> lock(_mu);
    _queue.push(std::move(def));
  }
  _cv.notify_one();
}

void IndexBuilder::Run() {
  while (true) {
    IndexDefinition def;
    {
      std::unique_lock<std::mutex> lock(_mu);
      _cv.wait(lock, [&] { return _stop || !_queue.empty(); });
      if (_stop && _queue.empty())
        return;
      def = std::move(_queue.front());
      _queue.pop();
    }
    _engine.Backfill(def);
  }
}
