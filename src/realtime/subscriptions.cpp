#include "realtime/subscriptions.h"
#include "common/log.h"
#include "common/timestamp.h"
#include "db/field_path.h"
#include <algorithm>
#include <charconv>

namespace pod {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxSubscriptionIdBytes = 256;

std::expected<uint64_t, DbError> ParseResumeToken(const std::string &token) {
  uint64_t value = 0;
  const char *begin = token.data();
  const char *end = token.data() + token.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (token.empty() || ec != std::errc() || ptr != end)
    return std::unexpected(
        DbError::InvalidArgument("resume_token is not a valid token."));
  return value;
}

bool SameRevision(const Document &a, const Document &b) {
  return a.version == b.version && a.update_time_ms == b.update_time_ms &&
         a.create_time_ms == b.create_time_ms;
}

} // namespace

SubscriptionManager::SubscriptionManager(Database &db, MessageSink sink)
    : _db(db), _sink(std::move(sink)) {}

std::string SubscriptionManager::TargetKey(const std::string &ns,
                                           const std::string &path) {
  std::string k;
  k.reserve(ns.size() + path.size() + 1);
  k += ns;
  k += '\0';
  k += path;
  return k;
}

void SubscriptionManager::AddConnection(const std::string &conn_id) {
  std::lock_guard<std::mutex> lock(_mu);
  _connections.insert(conn_id);
}

void SubscriptionManager::RemoveConnection(const std::string &conn_id) {
  std::lock_guard<std::mutex> lock(_mu);
  _connections.erase(conn_id);
  auto it = _by_conn.find(conn_id);
  if (it == _by_conn.end())
    return;
  std::vector<SubPtr> subs;
  for (auto &[_, sub] : it->second)
    subs.push_back(sub);
  for (auto &sub : subs)
    RemoveLocked(sub);
}

void SubscriptionManager::RemoveLocked(const SubPtr &sub) {
  auto key = TargetKey(sub->ns, sub->target.ToString());
  if (auto t = _by_target.find(key); t != _by_target.end()) {
    std::erase(t->second, sub);
    if (t->second.empty())
      _by_target.erase(t);
  }
  if (auto c = _by_conn.find(sub->conn_id); c != _by_conn.end()) {
    c->second.erase(sub->id);
    if (c->second.empty())
      _by_conn.erase(c);
  }
}

size_t SubscriptionManager::SubscriptionCount() {
  std::lock_guard<std::mutex> lock(_mu);
  size_t n = 0;
  for (auto &[_, subs] : _by_conn)
    n += subs.size();
  return n;
}

bool SubscriptionManager::Matches(const Subscription &sub,
                                  const Document &doc) const {
  if (!sub.is_document && !MatchesQuery(doc, sub.spec))
    return false;
  return !sub.visible || sub.visible(doc);
}

std::expected<std::vector<Document>, DbError>
SubscriptionManager::ComputeView(const Subscription &sub) {
  if (sub.is_document) {
    auto doc = _db.Read(sub.ns, sub.target);
    if (!doc)
      return std::unexpected(doc.error());
    std::vector<Document> v;
    if (*doc && Matches(sub, **doc))
      v.push_back(std::move(**doc));
    return v;
  }
  const Visibility *vis = sub.visible ? &sub.visible : nullptr;
  if (sub.spec.limit) {
    // Uses index-ordered early termination when an index fits.
    auto page = _db.Query(sub.ns, sub.target, sub.spec, std::nullopt, vis);
    if (!page)
      return std::unexpected(page.error());
    return std::move(page->documents);
  }
  return _db.QueryAll(sub.ns, sub.target, sub.spec, vis);
}

std::expected<std::vector<Document>, DbError>
SubscriptionManager::ViewAt(const Subscription &sub, uint64_t token) {
  auto state = _db.StateAt(sub.ns, sub.target, token);
  if (!state)
    return state;
  std::vector<Document> view;
  for (auto &d : *state)
    if (Matches(sub, d))
      view.push_back(std::move(d));
  SortDocuments(view, sub.spec);
  if (sub.spec.limit && view.size() > *sub.spec.limit)
    view.resize(*sub.spec.limit);
  return view;
}

json SubscriptionManager::WireDocument(const Subscription &sub,
                                       const Document &doc) {
  return json{{"path", doc.path},
              {"data", Project(doc.data, sub.spec.project)},
              {"_v", doc.version}};
}

json SubscriptionManager::EventEnvelope(const Subscription &sub,
                                        const std::string &event_type,
                                        uint64_t seq) {
  return json{{"meta",
               {{"type", "EVENT"},
                {"subscription_id", sub.id},
                {"timestamp", current_iso8601_timestamp()}}},
              {"payload",
               {{"event_type", event_type},
                {"resume_token", std::to_string(seq)},
                {"commit_complete", false}}}};
}

void SubscriptionManager::Deliver(const Subscription &sub,
                                  std::vector<json> events) {
  if (events.empty())
    return;
  events.back()["payload"]["commit_complete"] = true;
  for (auto &e : events)
    _sink(sub.conn_id, std::move(e), sub.encoding);
}

std::expected<void, DbError>
SubscriptionManager::Subscribe(const std::string &conn_id,
                               const std::string &ns, WireEncoding encoding,
                               json ack, SubscribeParams params) {
  if (params.subscription_id.empty() ||
      params.subscription_id.size() > kMaxSubscriptionIdBytes ||
      !IsValidUtf8(params.subscription_id))
    return std::unexpected(DbError::InvalidArgument(
        "subscription_id must be a non-empty UTF-8 string of at most " +
        std::to_string(kMaxSubscriptionIdBytes) + " bytes."));
  const bool is_document = params.path.IsDocument();
  if (is_document &&
      (!params.spec.where.empty() || !params.spec.order_by.empty() ||
       params.spec.limit))
    return std::unexpected(DbError::InvalidArgument(
        "where, orderBy and limit are only valid on collection "
        "subscriptions."));
  std::optional<uint64_t> resume_from;
  if (params.resume_token) {
    auto t = ParseResumeToken(*params.resume_token);
    if (!t)
      return std::unexpected(t.error());
    resume_from = *t;
  }

  auto sub = std::make_shared<Subscription>(Subscription{
      .conn_id = conn_id,
      .id = params.subscription_id,
      .ns = ns,
      .target = std::move(params.path),
      .is_document = is_document,
      .spec = std::move(params.spec),
      .encoding = encoding,
      .visible = std::move(params.visible),
      .view = {},
      .last_acked = 0,
  });

  // Holding the commit lock means no commit can land between building the
  // views and registering for deltas.
  return _db.WithCommitLock([&](uint64_t seq) -> std::expected<void, DbError> {
    {
      std::lock_guard<std::mutex> lock(_mu);
      if (!_connections.contains(conn_id))
        return {}; // connection closed meanwhile; nothing to deliver to
      if (auto c = _by_conn.find(conn_id);
          c != _by_conn.end() && c->second.contains(sub->id))
        return std::unexpected(DbError::AlreadyExists(
            "Subscription '" + sub->id +
            "' already exists on this connection."));
    }
    auto current = ComputeView(*sub);
    if (!current)
      return std::unexpected(current.error());

    const bool resumed = resume_from && _db.CanResumeFrom(*resume_from);
    std::vector<Document> client_view;
    if (resumed) {
      auto at = ViewAt(*sub, *resume_from);
      if (!at)
        return std::unexpected(at.error());
      client_view = std::move(*at);
    }
    sub->last_acked = resume_from.value_or(0);

    std::lock_guard<std::mutex> lock(_mu);
    if (!_connections.contains(conn_id))
      return {};
    _by_conn[conn_id][sub->id] = sub;
    _by_target[TargetKey(ns, sub->target.ToString())].push_back(sub);

    ack["payload"]["resumed"] = resumed;
    _sink(conn_id, std::move(ack), encoding);

    if (resumed) {
      // Bring the client's view (as of its token) up to date with deltas.
      sub->view = std::move(client_view);
      std::vector<json> events = Diff(*sub, std::move(*current), seq);
      events.push_back(EventEnvelope(*sub, "SYNCED", seq));
      Deliver(*sub, std::move(events));
      return {};
    }

    sub->view = std::move(*current);
    std::vector<json> events;
    if (resume_from) {
      // The commit log no longer covers the token: start over.
      events.push_back(EventEnvelope(*sub, "RESET", seq));
    }
    json snapshot = EventEnvelope(*sub, "SNAPSHOT", seq);
    json docs = json::array();
    for (size_t i = 0; i < sub->view.size(); ++i) {
      json d = WireDocument(*sub, sub->view[i]);
      d["index"] = i;
      docs.push_back(std::move(d));
    }
    snapshot["payload"]["snapshot"] =
        json{{"is_synced", true}, {"documents", std::move(docs)}};
    events.push_back(std::move(snapshot));
    Deliver(*sub, std::move(events));
    return {};
  });
}

std::expected<void, DbError>
SubscriptionManager::Unsubscribe(const std::string &conn_id,
                                 const std::string &subscription_id) {
  std::lock_guard<std::mutex> lock(_mu);
  auto c = _by_conn.find(conn_id);
  if (c == _by_conn.end())
    return std::unexpected(DbError::NotFound(
        "Subscription '" + subscription_id + "' does not exist."));
  auto s = c->second.find(subscription_id);
  if (s == c->second.end())
    return std::unexpected(DbError::NotFound(
        "Subscription '" + subscription_id + "' does not exist."));
  SubPtr sub = s->second;
  RemoveLocked(sub);
  return {};
}

std::expected<void, DbError>
SubscriptionManager::Acknowledge(const std::string &conn_id,
                                 const std::string &subscription_id,
                                 const std::string &resume_token) {
  auto token = ParseResumeToken(resume_token);
  if (!token)
    return std::unexpected(token.error());
  std::lock_guard<std::mutex> lock(_mu);
  auto c = _by_conn.find(conn_id);
  if (c == _by_conn.end() || !c->second.contains(subscription_id))
    return std::unexpected(DbError::NotFound(
        "Subscription '" + subscription_id + "' does not exist."));
  auto &sub = c->second.at(subscription_id);
  sub->last_acked = std::max(sub->last_acked, *token);
  return {};
}

std::vector<json> SubscriptionManager::Diff(Subscription &sub,
                                            std::vector<Document> next,
                                            uint64_t seq) {
  auto less = [&](const Document &a, const Document &b) {
    return DocumentLess(a, b, sub.spec);
  };
  const std::vector<Document> old = std::move(sub.view);
  std::unordered_map<std::string, size_t> next_index, old_index;
  for (size_t i = 0; i < next.size(); ++i)
    next_index.emplace(next[i].path, i);
  for (size_t i = 0; i < old.size(); ++i)
    old_index.emplace(old[i].path, i);

  std::vector<json> events;
  auto delta_event = [&](const char *type, std::optional<size_t> from,
                         std::optional<size_t> to, const Document &doc,
                         bool with_document) {
    json e = EventEnvelope(sub, type, seq);
    json delta = json{{"path", doc.path},
                      {"old_index", from ? json(*from) : json(nullptr)},
                      {"new_index", to ? json(*to) : json(nullptr)}};
    if (with_document)
      delta["document"] = WireDocument(sub, doc);
    e["payload"]["delta"] = std::move(delta);
    events.push_back(std::move(e));
  };

  // Phase 1: removals, in old order. `cur` keeps every surviving document
  // with the data the client currently holds, so it stays sorted.
  std::vector<Document> cur;
  cur.reserve(std::max(old.size(), next.size()));
  size_t removed = 0;
  for (size_t i = 0; i < old.size(); ++i) {
    if (next_index.contains(old[i].path)) {
      cur.push_back(old[i]);
    } else {
      delta_event("REMOVED", i - removed, std::nullopt, old[i], false);
      ++removed;
    }
  }

  // Phase 2: additions and modifications, in new order. Each step keeps `cur`
  // sorted by the data held for every element, so lower_bound gives exact
  // positions; once every changed document is re-inserted cur == next.
  for (const Document &doc : next) {
    auto o = old_index.find(doc.path);
    if (o == old_index.end()) {
      auto at = std::lower_bound(cur.begin(), cur.end(), doc, less);
      const size_t to = static_cast<size_t>(at - cur.begin());
      cur.insert(at, doc);
      delta_event("ADDED", std::nullopt, to, doc, true);
      continue;
    }
    const Document &held = old[o->second];
    if (SameRevision(held, doc))
      continue;
    auto from_it = std::lower_bound(cur.begin(), cur.end(), held, less);
    if (from_it == cur.end() || from_it->path != held.path) {
      // Cannot happen while the invariant holds; fall back to a linear find.
      from_it = std::find_if(cur.begin(), cur.end(), [&](const Document &d) {
        return d.path == held.path;
      });
    }
    const size_t from = static_cast<size_t>(from_it - cur.begin());
    cur.erase(from_it);
    auto at = std::lower_bound(cur.begin(), cur.end(), doc, less);
    const size_t to = static_cast<size_t>(at - cur.begin());
    cur.insert(at, doc);
    delta_event("MODIFIED", from, to, doc, true);
  }

  sub.view = std::move(next);
  return events;
}

void SubscriptionManager::OnCommit(const CommitInfo &commit) {
  std::lock_guard<std::mutex> lock(_mu);
  if (_by_target.empty())
    return;

  // Group the commit's changes per affected subscription, preserving order.
  std::vector<std::pair<SubPtr, std::vector<const ChangeRecord *>>> work;
  std::unordered_map<Subscription *, size_t> slot;
  auto route = [&](const std::string &target, const ChangeRecord &change) {
    auto it = _by_target.find(TargetKey(commit.ns, target));
    if (it == _by_target.end())
      return;
    for (const auto &sub : it->second) {
      auto [pos, inserted] = slot.emplace(sub.get(), work.size());
      if (inserted)
        work.push_back({sub, {}});
      work[pos->second].second.push_back(&change);
    }
  };
  for (const auto &change : commit.changes) {
    route(change.collection, change);
    route(change.path, change);
  }

  for (auto &[sub, changes] : work) {
    std::vector<Document> next;
    if (sub->is_document) {
      // Only changes to this exact document are routed here.
      const ChangeRecord *last = changes.back();
      if (last->after && Matches(*sub, *last->after))
        next.push_back(*last->after);
    } else if (!sub->spec.limit) {
      auto less = [&](const Document &a, const Document &b) {
        return DocumentLess(a, b, sub->spec);
      };
      next = sub->view;
      for (const ChangeRecord *c : changes) {
        std::erase_if(next,
                      [&](const Document &d) { return d.path == c->path; });
        if (c->after && Matches(*sub, *c->after)) {
          auto at =
              std::lower_bound(next.begin(), next.end(), *c->after, less);
          next.insert(at, *c->after);
        }
      }
    } else {
      // Limited queries: documents can enter or leave the window without
      // being changed themselves, so recompute the window when relevant.
      bool relevant = false;
      for (const ChangeRecord *c : changes) {
        const bool in_view =
            std::any_of(sub->view.begin(), sub->view.end(),
                        [&](const Document &d) { return d.path == c->path; });
        if (in_view || (c->after && Matches(*sub, *c->after))) {
          relevant = true;
          break;
        }
      }
      if (!relevant)
        continue;
      auto view = ComputeView(*sub);
      if (!view) {
        Log(LogLevel::Error, "subscriptions",
            "Failed to refresh subscription '" + sub->id +
                "': " + view.error().message);
        continue;
      }
      next = std::move(*view);
    }
    Deliver(*sub, Diff(*sub, std::move(next), commit.sequence));
  }
}

} // namespace pod
