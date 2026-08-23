#include "cluster/upstream_transport.h"
#include "common/timestamp.h"
#include "network/ws/wire_protocol.h"

void UnavailableUpstreamTransport::Send(
    const std::string &node_id, const json &envelope,
    std::function<void(json response)> on_response) {
  std::string request_id;
  try {
    request_id = envelope.at("meta").at("request_id").get<std::string>();
  } catch (const json::exception &) {
    // Envelope was already validated by the caller; this is defensive.
  }
  on_response(json(ErrorResponse{
      .meta =
          AckMetaGeneric{
              .type = "ACK",
              .request_id = request_id,
              .timestamp = current_iso8601_timestamp(),
              .status = 503,
          },
      .payload = ErrorPayload{
          .code = "UPSTREAM_UNAVAILABLE",
          .message = "No connection to upstream node '" + node_id +
                     "' - the outbound Pod-to-Pod transport is not "
                     "implemented yet.",
      }}));
}
