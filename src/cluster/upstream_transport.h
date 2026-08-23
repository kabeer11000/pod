#pragma once
// Seam between the router's routing/bookkeeping logic and the actual
// network call to an upstream Pod node. entry_pods's dispatch closure only
// depends on this interface, so plugging in a real client later touches
// nothing else in the routing path.
//
// NOTE: there is no working outbound transport yet. uWS's own WS client
// (uwebsockets/ClientApp.h, in the vcpkg'd 20.78.0 build used here) is an
// unimplemented stub - its constructor, connect(), and run() are all empty
// bodies - and no other WS/RPC client dependency is wired into this project.
// UnavailableUpstreamTransport below is the honest placeholder until a real
// one is built (likely: a small hand-rolled WS client over raw sockets, or
// swapping in a client library once one is added as a dependency).
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

class UpstreamTransport {
public:
  virtual ~UpstreamTransport() = default;
  // Sends `envelope` to the Pod node identified by `node_id`; `on_response`
  // is invoked exactly once with the reply envelope (or a synthesized ERROR
  // envelope on failure/unavailability), from whatever thread the reply
  // arrives on.
  virtual void Send(const std::string &node_id, const json &envelope,
                    std::function<void(json response)> on_response) = 0;
};

class UnavailableUpstreamTransport : public UpstreamTransport {
public:
  void Send(const std::string &node_id, const json &envelope,
           std::function<void(json response)> on_response) override;
};
