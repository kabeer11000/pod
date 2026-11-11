// Security rules (parsing, matching, authorization through the database) and
// connection authentication.
#include "db/database.h"
#include "db/path.h"
#include "memory_storage.h"
#include "rules/jwt.h"
#include "rules/rules.h"
#include "server/authenticate.h"
#include "test.h"
#include <nlohmann/json.hpp>

using namespace pod;
using json = nlohmann::json;
using rules::Authorizer;
using rules::Op;
using rules::RuleSet;

namespace {

ResourcePath P(const std::string &s) { return *ParsePath(s); }

std::shared_ptr<const RuleSet> Rules(const json &config) {
  auto r = RuleSet::Parse(config);
  if (!r)
    std::cerr << "rules parse error: " << r.error() << std::endl;
  return r ? *r : nullptr;
}

AuthContext User(const std::string &uid, json extra = json::object(),
                 std::optional<int64_t> exp = std::nullopt) {
  AuthContext a;
  a.ns = "ns";
  extra["sub"] = uid;
  a.token = auth::VerifiedToken{uid, extra, exp};
  return a;
}

AuthContext Anonymous() {
  AuthContext a;
  a.ns = "ns";
  return a;
}

std::string ParseError(const json &config) {
  auto r = RuleSet::Parse(config);
  return r ? std::string() : r.error();
}

} // namespace

TEST(TestRulesParseErrors) {
  CHECK(!ParseError(json::array()).empty());
  CHECK(!ParseError({{"rules", 1}}).empty());
  CHECK(!ParseError({{"rules", {{{"match", "a/b"}}}}}).empty()); // no allow
  // Collections are not documents.
  CHECK(ParseError({{"rules", {{{"match", "users"}, {"allow", {{"read", "true"}}}}}}})
            .find("even number") != std::string::npos);
  CHECK(!ParseError({{"rules", {{{"match", "{a=**}/x"}, {"allow", {{"read", "true"}}}}}}})
             .empty());
  CHECK(ParseError({{"rules", {{{"match", "u/{auth}"}, {"allow", {{"read", "true"}}}}}}})
            .find("reserved") != std::string::npos);
  CHECK(ParseError({{"rules", {{{"match", "u/{x}/p/{x}"}, {"allow", {{"read", "true"}}}}}}})
            .find("twice") != std::string::npos);
  CHECK(ParseError({{"rules", {{{"match", "u/{x"}, {"allow", {{"read", "true"}}}}}}})
            .find("unterminated") != std::string::npos);
  CHECK(ParseError({{"rules", {{{"match", "u/{x}"}, {"allow", {{"modify", "true"}}}}}}})
            .find("unknown operation") != std::string::npos);
  CHECK(!ParseError({{"rules", {{{"match", "u/{x}"}, {"allow", {{"read", "1 +"}}}}}}})
             .empty());
  CHECK(!ParseError({{"rules", {{{"match", "u/{x}"}, {"allow", {{"read", true}}}}}}})
             .empty());
  CHECK(ParseError({{"rules", {{{"match", "u/{x}"}, {"allow", {{"read", "true"}}}},
                               {{"match", "p/{r=**}"}, {"allow", {{"write", "false"}}}}}}})
            .empty());
  CHECK(!RuleSet::LoadFile("/nonexistent/rules.json"));
}

TEST(TestRulesMatchPattern) {
  auto set = Rules({{"rules",
                     {{{"match", "a/{x}/b/{y}"}, {"allow", {{"read", "true"}}}},
                      {{"match", "pub/{rest=**}"}, {"allow", {{"read", "true"}}}}}}});
  const auto &two = set->Rules()[0].pattern;
  const auto &rest = set->Rules()[1].pattern;
  auto m = rules::MatchPattern(two, {"a", "1", "b", "2"});
  CHECK(m && (*m)["x"] == "1" && (*m)["y"] == "2");
  CHECK(!rules::MatchPattern(two, {"a", "1", "c", "2"}));
  CHECK(!rules::MatchPattern(two, {"a", "1"}));
  CHECK(!rules::MatchPattern(two, {"a", "1", "b", "2", "c", "3"}));
  auto r = rules::MatchPattern(rest, {"pub", "d", "sub", "e"});
  CHECK(r && (*r)["rest"] == "d/sub/e");
  auto r0 = rules::MatchPattern(rest, {"pub"});
  CHECK(r0 && (*r0)["rest"] == "");
  CHECK(!rules::MatchPattern(rest, {"priv", "d"}));
}

TEST(TestRulesOpenMode) {
  MemoryStorage storage;
  Database db(storage);
  Authorizer open(db, nullptr);
  CHECK(open.OpenMode());
  CHECK(open.IsAdmin(Anonymous()));
  CHECK(open.Allows(Anonymous(), Op::Delete, "a/b", nullptr, nullptr));
  CHECK(!open.MakeGuard(Anonymous()));
  CHECK(!open.MakeVisibility(Anonymous(), Op::List));
}

TEST(TestRulesOwnerOnly) {
  MemoryStorage storage;
  Database db(storage);
  Authorizer authz(db, Rules({{"rules",
                               {{{"match", "users/{userId}"},
                                 {"allow",
                                  {{"read", "auth != null && auth.uid == userId"},
                                   {"create", "auth != null && auth.uid == userId && "
                                              "request.resource.data.owner == auth.uid"},
                                   {"update", "auth.uid == userId && "
                                              "resource.data.owner == "
                                              "request.resource.data.owner"},
                                   {"delete", "auth.token.admin == true"}}}}}}}));
  CHECK(!authz.OpenMode());
  const auto alice = User("alice");
  const auto bob = User("bob");
  CHECK(authz.Allows(alice, Op::Get, "users/alice", nullptr, nullptr));
  CHECK(authz.Allows(alice, Op::List, "users/alice", nullptr, nullptr));
  CHECK(!authz.Allows(bob, Op::Get, "users/alice", nullptr, nullptr));
  CHECK(!authz.Allows(Anonymous(), Op::Get, "users/alice", nullptr, nullptr));
  // No matching rule = denied.
  CHECK(!authz.Allows(alice, Op::Get, "other/alice", nullptr, nullptr));
  CHECK(!authz.Allows(alice, Op::Get, "users/alice/posts/p", nullptr, nullptr));

  // Writes through the database go through the guard: create/update/delete.
  const auto guard = authz.MakeGuard(alice);
  auto ok = db.Write("ns", P("users/alice"), {{"owner", "alice"}}, false, &guard);
  CHECK(ok.has_value());
  auto spoof = db.Write("ns", P("users/x"), {{"owner", "x"}}, false, &guard);
  CHECK(!spoof && spoof.error().code == "PERMISSION_DENIED" &&
        spoof.error().status == 403);
  auto bad_create = db.Write("ns", P("users/bob"), {{"owner", "alice"}}, false,
                             &guard);
  CHECK(!bad_create);
  auto update = db.Write("ns", P("users/alice"), {{"n", 1}}, true, &guard);
  CHECK(update.has_value()); // merge keeps owner
  auto steal = db.Write("ns", P("users/alice"), {{"owner", "bob"}}, true, &guard);
  CHECK(!steal);
  auto del = db.Delete("ns", P("users/alice"), &guard);
  CHECK(!del);
  CHECK(*db.Read("ns", P("users/alice")) != std::nullopt);
  const auto admin_guard = authz.MakeGuard(User("root", {{"admin", true}}));
  CHECK(db.Delete("ns", P("users/alice"), &admin_guard).has_value());
  CHECK(*db.Read("ns", P("users/alice")) == std::nullopt);
}

TEST(TestRulesExpiredTokenIsAnonymous) {
  MemoryStorage storage;
  Database db(storage);
  Authorizer authz(
      db, Rules({{"rules", {{{"match", "d/{id}"}, {"allow", {{"read", "auth != null"}}}}}}}));
  CHECK(authz.Allows(User("u", json::object(), 32503680000), Op::Get, "d/1",
                     nullptr, nullptr));
  CHECK(!authz.Allows(User("u", json::object(), 1000), Op::Get, "d/1", nullptr,
                      nullptr));
  CHECK(!authz.IsAdmin(User("u", {{"admin", true}}, 1000)));
  CHECK(authz.IsAdmin(User("u", {{"admin", true}})));
  CHECK(!authz.IsAdmin(User("u", {{"admin", "yes"}})));
  CHECK(!authz.IsAdmin(Anonymous()));
}

TEST(TestRulesLookupsAndErrors) {
  MemoryStorage storage;
  Database db(storage);
  CHECK(db.Write("ns", P("members/alice"), {{"role", "editor"}}, false));
  CHECK(db.Write("other", P("members/bob"), {{"role", "editor"}}, false));
  Authorizer authz(
      db,
      Rules({{"rules",
              {{{"match", "docs/{id}"},
                {"allow",
                 {{"read", "exists('members/' + auth.uid)"},
                  {"update", "get('members/' + auth.uid).data.role == 'editor'"}}}},
               // Errors (missing field, bad path) evaluate to false.
               {{"match", "err/{id}"},
                {"allow", {{"read", "resource.data.missing == 1"},
                           {"update", "get('not a//path') != null"}}}},
               // More than 10 lookups in one evaluation is an error.
               {{"match", "many/{id}"},
                {"allow", {{"read", "[1,2,3,4,5,6,7,8,9,10,11].all(i, "
                                    "!exists('members/x' + string(i)))"}}}},
               {{"match", "ten/{id}"},
                {"allow", {{"read", "[1,2,3,4,5,6,7,8,9,10].all(i, "
                                    "!exists('members/x' + string(i)))"}}}}}}}));
  CHECK(authz.Allows(User("alice"), Op::Get, "docs/1", nullptr, nullptr));
  // Lookups are confined to the connection's namespace.
  CHECK(!authz.Allows(User("bob"), Op::Get, "docs/1", nullptr, nullptr));
  const json data = {{"t", 1}};
  CHECK(authz.Allows(User("alice"), Op::Update, "docs/1", nullptr, &data));
  CHECK(!authz.Allows(User("bob"), Op::Update, "docs/1", nullptr, &data));
  Document existing{"err/1", "1", json::object(), 1};
  CHECK(!authz.Allows(User("alice"), Op::Get, "err/1", &existing, nullptr));
  CHECK(!authz.Allows(User("alice"), Op::Update, "err/1", &existing, &data));
  CHECK(!authz.Allows(User("alice"), Op::Get, "many/1", nullptr, nullptr));
  CHECK(authz.Allows(User("alice"), Op::Get, "ten/1", nullptr, nullptr));
}

TEST(TestRulesAnyRuleGrants) {
  MemoryStorage storage;
  Database db(storage);
  Authorizer authz(db, Rules({{"rules",
                               {{{"match", "p/{id}"}, {"allow", {{"get", "id == 'a'"}}}},
                                {{"match", "p/{rest=**}"},
                                 {"allow", {{"read", "rest == 'b'"}}}}}}}));
  CHECK(authz.Allows(Anonymous(), Op::Get, "p/a", nullptr, nullptr));
  CHECK(authz.Allows(Anonymous(), Op::Get, "p/b", nullptr, nullptr));
  CHECK(!authz.Allows(Anonymous(), Op::Get, "p/c", nullptr, nullptr));
  // "get" does not grant "list".
  CHECK(!authz.Allows(Anonymous(), Op::List, "p/a", nullptr, nullptr));
  CHECK(authz.Allows(Anonymous(), Op::List, "p/b", nullptr, nullptr));
}

TEST(TestRulesQueryVisibilityAndAtomicGuard) {
  MemoryStorage storage;
  Database db(storage);
  for (const char *id : {"a", "b", "c", "d"})
    CHECK(db.Write("ns", P(std::string("notes/") + id),
                   {{"owner", std::string(id) < "c" ? "alice" : "bob"}, {"n", 0}},
                   false));
  Authorizer authz(db, Rules({{"rules",
                               {{{"match", "notes/{id}"},
                                 {"allow", {{"read", "resource.data.owner == auth.uid"},
                                            {"write", "resource == null || "
                                                      "resource.data.owner == "
                                                      "auth.uid"}}}}}}}));
  const auto alice = User("alice");
  const Visibility visible = authz.MakeVisibility(alice, Op::List);
  auto page = db.Query("ns", P("notes"), QuerySpec{}, std::nullopt, &visible);
  CHECK(page && page->documents.size() == 2);
  for (const auto &d : page->documents)
    CHECK(d.data["owner"] == "alice");

  // MUTATE_MANY touching a forbidden document fails as a whole.
  const ChangeGuard guard = authz.MakeGuard(alice);
  auto r = db.MutateMany("ns", P("notes"), {}, {},
                         {{*ParseFieldPath("n"), json(1)}}, &guard);
  CHECK(!r && r.error().code == "PERMISSION_DENIED");
  for (const char *id : {"a", "b", "c", "d"})
    CHECK((*db.Read("ns", P(std::string("notes/") + id)))->data["n"] == 0);
  // A filter restricted to her documents succeeds.
  auto f = MakeFilter("owner", "EQUAL", "alice");
  CHECK(f.has_value());
  auto ok = db.MutateMany("ns", P("notes"), {*f}, {},
                          {{*ParseFieldPath("n"), json(1)}}, &guard);
  CHECK(ok && ok->affected_count == 2);
  // Collection delete is checked per document.
  CHECK(!db.Delete("ns", P("notes"), &guard));
  CHECK((*db.Query("ns", P("notes"), QuerySpec{}, std::nullopt)).documents.size() == 4);
}

// ---------------------------------------------------------------------------
// Connection authentication

TEST(TestAuthenticateConnection) {
  PodConfig cfg;
  cfg.default_namespace = "dflt";
  const std::string secret = "0123456789abcdef0123456789abcdef";

  auto anon = AuthenticateConnection(cfg, "", "");
  CHECK(anon && anon->ns == "dflt" && !anon->token);
  auto bad_ns = AuthenticateConnection(cfg, "bad ns!", "");
  CHECK(!bad_ns && bad_ns.error().ws_close_code == 4400);
  // A token without a configured secret is rejected rather than ignored.
  auto unconfigured = AuthenticateConnection(
      cfg, "a", auth::SignJwtHs256({{"sub", "u"}}, secret));
  CHECK(!unconfigured && unconfigured.error().ws_close_code == 4401);

  cfg.auth_secret = secret;
  auto ok = AuthenticateConnection(cfg, "a",
                                   auth::SignJwtHs256({{"sub", "u1"}}, secret));
  CHECK(ok && ok->ns == "a" && ok->token && ok->token->uid == "u1");
  auto forged = AuthenticateConnection(
      cfg, "a", auth::SignJwtHs256({{"sub", "u1"}}, "another-secret-value"));
  CHECK(!forged && forged.error().ws_close_code == 4401);
  auto expired = AuthenticateConnection(
      cfg, "a", auth::SignJwtHs256({{"sub", "u1"}, {"exp", 1000}}, secret));
  CHECK(!expired && expired.error().ws_close_code == 4401);

  // "ns" claim restricts namespaces.
  auto ns_ok = AuthenticateConnection(
      cfg, "a", auth::SignJwtHs256({{"sub", "u"}, {"ns", "a"}}, secret));
  CHECK(ns_ok.has_value());
  auto ns_list = AuthenticateConnection(
      cfg, "b", auth::SignJwtHs256({{"sub", "u"}, {"ns", {"a", "b"}}}, secret));
  CHECK(ns_list.has_value());
  auto ns_wrong = AuthenticateConnection(
      cfg, "c", auth::SignJwtHs256({{"sub", "u"}, {"ns", {"a", "b"}}}, secret));
  CHECK(!ns_wrong && ns_wrong.error().ws_close_code == 4403);
  // The default namespace is subject to the claim too.
  auto ns_default = AuthenticateConnection(
      cfg, "", auth::SignJwtHs256({{"sub", "u"}, {"ns", "a"}}, secret));
  CHECK(!ns_default && ns_default.error().ws_close_code == 4403);
  auto ns_bad = AuthenticateConnection(
      cfg, "a", auth::SignJwtHs256({{"sub", "u"}, {"ns", 5}}, secret));
  CHECK(!ns_bad && ns_bad.error().ws_close_code == 4401);

  cfg.require_auth = true;
  auto required = AuthenticateConnection(cfg, "a", "");
  CHECK(!required && required.error().ws_close_code == 4401);
}
