// Idea is how pod/pods share the same task_queue and thread_pool
// When pods starts, it creates client connections to upstream pod databases running in `node` mode
// The handler recieves a response it queues a Task to the task_queue with a closure which
// checks if response matches an outbound/ongoing proxy request in the proxy_queue and resolves it and 
// sends a defered response over ws to the original client.
// 
// When pods recieves a request it adds a Task to parse request
// Within the closure we add code which enqueues a outbound/ongoing proxy request to 
// outbound_map<request_id, .
// 

#include <mutex>
class OutboundMap {
    private:
        std::mutex _mu;
    public:
        void 
};