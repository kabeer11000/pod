// Real-time subscriptions (SUBSCRIBE / UNSUBSCRIBE / client ACK / EVENT).
//
// A subscription targets either a document (even path) or a collection query
// (odd path, with optional where/orderBy/limit/project). Each subscription
// keeps the exact result view the client holds. Lifecycle:
//
//   SUBSCRIBE  -> ACK {status: SUBSCRIBED, resumed: false}
//              -> [EVENT RESET]   when a resume_token can't be honoured
//              -> EVENT SNAPSHOT  full initial result set (is_synced: true)
//   or, resuming from a token still covered by the commit log:
//              -> ACK {resumed: true}
//              -> EVENT ADDED/MODIFIED/REMOVED ...  (client view -> current)
//              -> EVENT SYNCED
//   commit     -> EVENT REMOVED / ADDED / MODIFIED deltas
//
// Delta indexes describe how to transform the client's array step by step,
// in the order events are delivered:
//   REMOVED  : old_index = position before removal, new_index = null
//   ADDED    : old_index = null, new_index = position after insertion
//   MODIFIED : old_index = position before, new_index = position after the
//              document is removed and re-inserted with its new data
//
// resume_token is the database commit sequence number at which the event was
// produced; commit_complete marks the last event of a batch, after which the
// client's view equals the server's view at resume_token. Registration
// happens under the database commit lock, so the snapshot and the following
// deltas never miss or duplicate a commit.
#pragma once
#include "db/database.h"
#include "db/error.h"
#include "db/path.h"
#include "db/query.h"
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pod {

enum class WireEncoding { Json, MsgPack };

// Delivers `message` to connection `conn_id`. Calls for the same connection
// must be delivered in call order.
using MessageSink = std::function<void(const std::string &conn_id,
                                       nlohmann::json message,
                                       WireEncoding encoding)>;

struct SubscribeParams {
  std::string subscription_id;
  ResourcePath path;
  QuerySpec spec;
  std::optional<std::string> resume_token;
  // Optional read filter (security rules) applied to every document.
  Visibility visible;
};

class SubscriptionManager {
public:
  SubscriptionManager(Database &db, MessageSink sink);
  SubscriptionManager(const SubscriptionManager &) = delete;
  SubscriptionManager &operator=(const SubscriptionManager &) = delete;

  // Connections must be registered before they can subscribe; removing a
  // connection drops all of its subscriptions.
  void AddConnection(const std::string &conn_id);
  void RemoveConnection(const std::string &conn_id);

  // On success sends `ack` (with payload.resumed filled in) followed by the
  // initial events, all in order. On failure sends nothing.
  std::expected<void, DbError> Subscribe(const std::string &conn_id,
                                         const std::string &ns,
                                         WireEncoding encoding,
                                         nlohmann::json ack,
                                         SubscribeParams params);
  std::expected<void, DbError> Unsubscribe(const std::string &conn_id,
                                           const std::string &subscription_id);
  // Records the client's acknowledgement of events up to `resume_token`.
  std::expected<void, DbError> Acknowledge(const std::string &conn_id,
                                           const std::string &subscription_id,
                                           const std::string &resume_token);

  // Database commit listener. Called under the database commit lock.
  void OnCommit(const CommitInfo &commit);

  size_t SubscriptionCount();

private:
  struct Subscription {
    std::string conn_id;
    std::string id;
    std::string ns;
    ResourcePath target;
    bool is_document;
    QuerySpec spec;
    WireEncoding encoding;
    Visibility visible;
    std::vector<Document> view; // sorted exactly like the client's array
    uint64_t last_acked = 0;
  };
  using SubPtr = std::shared_ptr<Subscription>;

  static std::string TargetKey(const std::string &ns, const std::string &path);
  bool Matches(const Subscription &sub, const Document &doc) const;
  // Current result view of a subscription.
  std::expected<std::vector<Document>, DbError>
  ComputeView(const Subscription &sub);
  // The view a subscription had at commit `token` (requires commit lock).
  std::expected<std::vector<Document>, DbError>
  ViewAt(const Subscription &sub, uint64_t token);
  // Diffs sub.view -> next and returns the events (in order), installing
  // `next` as the view.
  std::vector<nlohmann::json> Diff(Subscription &sub, std::vector<Document> next,
                                   uint64_t seq);
  // Sends events; the last one is flagged commit_complete.
  void Deliver(const Subscription &sub, std::vector<nlohmann::json> events);
  nlohmann::json EventEnvelope(const Subscription &sub,
                               const std::string &event_type, uint64_t seq);
  nlohmann::json WireDocument(const Subscription &sub, const Document &doc);
  void RemoveLocked(const SubPtr &sub);

  Database &_db;
  MessageSink _sink;
  std::mutex _mu;
  std::unordered_set<std::string> _connections;
  // conn_id -> subscription_id -> subscription
  std::unordered_map<std::string, std::unordered_map<std::string, SubPtr>>
      _by_conn;
  // ns \0 path -> subscriptions targeting it
  std::unordered_map<std::string, std::vector<SubPtr>> _by_target;
};

} // namespace pod
