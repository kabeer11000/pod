// Main task_queue types and interfaces
// This creates two task queues ideally, one for incoming events and real-time tasks and one for longer running tasks such as indexing.
// We will do this via two different thread pools
//
// Tasks define a structure which takes context data and a closure which runs when a worker executes that task
#pragma once
#include <condition_variable>
#include <mutex>
#include <queue>
#include "common/task.h"
// Thread sleeps if mutex isn't available.
// Thread sleeps if mutex aquired and condition is not valid (array is empty).
class TaskQueue {
    private:
        std::queue<Task> _queue;
        // CAS + Sleep Lock fallback / Mutex for the Global TaskQueue
        std::mutex _mu;
        // Conditional variable which invokes a futex / sleeps on a channel for condition
        // If its ever valid another thread calls .notify / .notify_one
        // which wakes up some other thread sleeping on same channel / futex.
        //
        // Conditional allows for checking when condition is active.
        // It lets go of the mutex and sleeps. When woken up
        // it reaquires the mutex (which can also cause sleep-behaviour if its locked)
        std::condition_variable _cv;
        // Once closed, push() is a no-op and pop() drains the remaining tasks
        // and then returns false so worker threads can exit.
        bool _closed = false;
    public:
        TaskQueue() = default;
        void push(Task t);
        // Blocks until a task is available. Returns false once the queue has
        // been closed and fully drained.
        bool pop(Task& t);
        // Wakes every waiting worker; used for graceful shutdown.
        void close();
};
