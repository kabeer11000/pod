#include "cluster/outbound_map.h"

void OutboundMap::Track(const std::string &upstream_request_id,
                        PendingProxyRequest req) {
  std::lock_guard<std::mutex> lock(_mu);
  _pending[upstream_request_id] = std::move(req);
}

std::optional<PendingProxyRequest>
OutboundMap::Resolve(const std::string &upstream_request_id) {
  std::lock_guard<std::mutex> lock(_mu);
  auto it = _pending.find(upstream_request_id);
  if (it == _pending.end())
    return std::nullopt;
  auto req = std::move(it->second);
  _pending.erase(it);
  return req;
}

void OutboundMap::Remove(const std::string &upstream_request_id) {
  std::lock_guard<std::mutex> lock(_mu);
  _pending.erase(upstream_request_id);
}

OutboundMap &global_outbound_map() {
  static OutboundMap m;
  return m;
}
