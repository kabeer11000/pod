// Core engine tests: paths, keys, field paths, values, filters, database,
// queries, subscriptions, resume, LevelDB driver and wire parsing.
#include "test.h"
#include "memory_storage.h"
#include "db/database.h"
#include "db/field_path.h"
#include "db/keys.h"
#include "db/path.h"
#include "db/query.h"
#include "network/ws/wire_protocol.h"
#include "realtime/subscriptions.h"
#include "storage/driver_leveldb.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace pod;

static ResourcePath P(const std::string &s) {
  auto p = ParsePath(s);
  if (!p) {
    std::cerr << "bad test path " << s << std::endl;
    std::abort();
  }
  return *p;
}

static QuerySpec Spec(const json &payload) {
  json msg = {{"meta", {{"type", "QUERY"}, {"request_id", "r"}}},
              {"payload", payload}};
  msg["payload"]["path"] = "c";
  auto q = wire::ParseQuery(msg);
  if (!q) {
    std::cerr << "bad test spec: " << q.error().message << std::endl;
    std::abort();
  }
  return q->spec;
}

// ---------------------------------------------------------------------------
TEST(TestPaths) {
  CHECK(ParsePath("users").has_value());
  CHECK(ParsePath("/users/u1/").has_value());
  CHECK(ParsePath("users/u1")->IsDocument());
  CHECK(ParsePath("users/u1/posts")->IsCollection());
  CHECK_EQ(ParsePath("users/u1/posts/p1")->ParentCollection(), "users/u1/posts");
  CHECK(!ParsePath("").has_value());
  CHECK(!ParsePath("///").has_value());
  CHECK(!ParsePath("a//b").has_value());
  CHECK(!ParsePath("a/../b").has_value());
  CHECK(!ParsePath("a/__x__").has_value());
  CHECK(!ParsePath(std::string("a/b\0c", 5)).has_value());
  CHECK(!ParsePath("a/\xff").has_value());
  CHECK(!ParseDocumentPath("users").has_value());
  CHECK(!ParseCollectionPath("users/u1").has_value());
  CHECK(ValidateNamespace("tenant_1-a").has_value());
  CHECK(!ValidateNamespace("").has_value());
  CHECK(!ValidateNamespace("a/b").has_value());
  CHECK(!ValidateNamespace(std::string(65, 'a')).has_value());
  CHECK(IsValidUtf8("héllo ✓ 😀"));
  CHECK(!IsValidUtf8("\xc0\x80")); // overlong
  CHECK(!IsValidUtf8("\xed\xa0\x80")); // surrogate
}

TEST(TestKeys) {
  // Direct children prefix must not match sub-collection documents.
  const auto coll = keys::CollectionDocumentsPrefix("ns", P("users"));
  const auto direct = keys::DocumentKey("ns", P("users/u1"));
  const auto nested = keys::DocumentKey("ns", P("users/u1/posts/p1"));
  const auto other = keys::DocumentKey("ns", P("usersX/u1"));
  CHECK(direct.starts_with(coll));
  CHECK(!nested.starts_with(coll));
  CHECK(!other.starts_with(coll));
  CHECK(nested.starts_with(keys::DescendantsPrefix("ns", P("users"))));
  CHECK(nested.starts_with(keys::DescendantsPrefix("ns", P("users/u1"))));
  CHECK(!nested.starts_with(keys::DescendantsPrefix("ns", P("users/u2"))));
  CHECK(!other.starts_with(keys::DescendantsPrefix("ns", P("users"))));
  // Namespaces never share prefixes: "a" vs "ab".
  CHECK(!keys::DocumentKey("ab", P("users/u1"))
             .starts_with(keys::NamespacePrefix("a")));
  CHECK_EQ(*keys::DocumentPathFromKey("ns", nested), "users/u1/posts/p1");
}

TEST(TestFieldPaths) {
  json d = {{"a", {{"b", 1}, {"c", 2}}}, {"x", 5}};
  CHECK_EQ(*GetField(d, *ParseFieldPath("a.b")), json(1));
  CHECK(GetField(d, *ParseFieldPath("a.z")) == nullptr);
  CHECK(GetField(d, *ParseFieldPath("x.y")) == nullptr);
  CHECK(!ParseFieldPath("a..b").has_value());
  CHECK(!ParseFieldPath(".a").has_value());
  CHECK(!ParseFieldPath("").has_value());

  json s = d;
  SetField(s, *ParseFieldPath("x.y.z"), 7); // replaces non-object 5
  CHECK_EQ(s["x"], json({{"y", {{"z", 7}}}}));
  CHECK(RemoveField(s, *ParseFieldPath("a.b")));
  CHECK(!RemoveField(s, *ParseFieldPath("a.b")));
  CHECK_EQ(s["a"], json({{"c", 2}}));

  CHECK_EQ(Project(d, {*ParseFieldPath("a.c"), *ParseFieldPath("missing")}),
           json({{"a", {{"c", 2}}}}));
  CHECK_EQ(Project(d, {*ParseFieldPath("a"), *ParseFieldPath("a.b")}),
           json({{"a", {{"b", 1}, {"c", 2}}}}));
  CHECK_EQ(Project(d, {}), d);

  json m = {{"a", {{"b", 1}}}, {"arr", {1, 2}}};
  DeepMerge(m, {{"a", {{"c", 3}}}, {"arr", {9}}, {"n", nullptr}});
  CHECK_EQ(m, json({{"a", {{"b", 1}, {"c", 3}}}, {"arr", {9}}, {"n", nullptr}}));

  CHECK(FieldPathsOverlap(*ParseFieldPath("a"), *ParseFieldPath("a.b")));
  CHECK(!FieldPathsOverlap(*ParseFieldPath("a.b"), *ParseFieldPath("a.c")));
}

TEST(TestValueOrdering) {
  CHECK(CompareValues(nullptr, false) < 0);
  CHECK(CompareValues(true, 0) < 0);
  CHECK(CompareValues(1000, "a") < 0);
  CHECK(CompareValues("z", json::array()) < 0);
  CHECK(CompareValues(json::array(), json::object()) < 0);
  CHECK_EQ(CompareValues(1, 1.0), 0);
  CHECK(CompareValues(1, 1.5) < 0);
  CHECK(CompareValues(-2, -2.5) > 0);
  CHECK(CompareValues(json(std::numeric_limits<uint64_t>::max()),
                      json(std::numeric_limits<int64_t>::max())) > 0);
  CHECK(CompareValues(json(std::numeric_limits<int64_t>::min()), -1e30) > 0);
  CHECK(CompareValues(json(9007199254740993ULL), 9007199254740992.0) > 0);
  CHECK(CompareValues(std::nan(""), -1e308) < 0);
  CHECK_EQ(CompareValues(std::nan(""), std::nan("")), 0);
  CHECK(CompareValues("B", "a") < 0); // byte order
  CHECK(CompareValues(json({1, 2}), json({1, 2, 0})) < 0);
  CHECK(CompareValues(json({{"a", 1}}), json({{"a", 2}})) < 0);
}

TEST(TestFilters) {
  const json d = {{"n", 5},     {"s", "m"},
                  {"tags", {"x", "y"}}, {"nested", {{"v", true}}},
                  {"nul", nullptr}};
  auto f = [](const char *field, const char *op, json v) {
    auto r = MakeFilter(field, op, v);
    if (!r) {
      std::cerr << "bad filter " << r.error().message << std::endl;
      std::abort();
    }
    return *r;
  };
  CHECK(MatchesFilter(d, f("n", "EQUAL", 5.0)));
  CHECK(MatchesFilter(d, f("n", "GREATER", 4)));
  CHECK(!MatchesFilter(d, f("n", "GREATER", 5)));
  CHECK(MatchesFilter(d, f("n", "GREATER_EQUAL", 5)));
  CHECK(MatchesFilter(d, f("n", "LESSER", 5.5)));
  CHECK(MatchesFilter(d, f("n", "LESSER_EQUAL", 5)));
  CHECK(!MatchesFilter(d, f("n", "LESSER", "zzz"))); // cross-type never ranges
  CHECK(MatchesFilter(d, f("s", "GREATER", "a")));
  CHECK(MatchesFilter(d, f("nested.v", "EQUAL", true)));
  CHECK(MatchesFilter(d, f("nul", "EQUAL", nullptr)));
  CHECK(!MatchesFilter(d, f("missing", "EQUAL", nullptr)));
  CHECK(MatchesFilter(d, f("tags", "ARRAY.IN", "x")));
  CHECK(!MatchesFilter(d, f("tags", "ARRAY.IN", "z")));
  CHECK(MatchesFilter(d, f("tags", "ARRAY.NOT_IN", "z")));
  CHECK(!MatchesFilter(d, f("tags", "ARRAY.NOT_IN", "x")));
  CHECK(!MatchesFilter(d, f("n", "ARRAY.NOT_IN", "z"))); // not an array
  CHECK(MatchesFilter(d, f("tags", "ARRAY.EQUALS", json({"x", "y"}))));
  CHECK(!MatchesFilter(d, f("tags", "ARRAY.EQUALS", json({"y", "x"}))));
  CHECK(MatchesFilter(d, f("tags", "ARRAY.ELEMENT_AT",
                           json({{"index", 1}, {"value", "y"}}))));
  CHECK(MatchesFilter(d, f("tags", "ARRAY.ELEMENT_AT",
                           json({{"index", -2}, {"value", "x"}}))));
  CHECK(!MatchesFilter(d, f("tags", "ARRAY.ELEMENT_AT",
                            json({{"index", 2}, {"value", "x"}}))));
  CHECK(!MakeFilter("a", "LIKE", 1).has_value());
  CHECK(!MakeFilter("a", "ARRAY.EQUALS", 1).has_value());
  CHECK(!MakeFilter("a", "ARRAY.ELEMENT_AT", json({{"index", "0"}})).has_value());
}

// ---------------------------------------------------------------------------
TEST(TestDatabaseCrud) {
  MemoryStorage storage;
  Database db(storage);
  std::vector<CommitInfo> commits;
  db.SetCommitListener([&](const CommitInfo &c) { commits.push_back(c); });

  auto w1 = db.Write("ns", P("users/u1"), {{"name", "a"}, {"age", 1}}, false);
  CHECK(w1 && w1->created && w1->version == 1);
  auto w2 = db.Write("ns", P("users/u1"), {{"age", 2}, {"x", {{"y", 1}}}}, true);
  CHECK(w2 && !w2->created && w2->version == 2);
  auto r = db.Read("ns", P("users/u1"));
  CHECK(r && *r);
  CHECK_EQ((*r)->data, json({{"name", "a"}, {"age", 2}, {"x", {{"y", 1}}}}));
  CHECK_EQ((*r)->version, 2u);
  auto w3 = db.Write("ns", P("users/u1"), {{"only", true}}, false);
  CHECK(w3 && w3->version == 3);
  CHECK_EQ((*db.Read("ns", P("users/u1")))->data, json({{"only", true}}));
  CHECK(!db.Write("ns", P("users/u1"), json::array(), false).has_value());
  CHECK(!db.Write("ns", P("users"), json::object(), false).has_value());
  CHECK_EQ(commits.size(), 3u);
  CHECK_EQ(commits.back().sequence, 3u);
  CHECK(commits.front().changes[0].before == std::nullopt);

  // Namespace isolation.
  CHECK(!*db.Read("other", P("users/u1")));
  CHECK(db.Write("other", P("users/u1"), {{"tenant", "other"}}, false));
  CHECK_EQ((*db.Read("ns", P("users/u1")))->data, json({{"only", true}}));
  CHECK_EQ(db.QueryAll("other", P("users"), {})->size(), 1u);

  // Recursive delete.
  CHECK(db.Write("ns", P("users/u1/posts/p1"), {{"t", 1}}, false));
  CHECK(db.Write("ns", P("users/u1/posts/p1/likes/l1"), {{"t", 1}}, false));
  CHECK(db.Write("ns", P("users/u2"), {{"t", 1}}, false));
  CHECK(db.Write("ns", P("usersX/u1"), {{"t", 1}}, false));
  auto del = db.Delete("ns", P("users/u1"));
  CHECK(del && !del->is_collection && del->affected_count == 3);
  CHECK(!*db.Read("ns", P("users/u1/posts/p1")));
  CHECK(*db.Read("ns", P("users/u2")));
  auto del_none = db.Delete("ns", P("users/u1"));
  CHECK(del_none && del_none->affected_count == 0);
  auto delc = db.Delete("ns", P("users"));
  CHECK(delc && delc->is_collection && delc->affected_count == 1);
  CHECK(*db.Read("ns", P("usersX/u1")));          // sibling untouched
  CHECK(*db.Read("other", P("users/u1")));        // other namespace untouched

  // Failed commits leave state and sequence untouched.
  const size_t before = commits.size();
  storage.fail_writes = true;
  CHECK(!db.Write("ns", P("users/z"), {{"a", 1}}, false).has_value());
  storage.fail_writes = false;
  CHECK_EQ(commits.size(), before);
  auto ok = db.Write("ns", P("users/z"), {{"a", 1}}, false);
  CHECK(ok && ok->version == 1);
  CHECK_EQ(commits.back().sequence, commits[before - 1].sequence + 1);

  // Sequence persists across restarts.
  Database reopened(storage);
  uint64_t seq_now = reopened.WithCommitLock([](uint64_t s) { return s; });
  CHECK_EQ(seq_now, commits.back().sequence);
}

TEST(TestQueries) {
  MemoryStorage storage;
  Database db(storage);
  for (int i = 0; i < 10; ++i)
    db.Write("ns", P("items/i" + std::to_string(i)),
             {{"n", i % 4}, {"tag", i % 2 ? "odd" : "even"}, {"i", i}}, false);
  db.Write("ns", P("items/nofield"), {{"tag", "odd"}}, false);
  db.Write("ns", P("items/i0/sub/s"), {{"n", 0}}, false); // not a direct child

  auto all = db.Query("ns", P("items"), {}, std::nullopt);
  CHECK(all && all->documents.size() == 11 && !all->has_more);

  auto evens = db.Query("ns", P("items"),
                        Spec({{"where", {{{"field", "tag"},
                                          {"op", "EQUAL"},
                                          {"value", "even"}}}}}),
                        std::nullopt);
  CHECK(evens && evens->documents.size() == 5);

  // orderBy n DESC then default id tie-break (descending); nofield excluded.
  auto spec = Spec({{"orderBy", {{{"field", "n"}, {"direction", "DESCENDING"}}}},
                    {"limit", 4},
                    {"project", {"n"}}});
  auto p1 = db.Query("ns", P("items"), spec, std::nullopt);
  CHECK(p1 && p1->documents.size() == 4 && p1->has_more);
  std::vector<std::string> seen;
  std::optional<std::string> cursor;
  for (int guard = 0; guard < 10; ++guard) {
    auto page = db.Query("ns", P("items"), spec, cursor);
    CHECK(page.has_value());
    for (auto &d : page->documents)
      seen.push_back(d.id);
    if (!page->has_more) {
      CHECK(!page->next_cursor);
      break;
    }
    cursor = page->next_cursor;
  }
  const std::vector<std::string> expected = {"i7", "i3", "i6", "i2", "i9",
                                             "i5", "i1", "i8", "i4", "i0"};
  CHECK(seen == expected);

  // Cursor whose document changed so it no longer matches still resumes.
  auto even_spec = Spec({{"where", {{{"field", "tag"},
                                     {"op", "EQUAL"},
                                     {"value", "even"}}}},
                         {"limit", 2}});
  auto e1 = db.Query("ns", P("items"), even_spec, std::nullopt);
  CHECK(e1 && e1->next_cursor && *e1->next_cursor == "i2");
  db.Write("ns", P("items/i2"), {{"tag", "odd"}, {"n", 2}, {"i", 2}}, false);
  auto e2 = db.Query("ns", P("items"), even_spec, e1->next_cursor);
  CHECK(e2 && e2->documents.size() == 2 && e2->documents[0].id == "i4");
  CHECK(!db.Query("ns", P("items"), even_spec, std::string("nope")).has_value());
  CHECK(!db.Query("ns", P("items"), even_spec, std::string("a/b")).has_value());

  // MUTATE_MANY
  std::vector<Filter> where = {*MakeFilter("tag", "EQUAL", "odd")};
  auto mm = db.MutateMany("ns", P("items"), where, {*ParseFieldPath("i")},
                          {{*ParseFieldPath("meta.flag"), true}});
  CHECK(mm && mm->affected_count == 7); // i1,i3,i5,i7,i9,i2,nofield
  auto i1 = *db.Read("ns", P("items/i1"));
  CHECK(i1 && !i1->data.contains("i") && i1->data["meta"]["flag"] == true);
  CHECK_EQ(i1->version, 2u);
  auto again = db.MutateMany("ns", P("items"), where, {*ParseFieldPath("i")},
                             {{*ParseFieldPath("meta.flag"), true}});
  CHECK(again && again->affected_count == 0); // no-op: nothing changed
  CHECK(!db.MutateMany("ns", P("items"), {}, {}, {}).has_value());
  CHECK(!db.MutateMany("ns", P("items"), {}, {*ParseFieldPath("a")},
                       {{*ParseFieldPath("a.b"), 1}})
             .has_value());
}

// ---------------------------------------------------------------------------
// Client model: applies EVENT frames exactly as a client would.
struct ClientView {
  std::vector<json> docs; // {path, data, _v}
  bool got_ack = false;
  bool got_snapshot = false;
  std::vector<std::string> types;
  // State a client would persist: the view at the last commit_complete event.
  std::string complete_token;
  std::vector<json> complete_docs;
  bool resumed = false;
  bool Apply(const json &msg) {
    if (msg["meta"]["type"] == "ACK") {
      got_ack = true;
      resumed = msg["payload"].value("resumed", false);
      return true;
    }
    if (msg["meta"]["type"] != "EVENT")
      return false;
    const bool ok = ApplyEvent(msg["payload"]);
    if (ok && msg["payload"]["commit_complete"] == true) {
      complete_token = msg["payload"]["resume_token"];
      complete_docs = docs;
    }
    return ok;
  }
  bool ApplyEvent(const json &p) {
    const std::string t = p["event_type"];
    types.push_back(t);
    if (t == "SYNCED")
      return true;
    if (t == "RESET") {
      docs.clear();
      return true;
    }
    if (t == "SNAPSHOT") {
      got_snapshot = true;
      docs.clear();
      for (const auto &d : p["snapshot"]["documents"]) {
        if (d["index"] != docs.size())
          return false;
        docs.push_back(d);
      }
      return true;
    }
    const auto &delta = p["delta"];
    if (t == "REMOVED") {
      size_t i = delta["old_index"];
      if (i >= docs.size() || docs[i]["path"] != delta["path"])
        return false;
      docs.erase(docs.begin() + static_cast<long>(i));
      return delta["new_index"].is_null();
    }
    if (t == "ADDED") {
      size_t i = delta["new_index"];
      if (i > docs.size() || !delta["old_index"].is_null())
        return false;
      docs.insert(docs.begin() + static_cast<long>(i), delta["document"]);
      return true;
    }
    if (t == "MODIFIED") {
      size_t from = delta["old_index"], to = delta["new_index"];
      if (from >= docs.size() || docs[from]["path"] != delta["path"])
        return false;
      docs.erase(docs.begin() + static_cast<long>(from));
      if (to > docs.size())
        return false;
      docs.insert(docs.begin() + static_cast<long>(to), delta["document"]);
      return true;
    }
    return false;
  }
};

TEST(TestSubscriptionsRandomized) {
  MemoryStorage storage;
  Database db(storage);
  std::map<std::string, ClientView> clients; // key: subscription id
  bool protocol_ok = true;
  SubscriptionManager subs(
      db, [&](const std::string &, json msg, WireEncoding) {
        std::string sid = msg["meta"]["type"] == "ACK"
                              ? msg["payload"]["subscription_id"]
                              : msg["meta"]["subscription_id"];
        if (!clients[sid].Apply(msg)) {
          protocol_ok = false;
          std::cerr << "bad event " << msg.dump() << std::endl;
        }
      });
  db.SetCommitListener([&](const CommitInfo &c) { subs.OnCommit(c); });
  subs.AddConnection("c1");

  struct Case {
    std::string id;
    std::string path;
    json spec;
  };
  const std::vector<Case> cases = {
      {"all", "col", json::object()},
      {"ordered", "col",
       {{"orderBy", {{{"field", "score"}, {"direction", "DESCENDING"}}}}}},
      {"filtered", "col",
       {{"where", {{{"field", "score"}, {"op", "GREATER_EQUAL"}, {"value", 3}}}},
        {"orderBy", {{{"field", "score"}}}}}},
      {"limited", "col",
       {{"orderBy", {{{"field", "score"}}, {{"field", "name"}}}}, {"limit", 3}}},
      {"limited_filtered", "col",
       {{"where", {{{"field", "tags"}, {"op", "ARRAY.IN"}, {"value", "red"}}}},
        {"limit", 2}}},
      {"projected", "col", {{"project", {"name"}}}},
      {"doc", "col/d3", json::object()},
      {"nested", "col/d1/sub", json::object()},
  };
  for (const auto &c : cases) {
    json msg = {{"meta", {{"type", "SUBSCRIBE"}, {"request_id", c.id}}},
                {"payload", c.spec}};
    msg["payload"]["path"] = c.path;
    msg["payload"]["subscription_id"] = c.id;
    auto req = wire::ParseSubscribe(msg);
    CHECK(req.has_value());
    json ack = {{"meta", {{"type", "ACK"}}},
                {"payload", {{"subscription_id", c.id}}}};
    auto ok = subs.Subscribe(
        "c1", "ns", WireEncoding::Json, ack,
        SubscribeParams{req->subscription_id, req->path, req->spec, std::nullopt, {}});
    CHECK(ok.has_value());
    CHECK(clients[c.id].got_ack && clients[c.id].got_snapshot);
  }
  // Duplicate id is rejected; document subscriptions reject query clauses.
  CHECK(!subs.Subscribe("c1", "ns", WireEncoding::Json, {},
                        SubscribeParams{"all", P("col"), {}, std::nullopt, {}})
             .has_value());
  CHECK(!subs.Subscribe("c1", "ns", WireEncoding::Json, {},
                        SubscribeParams{"x", P("col/d1"),
                                        Spec({{"limit", 1}}), std::nullopt, {}})
             .has_value());

  std::mt19937 rng(12345);
  auto rnd = [&](int n) { return static_cast<int>(rng() % n); };
  const std::vector<std::string> colors = {"red", "green", "blue"};
  for (int step = 0; step < 1500; ++step) {
    const std::string id = "d" + std::to_string(rnd(12));
    const int action = rnd(10);
    if (action < 5) {
      json data = {{"score", rnd(6)},
                   {"name", "n" + std::to_string(rnd(5))},
                   {"tags", {colors[rnd(3)], colors[rnd(3)]}}};
      if (rnd(8) == 0)
        data.erase("score"); // excluded from orderBy subscriptions
      db.Write("ns", P("col/" + id), data, rnd(3) == 0);
    } else if (action < 7) {
      db.Delete("ns", P("col/" + id));
    } else if (action < 8) {
      db.Write("ns", P("col/d1/sub/s" + std::to_string(rnd(3))),
               {{"v", rnd(100)}}, false);
    } else if (action < 9) {
      std::vector<Filter> where = {*MakeFilter("score", "LESSER", rnd(6))};
      db.MutateMany("ns", P("col"), where, {},
                    {{*ParseFieldPath("score"), rnd(6)}});
    } else {
      db.Write("other", P("col/" + id), {{"score", 1}}, false); // other ns
    }
    if (rnd(200) == 0)
      db.Delete("ns", P("col"));

    // After every commit each client view must equal a fresh evaluation.
    for (const auto &c : cases) {
      auto &view = clients[c.id].docs;
      auto path = P(c.path);
      std::vector<Document> truth;
      QuerySpec spec;
      if (path.IsDocument()) {
        if (auto d = *db.Read("ns", path))
          truth.push_back(*d);
      } else {
        json msg = {{"meta", {{"type", "SUBSCRIBE"}, {"request_id", "x"}}},
                    {"payload", c.spec}};
        msg["payload"]["path"] = c.path;
        msg["payload"]["subscription_id"] = "x";
        spec = wire::ParseSubscribe(msg)->spec;
        truth = *db.QueryAll("ns", path, spec);
        if (spec.limit && truth.size() > *spec.limit)
          truth.resize(*spec.limit);
      }
      bool same = view.size() == truth.size();
      for (size_t i = 0; same && i < truth.size(); ++i)
        same = view[i]["path"] == truth[i].path &&
               view[i]["_v"] == truth[i].version &&
               view[i]["data"] == Project(truth[i].data, spec.project);
      if (!same) {
        std::cerr << "view mismatch for " << c.id << " at step " << step
                  << std::endl;
        CHECK(false);
        return;
      }
    }
  }
  CHECK(protocol_ok);

  // Unsubscribe / connection teardown.
  CHECK(subs.Unsubscribe("c1", "all").has_value());
  CHECK(!subs.Unsubscribe("c1", "all").has_value());
  CHECK(subs.Acknowledge("c1", "doc", "5").has_value());
  CHECK(!subs.Acknowledge("c1", "doc", "x5").has_value());
  CHECK_EQ(subs.SubscriptionCount(), cases.size() - 1);
  subs.RemoveConnection("c1");
  CHECK_EQ(subs.SubscriptionCount(), 0u);
  // Subscribing on a closed connection is a silent no-op.
  CHECK(subs.Subscribe("c1", "ns", WireEncoding::Json, {},
                       SubscribeParams{"late", P("col"), {}, std::nullopt, {}})
            .has_value());
  CHECK_EQ(subs.SubscriptionCount(), 0u);
}

TEST(TestResume) {
  MemoryStorage storage;
  Database db(storage);
  std::vector<std::string> types;
  SubscriptionManager subs(db, [&](const std::string &, json msg, WireEncoding) {
    types.push_back(msg["meta"]["type"] == "EVENT"
                        ? msg["payload"]["event_type"].get<std::string>()
                        : msg["meta"]["type"].get<std::string>());
  });
  db.SetCommitListener([&](const CommitInfo &c) { subs.OnCommit(c); });
  subs.AddConnection("c");
  db.Write("ns", P("a/b"), {{"x", 1}}, false); // seq 1
  auto sub = [&](const char *id, const char *token) {
    types.clear();
    return subs.Subscribe("c", "ns", WireEncoding::Json,
                          json{{"meta", {{"type", "ACK"}}}},
                          SubscribeParams{id, P("a"), {}, std::string(token), {}});
  };
  // Already up to date: nothing to replay.
  CHECK(sub("s1", "1").has_value());
  CHECK((types == std::vector<std::string>{"ACK", "SYNCED"}));
  // From before the first commit: replay the insert.
  CHECK(sub("s2", "0").has_value());
  CHECK((types == std::vector<std::string>{"ACK", "ADDED", "SYNCED"}));
  // A token from the future (or another server) cannot be honoured.
  CHECK(sub("s3", "99").has_value());
  CHECK((types == std::vector<std::string>{"ACK", "RESET", "SNAPSHOT"}));
  CHECK(!sub("s4", "abc").has_value());
}

TEST(TestResumeRetention) {
  MemoryStorage storage;
  {
    Database db(storage, DatabaseOptions{.log_retention = 3});
    for (int i = 0; i < 5; ++i)
      db.Write("ns", P("a/b"), {{"x", i}}, false); // seq 1..5
    db.WithCommitLock([&](uint64_t seq) {
      CHECK_EQ(seq, 5u);
      CHECK(!db.CanResumeFrom(1)); // commits 2..5 = 4 > 3 retained
      CHECK(db.CanResumeFrom(2));
      CHECK(db.CanResumeFrom(5));
      CHECK(!db.CanResumeFrom(6));
      auto at2 = db.StateAt("ns", P("a/b"), 2);
      CHECK(at2 && at2->size() == 1 && (*at2)[0].data["x"] == 1);
      auto at4 = db.StateAt("ns", P("a"), 4);
      CHECK(at4 && at4->size() == 1 && (*at4)[0].data["x"] == 3);
      return 0;
    });
    size_t log_records = 0;
    storage.Scan(std::string("L\0", 2), [&](std::string_view, ByteView) {
      ++log_records;
      return true;
    });
    CHECK_EQ(log_records, 3u);
  }
  {
    // Lowering retention on restart prunes the log immediately.
    Database db(storage, DatabaseOptions{.log_retention = 1});
    size_t log_records = 0;
    storage.Scan(std::string("L\0", 2), [&](std::string_view, ByteView) {
      ++log_records;
      return true;
    });
    CHECK_EQ(log_records, 1u);
    db.WithCommitLock([&](uint64_t) {
      CHECK(!db.CanResumeFrom(3));
      CHECK(db.CanResumeFrom(4));
      return 0;
    });
  }
  {
    // Disabled log: never resumable, nothing retained.
    Database db(storage, DatabaseOptions{.log_retention = 0});
    db.Write("ns", P("a/c"), {{"x", 1}}, false);
    db.WithCommitLock([&](uint64_t seq) {
      CHECK(!db.CanResumeFrom(seq));
      return 0;
    });
    size_t log_records = 0;
    storage.Scan(std::string("L\0", 2), [&](std::string_view, ByteView) {
      ++log_records;
      return true;
    });
    CHECK_EQ(log_records, 0u);
  }
}

// Clients persist their view at commit_complete boundaries and later resume
// from that token; the deltas they receive must bring them exactly to the
// current view.
TEST(TestResumeRandomized) {
  MemoryStorage storage;
  Database db(storage);
  std::map<std::string, ClientView> clients;
  bool protocol_ok = true;
  SubscriptionManager subs(
      db, [&](const std::string &, json msg, WireEncoding) {
        std::string sid = msg["meta"]["type"] == "ACK"
                              ? msg["payload"]["subscription_id"]
                              : msg["meta"]["subscription_id"];
        if (!clients[sid].Apply(msg)) {
          protocol_ok = false;
          std::cerr << "bad event " << msg.dump() << std::endl;
        }
      });
  db.SetCommitListener([&](const CommitInfo &c) { subs.OnCommit(c); });
  subs.AddConnection("c");

  const std::vector<std::pair<std::string, json>> cases = {
      {"col", json::object()},
      {"col", {{"orderBy", {{{"field", "score"}, {"direction", "DESCENDING"}}}}}},
      {"col", {{"where", {{{"field", "score"}, {"op", "LESSER"}, {"value", 3}}}},
               {"limit", 2}}},
      {"col/d2", json::object()},
  };
  auto spec_of = [&](size_t i) {
    json msg = {{"meta", {{"type", "SUBSCRIBE"}, {"request_id", "x"}}},
                {"payload", cases[i].second}};
    msg["payload"]["path"] = cases[i].first;
    msg["payload"]["subscription_id"] = "x";
    return wire::ParseSubscribe(msg)->spec;
  };
  int generation = 0;
  std::vector<std::string> live(cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    live[i] = "g0-" + std::to_string(i);
    CHECK(subs.Subscribe("c", "ns", WireEncoding::Json,
                         json{{"meta", {{"type", "ACK"}}},
                              {"payload", {{"subscription_id", live[i]}}}},
                         SubscribeParams{live[i], P(cases[i].first), spec_of(i),
                                         std::nullopt, {}})
              .has_value());
  }

  std::mt19937 rng(777);
  auto rnd = [&](int n) { return static_cast<int>(rng() % n); };
  int resumes = 0;
  for (int step = 0; step < 600; ++step) {
    const std::string id = "d" + std::to_string(rnd(6));
    switch (rnd(4)) {
    case 0:
    case 1:
      db.Write("ns", P("col/" + id), {{"score", rnd(6)}}, rnd(2) == 0);
      break;
    case 2:
      db.Delete("ns", P("col/" + id));
      break;
    default:
      db.MutateMany("ns", P("col"), {*MakeFilter("score", "GREATER", rnd(6))},
                    {}, {{*ParseFieldPath("score"), rnd(6)}});
    }
    if (rnd(5) != 0)
      continue;
    // Resume one case: drop the live subscription and come back with the
    // persisted token + view.
    const size_t i = static_cast<size_t>(rnd(static_cast<int>(cases.size())));
    ClientView persisted = clients[live[i]];
    CHECK(subs.Unsubscribe("c", live[i]).has_value());
    live[i] = "g" + std::to_string(++generation) + "-" + std::to_string(i);
    ClientView &fresh = clients[live[i]];
    fresh.docs = persisted.complete_docs;
    CHECK(subs.Subscribe("c", "ns", WireEncoding::Json,
                         json{{"meta", {{"type", "ACK"}}},
                              {"payload", {{"subscription_id", live[i]}}}},
                         SubscribeParams{live[i], P(cases[i].first), spec_of(i),
                                         persisted.complete_token, {}})
              .has_value());
    CHECK(fresh.resumed);
    ++resumes;
    // The resumed view must equal a fresh evaluation.
    std::vector<Document> truth;
    auto path = P(cases[i].first);
    if (path.IsDocument()) {
      if (auto d = *db.Read("ns", path))
        truth.push_back(*d);
    } else {
      auto spec = spec_of(i);
      truth = *db.QueryAll("ns", path, spec);
      if (spec.limit && truth.size() > *spec.limit)
        truth.resize(*spec.limit);
    }
    bool same = fresh.docs.size() == truth.size();
    for (size_t k = 0; same && k < truth.size(); ++k)
      same = fresh.docs[k]["path"] == truth[k].path &&
             fresh.docs[k]["_v"] == truth[k].version;
    if (!same) {
      std::cerr << "resume mismatch for case " << i << " at step " << step
                << std::endl;
      CHECK(false);
      return;
    }
  }
  CHECK(resumes > 50);
  CHECK(protocol_ok);
}

TEST(TestStorageIterate) {
  const auto dir =
      std::filesystem::temp_directory_path() / "pod_unit_test_iterate";
  std::filesystem::remove_all(dir);
  MemoryStorage mem;
  {
    LevelDBStorageDriver ldb(dir.string());
    for (const char *k : {"a", "b1", "b2", "b3", "b\xff", "c"}) {
      const ByteVector v{1};
      ldb.Put(k, v);
      mem.Put(k, v);
    }
    auto collect = [](StorageReader &r, std::string_view prefix,
                      std::string_view start, bool reverse) {
      std::vector<std::string> out;
      r.Iterate(prefix, start, reverse, [&](std::string_view k, ByteView) {
        out.emplace_back(k);
        return true;
      });
      return out;
    };
    for (StorageReader *r : {static_cast<StorageReader *>(&ldb),
                             static_cast<StorageReader *>(&mem)}) {
      CHECK((collect(*r, "b", "", false) ==
             std::vector<std::string>{"b1", "b2", "b3", "b\xff"}));
      CHECK((collect(*r, "b", "", true) ==
             std::vector<std::string>{"b\xff", "b3", "b2", "b1"}));
      CHECK((collect(*r, "b", "b2", false) ==
             std::vector<std::string>{"b2", "b3", "b\xff"}));
      CHECK((collect(*r, "b", "b2", true) ==
             std::vector<std::string>{"b2", "b1"}));
      CHECK((collect(*r, "b", "b25", true) ==
             std::vector<std::string>{"b2", "b1"}));
      CHECK((collect(*r, "b", "z", true) ==
             std::vector<std::string>{"b\xff", "b3", "b2", "b1"}));
      CHECK(collect(*r, "x", "", true).empty());
    }
    // Snapshots are immune to later writes.
    auto snap = ldb.Snapshot();
    ldb.Put("b4", ByteVector{2});
    ldb.Delete("b1");
    CHECK((collect(*snap, "b", "", false) ==
           std::vector<std::string>{"b1", "b2", "b3", "b\xff"}));
    CHECK(snap->Get("b1").first.is_ok && !snap->Get("b1").first.is_not_found);
    CHECK(ldb.Get("b1").first.is_not_found);
  }
  std::filesystem::remove_all(dir);
}

TEST(TestIndexEncodingOrder) {
  std::vector<json> values = {nullptr, false, true, -1e300, -5, -1.5, -0.0,
                              0, 0.5, 1, 1.0, 2, 1e15,
                              json(std::numeric_limits<uint64_t>::max()),
                              std::nan(""), "", "a", "x",
                              std::string("a\0b", 3), "ab", "b", "é",
                              json::array(), json({1}), json({1, 2}), json({2}),
                              json({"a"}), json::object(), json({{"a", 1}}),
                              json({{"a", 2}}), json({{"b", 0}}),
                              json({{"a", 1}, {"b", 1}})};
  for (const auto &a : values)
    for (const auto &b : values) {
      std::string ea, eb;
      EncodeIndexValue(ea, a);
      EncodeIndexValue(eb, b);
      const int c = CompareValues(a, b);
      if (c == 0)
        CHECK_EQ(ea, eb);
      else if (c < 0)
        CHECK(ea < eb); // all values here are exactly representable
      else
        CHECK(ea > eb);
      // Prefix-freedom: no encoding is a proper prefix of another.
      if (ea != eb)
        CHECK(!eb.starts_with(ea));
    }
  // Large integers that round to the same double share a position.
  std::string x, y;
  EncodeIndexValue(x, json(9007199254740993ULL));
  EncodeIndexValue(y, json(9007199254740992ULL));
  CHECK_EQ(x, y);
}

TEST(TestIndexDefinitions) {
  auto d = MakeIndexDefinition(
      "posts", json::array({{{"field", "author"}},
                            {{"field", "tags"}, {"mode", "ARRAY_CONTAINS"}}}));
  CHECK(d.has_value());
  CHECK_EQ(d->id, "posts:author,tags[]");
  CHECK(IndexDefinitionFromJson(d->ToJson()).has_value());
  CHECK_EQ(IndexDefinitionFromJson(d->ToJson())->id, d->id);
  CHECK(!MakeIndexDefinition("a/b", json::array({{{"field", "x"}}})));
  CHECK(!MakeIndexDefinition("posts", json::array()));
  CHECK(!MakeIndexDefinition("posts", json::array({{{"field", "x"}},
                                                   {{"field", "x"}}})));
  CHECK(!MakeIndexDefinition(
      "posts", json::array({{{"field", "a"}, {"mode", "ARRAY_CONTAINS"}},
                            {{"field", "b"}, {"mode", "ARRAY_CONTAINS"}}})));
  CHECK(!MakeIndexDefinition("posts", json::array({{{"field", "a"},
                                                    {"mode", "WEIRD"}}})));
}

// Every query must return identical pages with and without indexes.
TEST(TestIndexedQueriesMatchScan) {
  MemoryStorage s1, s2;
  Database indexed(s1), plain(s2);
  std::mt19937 rng(99);
  auto rnd = [&](int n) { return static_cast<int>(rng() % n); };
  const std::vector<std::string> colors = {"red", "green", "blue", "black"};
  auto random_doc = [&]() {
    json d = {{"a", rnd(5)},
              {"b", rnd(3) == 0 ? json(rnd(4) + 0.5) : json(rnd(4))},
              {"c", colors[rnd(4)]},
              {"tags", {colors[rnd(4)], colors[rnd(4)]}}};
    if (rnd(6) == 0)
      d.erase("b");
    if (rnd(9) == 0)
      d["a"] = "str";
    return d;
  };
  auto write_both = [&](const std::string &path, const json &d, bool merge) {
    indexed.Write("ns", P(path), d, merge);
    plain.Write("ns", P(path), d, merge);
  };
  // Some data exists before the indexes (exercises backfill).
  for (int i = 0; i < 60; ++i)
    write_both("p/" + std::to_string(rnd(80)), random_doc(), false);
  write_both("other/x/p/z", random_doc(), false); // collection group member
  for (const json &fields :
       {json::array({{{"field", "a"}}}),
        json::array({{{"field", "a"}}, {{"field", "b"}}}),
        json::array({{{"field", "c"}}, {{"field", "a"}}}),
        json::array({{{"field", "tags"}, {"mode", "ARRAY_CONTAINS"}},
                     {{"field", "a"}}}),
        json::array({{{"field", "b"}}})}) {
    auto def = MakeIndexDefinition("p", fields);
    CHECK(def.has_value());
    auto r = indexed.CreateIndex("ns", *def);
    CHECK(r && r->created);
    auto again = indexed.CreateIndex("ns", *def);
    CHECK(again && !again->created);
  }
  CHECK_EQ(indexed.ListIndexes("ns").size(), 5u);
  CHECK(indexed.ListIndexes("other").empty());

  const std::vector<json> queries = {
      {{"where", {{{"field", "a"}, {"op", "EQUAL"}, {"value", 2}}}}},
      {{"where", {{{"field", "a"}, {"op", "EQUAL"}, {"value", 1}},
                  {{"field", "b"}, {"op", "GREATER_EQUAL"}, {"value", 1}}}}},
      {{"where", {{{"field", "a"}, {"op", "EQUAL"}, {"value", 1}}}},
       {"orderBy", {{{"field", "b"}, {"direction", "DESCENDING"}}}},
       {"limit", 3}},
      {{"where", {{{"field", "a"}, {"op", "EQUAL"}, {"value", 3}}}},
       {"orderBy", {{{"field", "b"}}}},
       {"limit", 2}},
      {{"orderBy", {{{"field", "b"}}}}, {"limit", 4}},
      {{"orderBy", {{{"field", "b"}, {"direction", "DESCENDING"}}}},
       {"limit", 5}},
      {{"where", {{{"field", "b"}, {"op", "LESSER"}, {"value", 2}},
                  {{"field", "b"}, {"op", "GREATER"}, {"value", 0}}}},
       {"orderBy", {{{"field", "b"}}}},
       {"limit", 3}},
      {{"where", {{{"field", "c"}, {"op", "EQUAL"}, {"value", "red"}},
                  {{"field", "a"}, {"op", "GREATER"}, {"value", 1}}}}},
      {{"where", {{{"field", "c"}, {"op", "EQUAL"}, {"value", "blue"}}}},
       {"orderBy", {{{"field", "a"}}}},
       {"limit", 2}},
      {{"where", {{{"field", "tags"}, {"op", "ARRAY.IN"}, {"value", "green"}},
                  {{"field", "a"}, {"op", "LESSER_EQUAL"}, {"value", 2}}}}},
      {{"where", {{{"field", "tags"}, {"op", "ARRAY.IN"}, {"value", "red"}},
                  {{"field", "a"}, {"op", "EQUAL"}, {"value", 0}}}},
       {"limit", 2}},
      {{"where", {{{"field", "a"}, {"op", "EQUAL"}, {"value", "str"}}}}},
      {{"limit", 7}},
  };

  bool any_index_used = false;
  auto compare_all = [&](int round) {
    for (size_t qi = 0; qi < queries.size(); ++qi) {
      const QuerySpec spec = Spec(queries[qi]);
      std::optional<std::string> c1, c2;
      for (int page = 0; page < 40; ++page) {
        auto p1 = indexed.Query("ns", P("p"), spec, c1);
        auto p2 = plain.Query("ns", P("p"), spec, c2);
        CHECK(p1.has_value() && p2.has_value());
        if (!p1 || !p2)
          return;
        CHECK(!p2->index_used);
        if (p1->index_used)
          any_index_used = true;
        bool same = p1->documents.size() == p2->documents.size() &&
                    p1->has_more == p2->has_more &&
                    p1->next_cursor == p2->next_cursor;
        for (size_t k = 0; same && k < p1->documents.size(); ++k)
          same = p1->documents[k].path == p2->documents[k].path &&
                 p1->documents[k].data == p2->documents[k].data;
        if (!same) {
          std::cerr << "query " << qi << " differs in round " << round
                    << " page " << page << " (index "
                    << p1->index_used.value_or("none") << ")" << std::endl;
          CHECK(false);
          return;
        }
        if (!p1->has_more)
          break;
        c1 = p1->next_cursor;
        c2 = p2->next_cursor;
      }
      auto all1 = indexed.QueryAll("ns", P("p"), spec);
      auto all2 = plain.QueryAll("ns", P("p"), spec);
      CHECK(all1 && all2 && all1->size() == all2->size());
    }
  };

  for (int round = 0; round < 30; ++round) {
    compare_all(round);
    // Mutations keep the indexes in sync.
    for (int i = 0; i < 15; ++i) {
      const std::string path = "p/" + std::to_string(rnd(80));
      switch (rnd(4)) {
      case 0:
      case 1:
        write_both(path, random_doc(), rnd(3) == 0);
        break;
      case 2:
        indexed.Delete("ns", P(path));
        plain.Delete("ns", P(path));
        break;
      default: {
        std::vector<Filter> w = {*MakeFilter("a", "EQUAL", rnd(5))};
        const int value = rnd(4);
        indexed.MutateMany("ns", P("p"), w, {}, {{*ParseFieldPath("b"), value}});
        plain.MutateMany("ns", P("p"), w, {}, {{*ParseFieldPath("b"), value}});
      }
      }
    }
  }
  CHECK(any_index_used);

  // The equality index is actually narrowing the scan.
  auto narrow = indexed.Query("ns", P("p"),
                              Spec({{"where", {{{"field", "a"},
                                                {"op", "EQUAL"},
                                                {"value", 4}}}}}),
                              std::nullopt);
  auto full = plain.Query("ns", P("p"),
                          Spec({{"where", {{{"field", "a"},
                                            {"op", "EQUAL"},
                                            {"value", 4}}}}}),
                          std::nullopt);
  CHECK(narrow && full && narrow->index_used.has_value());
  CHECK(narrow->documents_scanned < full->documents_scanned);

  // Dropping removes every entry; queries fall back to scans.
  for (const auto &d : indexed.ListIndexes("ns"))
    CHECK(indexed.DropIndex("ns", d.id).has_value());
  CHECK(!indexed.DropIndex("ns", "p:a").has_value());
  size_t entries = 0;
  s1.Scan(std::string("i\0", 2), [&](std::string_view, ByteView) {
    ++entries;
    return true;
  });
  CHECK_EQ(entries, 0u);
  compare_all(-1);

  // Index definitions persist across restarts.
  CHECK(indexed.CreateIndex("ns", *MakeIndexDefinition(
                                      "p", json::array({{{"field", "a"}}}))));
  Database reopened(s1);
  CHECK_EQ(reopened.ListIndexes("ns").size(), 1u);
}

TEST(TestLevelDB) {
  const auto dir =
      std::filesystem::temp_directory_path() / "pod_unit_test_leveldb";
  std::filesystem::remove_all(dir);
  {
    LevelDBStorageDriver driver(dir.string());
    Database db(driver);
    CHECK(db.Write("ns", P("users/u1"), {{"a", 1}}, false));
    CHECK(db.Write("ns", P("users/u2"), {{"a", 2}}, false));
    CHECK(db.Write("ns", P("users/u1/posts/p"), {{"a", 3}}, false));
    CHECK_EQ(db.QueryAll("ns", P("users"), {})->size(), 2u);
    auto d = db.Delete("ns", P("users"));
    CHECK(d && d->affected_count == 3);
    CHECK(db.Write("ns", P("users/u3"), {{"a", 1}}, false));
  }
  {
    LevelDBStorageDriver driver(dir.string());
    Database db(driver);
    CHECK_EQ(db.WithCommitLock([](uint64_t s) { return s; }), 5u);
    auto r = db.Read("ns", P("users/u3"));
    CHECK(r && *r && (*r)->data["a"] == 1);
  }
  std::filesystem::remove_all(dir);
}

TEST(TestWireParsing) {
  auto msg = [](json meta, json payload) {
    return json{{"meta", meta}, {"payload", payload}};
  };
  json rm = {{"type", "READ"}, {"request_id", "1"}};
  CHECK(wire::ParseMeta(msg(rm, {})).has_value());
  CHECK(!wire::ParseMeta(json{{"meta", {{"type", "READ"}}}}).has_value());
  CHECK(wire::ParseMeta(json{{"meta", {{"type", "ACK"}}}}).has_value());
  CHECK(!wire::ParseRead(msg(rm, {{"path", "users"}})).has_value());
  CHECK(wire::ParseRead(msg(rm, {{"path", "users/u1"}})).has_value());
  CHECK(!wire::ParseWrite(msg(rm, {{"path", "u/1"}, {"data", 5}})).has_value());
  CHECK(!wire::ParseWrite(msg(rm, {{"path", "u/1"}, {"merge", "yes"},
                                   {"data", json::object()}}))
             .has_value());
  CHECK(wire::ParseWrite(msg(rm, {{"path", "u/1"}, {"data", json::object()}}))
            .has_value());
  CHECK(!wire::ParseQuery(msg(rm, {{"path", "u"}, {"limit", 0}})).has_value());
  CHECK(!wire::ParseQuery(msg(rm, {{"path", "u"}, {"limit", 1.5}})).has_value());
  CHECK(wire::ParseQuery(msg(rm, {{"path", "u"}, {"limit", 10.0}})).has_value());
  CHECK(wire::ParseQuery(msg(rm, {{"path", "u"}, {"limit", nullptr},
                                  {"cursor", nullptr}, {"where", nullptr}}))
            .has_value());
  CHECK(!wire::ParseQuery(msg(rm, {{"path", "u"},
                                   {"where", {{{"field", "a"}, {"op", "EQUAL"}}}}}))
             .has_value()); // value required
  CHECK(wire::ParseQuery(msg(rm, {{"path", "u"},
                                  {"where", {{"field", "a"}, {"op", "EQUAL"},
                                             {"value", nullptr}}}}))
            .has_value()); // single object form, null value
  CHECK(!wire::ParseQuery(msg(rm, {{"path", "u"},
                                   {"orderBy", {{{"field", "a"},
                                                 {"direction", "UP"}}}}}))
             .has_value());
  CHECK(!wire::ParseMutateMany(msg(rm, {{"path", "u"}, {"set", {1, 2}}}))
             .has_value());
  CHECK(!wire::ParseSubscribe(msg(rm, {{"path", "u"}})).has_value());
  CHECK(!wire::ParseClientAck(json{{"meta", {{"type", "ACK"}}},
                                   {"payload", {{"resume_token", "1"}}}})
             .has_value());
  auto err = wire::ErrorMessage("r", DbError::NotFound("x"));
  CHECK(err["meta"]["type"] == "ERROR" && err["meta"]["status"] == 404);
}

