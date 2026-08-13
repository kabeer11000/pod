// Main task_queue types and interfaces
// This creates two task queues ideally, one for incoming events and real-time tasks and one for longer running tasks such as indexing.
// We will do this via two different thread pools
//
// Tasks define a structure which takes context data and a closure which runs when a worker executes that task
#include <condition_variable>
#include <mutex>
#include <queue>
#include "common/task_queue.h"

// Thread sleeps if mutex isn't available.
// Thread sleeps if mutex aquired and condition is not valid (array is empty).
void TaskQueue::push(Task t) {
    // Is lock currently available if so aquire
    std::unique_lock<std::mutex> lock(_mu);
    // Make the task object an rvalue such that a 0-copy
    _queue.push(std::move(t));
    // Since we're using std::unique_lock mutex is automatically released when this function's scope ends (Destructor)
    // Notify a thread sleeping on pop that new data is available
    _cv.notify_one();
}
void TaskQueue::pop(Task& t) {
    // Is lock currently available if so aquire
    std::unique_lock<std::mutex> lock(_mu);
    _cv.wait(lock, [&] {
        return !_queue.empty();
    });
    t = std::move(_queue.front());
    _queue.pop(); // Pops front value
    // Since we're using std::unique_lock mutex is automatically released when this function's scope ends (Destructor)
}
