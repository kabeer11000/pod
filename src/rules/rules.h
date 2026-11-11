// Security rules: path-matched CEL conditions deciding who may read and
// write which documents.
//
// Rules file (JSON, POD_RULES_PATH):
//   {
//     "rules": [
//       { "match": "users/{userId}",
//         "allow": { "read": "auth != null && auth.uid == userId",
//                    "write": "auth != null && auth.uid == userId" } },
//       { "match": "public/{rest=**}", "allow": { "read": "true" } }
//     ]
//   }
//
// "match" is a document path pattern: literal segments, {name} (exactly one
// segment) and a trailing {name=**} (zero or more segments, bound as the
// remaining path). "allow" keys: read (= get + list), write (= create +
// update + delete), or the individual operations. An operation is permitted
// if ANY matching rule's condition evaluates to exactly true; evaluation
// errors count as false. No matching rule = denied.
//
// Variables available to conditions:
//   auth       null, or { uid, token: <all JWT claims> }
//   request    { op: "get"|"list"|"create"|"update"|"delete", path, time
//                (unix ms), resource: null | { data } (the new data) }
//   resource   null, or the existing document { id, path, data, version }
//   namespace  the connection's namespace
//   <name>     every wildcard bound by "match"
// Functions: get(path) -> document map or null, exists(path) -> bool
// (paths are document paths in the same namespace; at most 10 lookups per
// evaluation), plus all CEL built-ins.
//
// Without a rules file the server runs in open mode: everything is allowed.
#pragma once
#include "db/database.h"
#include "db/error.h"
#include "rules/auth_context.h"
#include "rules/cel.h"
#include <expected>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pod::rules {

enum class Op { Get, List, Create, Update, Delete };
const char *OpName(Op op);

class RuleSet {
public:
  static std::expected<std::shared_ptr<const RuleSet>, std::string>
  Parse(const nlohmann::json &config);
  static std::expected<std::shared_ptr<const RuleSet>, std::string>
  LoadFile(const std::string &path);

  struct Segment {
    enum class Kind { Literal, Single, Rest } kind;
    std::string text; // literal text or variable name
  };
  struct Rule {
    std::string match;
    std::vector<Segment> pattern;
    // Indexed by Op; several keys (e.g. "read" and "get") may grant one op.
    std::vector<std::shared_ptr<const cel::Program>> conditions[5];
  };
  const std::vector<Rule> &Rules() const { return _rules; }

private:
  std::vector<Rule> _rules;
};

// Binds wildcards of `pattern` against a document path; nullopt if no match.
std::optional<nlohmann::json>
MatchPattern(const std::vector<RuleSet::Segment> &pattern,
             const std::vector<std::string> &segments);

class Authorizer {
public:
  // rules == nullptr runs in open mode.
  Authorizer(Database &db, std::shared_ptr<const RuleSet> rules);

  bool OpenMode() const { return !_rules; }
  // Index management and other administrative operations.
  bool IsAdmin(const AuthContext &auth) const;

  // `doc` is the existing document (nullopt if it does not exist).
  bool Allows(const AuthContext &auth, Op op, const std::string &path,
              const Document *resource,
              const nlohmann::json *new_data) const;

  // Read filter for queries/subscriptions (op = Get or List).
  Visibility MakeVisibility(const AuthContext &auth, Op op) const;
  // Write check for commits; maps changes to create/update/delete.
  ChangeGuard MakeGuard(const AuthContext &auth) const;

private:
  Database &_db;
  std::shared_ptr<const RuleSet> _rules;
};

} // namespace pod::rules
