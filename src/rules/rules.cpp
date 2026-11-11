#include "rules/rules.h"
#include "common/log.h"
#include "common/timestamp.h"
#include "db/path.h"
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

namespace pod::rules {

using json = nlohmann::json;

namespace {

constexpr int kMaxLookups = 10;

const std::set<std::string> kReserved = {
    "auth", "request", "resource", "namespace", "true",
    "false", "null", "in", "get", "exists"};

bool IsIdentifier(std::string_view s) {
  if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) ||
                     s[0] == '_'))
    return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
      return false;
  return true;
}

std::vector<std::string> SplitPath(std::string_view path) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= path.size()) {
    const size_t slash = path.find('/', pos);
    const auto seg = path.substr(
        pos, slash == std::string_view::npos ? std::string_view::npos
                                             : slash - pos);
    if (!seg.empty())
      out.emplace_back(seg);
    if (slash == std::string_view::npos)
      break;
    pos = slash + 1;
  }
  return out;
}

std::expected<std::vector<RuleSet::Segment>, std::string>
ParsePattern(const std::string &match) {
  std::vector<RuleSet::Segment> out;
  std::set<std::string> names;
  const auto parts = SplitPath(match);
  if (parts.empty())
    return std::unexpected("pattern is empty");
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string &p = parts[i];
    if (p.front() == '{') {
      if (p.back() != '}')
        return std::unexpected("unterminated wildcard '" + p + "'");
      std::string inner = p.substr(1, p.size() - 2);
      RuleSet::Segment::Kind kind = RuleSet::Segment::Kind::Single;
      if (inner.ends_with("=**")) {
        inner.resize(inner.size() - 3);
        kind = RuleSet::Segment::Kind::Rest;
        if (i + 1 != parts.size())
          return std::unexpected("{" + inner +
                                 "=**} must be the last segment");
      }
      if (!IsIdentifier(inner))
        return std::unexpected("invalid wildcard name '" + inner + "'");
      if (kReserved.contains(inner))
        return std::unexpected("wildcard name '" + inner + "' is reserved");
      if (!names.insert(inner).second)
        return std::unexpected("wildcard '" + inner + "' used twice");
      out.push_back({kind, inner});
    } else {
      if (p.find_first_of("{}") != std::string::npos)
        return std::unexpected("invalid segment '" + p + "'");
      auto check = ParsePath(p);
      if (!check)
        return std::unexpected("invalid segment '" + p +
                               "': " + check.error().message);
      out.push_back({RuleSet::Segment::Kind::Literal, p});
    }
  }
  const bool has_rest =
      out.back().kind == RuleSet::Segment::Kind::Rest;
  if (!has_rest && out.size() % 2 != 0)
    return std::unexpected(
        "pattern must describe documents (an even number of segments) "
        "unless it ends with {name=**}");
  return out;
}

json DocumentValue(const Document &d) {
  return json{{"id", d.id},
              {"path", d.path},
              {"data", d.data},
              {"version", d.version}};
}

} // namespace

const char *OpName(Op op) {
  switch (op) {
  case Op::Get:
    return "get";
  case Op::List:
    return "list";
  case Op::Create:
    return "create";
  case Op::Update:
    return "update";
  case Op::Delete:
    return "delete";
  }
  return "?";
}

std::expected<std::shared_ptr<const RuleSet>, std::string>
RuleSet::Parse(const json &config) {
  if (!config.is_object() || !config.contains("rules") ||
      !config["rules"].is_array())
    return std::unexpected("rules config must be {\"rules\": [...]}");
  auto set = std::make_shared<RuleSet>();
  size_t index = 0;
  for (const auto &r : config["rules"]) {
    const std::string where = "rules[" + std::to_string(index++) + "]";
    if (!r.is_object() || !r.contains("match") || !r["match"].is_string() ||
        !r.contains("allow") || !r["allow"].is_object())
      return std::unexpected(where +
                             " must be {\"match\": ..., \"allow\": {...}}");
    Rule rule;
    rule.match = r["match"].get<std::string>();
    auto pattern = ParsePattern(rule.match);
    if (!pattern)
      return std::unexpected(where + ".match: " + pattern.error());
    rule.pattern = std::move(*pattern);
    for (auto it = r["allow"].begin(); it != r["allow"].end(); ++it) {
      if (!it.value().is_string())
        return std::unexpected(where + ".allow." + it.key() +
                               " must be a string expression");
      auto program = cel::Compile(it.value().get<std::string>());
      if (!program)
        return std::unexpected(where + ".allow." + it.key() + ": " +
                               program.error());
      std::vector<Op> ops;
      const std::string &k = it.key();
      if (k == "read")
        ops = {Op::Get, Op::List};
      else if (k == "write")
        ops = {Op::Create, Op::Update, Op::Delete};
      else if (k == "get")
        ops = {Op::Get};
      else if (k == "list")
        ops = {Op::List};
      else if (k == "create")
        ops = {Op::Create};
      else if (k == "update")
        ops = {Op::Update};
      else if (k == "delete")
        ops = {Op::Delete};
      else
        return std::unexpected(where + ".allow: unknown operation '" + k +
                               "'");
      for (Op op : ops)
        rule.conditions[static_cast<int>(op)].push_back(*program);
    }
    set->_rules.push_back(std::move(rule));
  }
  return std::shared_ptr<const RuleSet>(std::move(set));
}

std::expected<std::shared_ptr<const RuleSet>, std::string>
RuleSet::LoadFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::unexpected("cannot open rules file '" + path + "'");
  std::stringstream ss;
  ss << in.rdbuf();
  json config;
  try {
    config = json::parse(ss.str());
  } catch (const std::exception &e) {
    return std::unexpected("rules file is not valid JSON: " +
                           std::string(e.what()));
  }
  return Parse(config);
}

std::optional<json> MatchPattern(const std::vector<RuleSet::Segment> &pattern,
                                 const std::vector<std::string> &segments) {
  json bindings = json::object();
  size_t i = 0;
  for (; i < pattern.size(); ++i) {
    const auto &p = pattern[i];
    if (p.kind == RuleSet::Segment::Kind::Rest) {
      std::string rest;
      for (size_t k = i; k < segments.size(); ++k) {
        if (k > i)
          rest += '/';
        rest += segments[k];
      }
      bindings[p.text] = rest;
      return bindings;
    }
    if (i >= segments.size())
      return std::nullopt;
    if (p.kind == RuleSet::Segment::Kind::Literal) {
      if (p.text != segments[i])
        return std::nullopt;
    } else {
      bindings[p.text] = segments[i];
    }
  }
  if (i != segments.size())
    return std::nullopt;
  return bindings;
}

Authorizer::Authorizer(Database &db, std::shared_ptr<const RuleSet> rules)
    : _db(db), _rules(std::move(rules)) {}

bool Authorizer::IsAdmin(const AuthContext &auth) const {
  if (OpenMode())
    return true;
  const int64_t now_s = current_unix_millis() / 1000;
  if (!auth.token || auth.Expired(now_s))
    return false;
  const auto &claims = auth.token->claims;
  return claims.is_object() && claims.contains("admin") &&
         claims["admin"] == true;
}

bool Authorizer::Allows(const AuthContext &auth, Op op,
                        const std::string &path, const Document *resource,
                        const json *new_data) const {
  if (OpenMode())
    return true;
  const int64_t now_ms = current_unix_millis();
  const auto segments = SplitPath(path);

  // An expired token is treated as anonymous.
  json vars = json::object();
  if (auth.token && !auth.Expired(now_ms / 1000))
    vars["auth"] = json{{"uid", auth.token->uid},
                        {"token", auth.token->claims}};
  else
    vars["auth"] = nullptr;
  vars["request"] = json{{"op", OpName(op)},
                         {"path", path},
                         {"time", now_ms},
                         {"resource", new_data ? json{{"data", *new_data}}
                                               : json(nullptr)}};
  vars["resource"] = resource ? DocumentValue(*resource) : json(nullptr);
  vars["namespace"] = auth.ns;

  int lookups = 0;
  auto lookup = [&](const std::vector<json> &args)
      -> std::expected<std::optional<Document>, std::string> {
    if (args.size() != 1 || !args[0].is_string())
      return std::unexpected("expects one document path string");
    if (++lookups > kMaxLookups)
      return std::unexpected("too many document lookups");
    auto p = ParseDocumentPath(args[0].get<std::string>());
    if (!p)
      return std::unexpected(p.error().message);
    auto d = _db.Read(auth.ns, *p);
    if (!d)
      return std::unexpected(d.error().message);
    return *d;
  };
  const cel::Function get_fn =
      [&](const std::vector<json> &args) -> std::expected<json, std::string> {
    auto d = lookup(args);
    if (!d)
      return std::unexpected(d.error());
    return *d ? DocumentValue(**d) : json(nullptr);
  };
  const cel::Function exists_fn =
      [&](const std::vector<json> &args) -> std::expected<json, std::string> {
    auto d = lookup(args);
    if (!d)
      return std::unexpected(d.error());
    return json(d->has_value());
  };

  for (const auto &rule : _rules->Rules()) {
    const auto &conds = rule.conditions[static_cast<int>(op)];
    if (conds.empty())
      continue;
    auto bindings = MatchPattern(rule.pattern, segments);
    if (!bindings)
      continue;
    cel::Environment env;
    env.variable = [&](std::string_view name) -> const json * {
      if (auto it = vars.find(name); it != vars.end())
        return &*it;
      if (auto it = bindings->find(name); it != bindings->end())
        return &*it;
      return nullptr;
    };
    env.function = [&](std::string_view name) -> const cel::Function * {
      if (name == "get")
        return &get_fn;
      if (name == "exists")
        return &exists_fn;
      return nullptr;
    };
    for (const auto &program : conds)
      if (cel::EvaluatesTrue(*program, env))
        return true;
  }
  return false;
}

Visibility Authorizer::MakeVisibility(const AuthContext &auth, Op op) const {
  if (OpenMode())
    return {};
  return [this, auth, op](const Document &d) {
    return Allows(auth, op, d.path, &d, nullptr);
  };
}

ChangeGuard Authorizer::MakeGuard(const AuthContext &auth) const {
  if (OpenMode())
    return {};
  return [this, auth](const ChangeRecord &c) -> std::expected<void, DbError> {
    const Op op = !c.before ? Op::Create : !c.after ? Op::Delete : Op::Update;
    const Document *resource = c.before ? &*c.before : nullptr;
    const json *data = c.after ? &c.after->data : nullptr;
    if (!Allows(auth, op, c.path, resource, data))
      return std::unexpected(DbError::PermissionDenied(
          std::string("Missing permission to ") + OpName(op) + " '" +
          c.path + "'."));
    return {};
  };
}

} // namespace pod::rules
