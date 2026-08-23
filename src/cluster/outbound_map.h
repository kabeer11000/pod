#pragma once
// Tracks requests the router (`pods`) has forwarded upstream and is still
// waiting on an ack for.
//
// When entry_pods proxies a client's request to whichever node ShardRing
// says owns it, it does so under a NEWLY generated request_id rather than
// the client's own - otherwise two different original clients whose
// request_ids happen to collide would corrupt each other's in-flight state
// upstream. This map is what lets the eventual upstream reply - arriving on
// some outbound connection's own message handler, identifiable only by
// that new request_id - find its way back to the original caller: rewrite
// the reply's request_id back to the client's own, and invoke the
// ResponseCallback that was waiting on it.
#include "network/ws/server.h" // ResponseCallback
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

struct PendingProxyRequest {
  std::string original_request_id;
  ResponseCallback respond;
  std::chrono::steady_clock::time_point enqueued_at;
};

class OutboundMap {
public:
  void Track(const std::string &upstream_request_id, PendingProxyRequest req);
  // Removes and returns the pending entry for `upstream_request_id`, or
  // nullopt if it was already resolved (or never existed) - callers should
  // treat a second resolution of the same id as a no-op, not an error.
  std::optional<PendingProxyRequest>
  Resolve(const std::string &upstream_request_id);
  void Remove(const std::string &upstream_request_id);

private:
  std::mutex _mu;
  std::unordered_map<std::string, PendingProxyRequest> _pending;
};

OutboundMap &global_outbound_map();
