// CEL interpreter tests.
#include "test.h"
#include "rules/cel.h"
#include <cmath>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace pod;

namespace {

struct Env {
  std::map<std::string, json, std::less<>> vars;
  std::map<std::string, cel::Function, std::less<>> fns;
  size_t max_steps = 10000;

  cel::Environment Get() const {
    cel::Environment e;
    e.variable = [this](std::string_view n) -> const json * {
      auto it = vars.find(n);
      return it == vars.end() ? nullptr : &it->second;
    };
    e.function = [this](std::string_view n) -> const cel::Function * {
      auto it = fns.find(n);
      return it == fns.end() ? nullptr : &it->second;
    };
    e.max_steps = max_steps;
    return e;
  }
};

std::expected<json, std::string> Run(std::string_view src,
                                     const Env &env = Env{}) {
  auto p = cel::Compile(src);
  if (!p)
    return std::unexpected("COMPILE: " + p.error());
  return cel::Evaluate(**p, env.Get());
}

// Evaluates and compares with `expected` using exact JSON equality (types
// matter: 1 vs 1.0 vs 1u).
bool Is(std::string_view src, const json &expected, const Env &env = Env{}) {
  auto r = Run(src, env);
  if (!r) {
    std::cerr << "  [" << src << "] error: " << r.error() << std::endl;
    return false;
  }
  const bool same = r->type() == expected.type() && *r == expected;
  if (!same)
    std::cerr << "  [" << src << "] = " << r->dump() << " expected "
              << expected.dump() << std::endl;
  return same;
}

bool IsError(std::string_view src, const Env &env = Env{}) {
  auto r = Run(src, env);
  if (r)
    std::cerr << "  [" << src << "] unexpectedly = " << r->dump() << std::endl;
  return !r.has_value() && r.error().rfind("COMPILE:", 0) != 0;
}

bool CompileFails(std::string_view src) { return !cel::Compile(src).has_value(); }

} // namespace

TEST(TestCelLiterals) {
  CHECK(Is("123", json(int64_t(123))));
  CHECK(Is("0x1F", json(int64_t(31))));
  CHECK(Is("0xFFu", json(uint64_t(255))));
  CHECK(Is("123u", json(uint64_t(123))));
  CHECK(Is("18446744073709551615u", json(std::numeric_limits<uint64_t>::max())));
  CHECK(Is("-9223372036854775808", json(std::numeric_limits<int64_t>::min())));
  CHECK(Is("1.5", json(1.5)));
  CHECK(Is("1e3", json(1000.0)));
  CHECK(Is("2.5e-3", json(0.0025)));
  CHECK(Is(".5", json(0.5)));
  CHECK(Is("true", json(true)));
  CHECK(Is("false", json(false)));
  CHECK(Is("null", json(nullptr)));
  CHECK(Is("'single'", json("single")));
  CHECK(Is("\"double\"", json("double")));
  CHECK(Is(R"('a\'b\"c\\d\ne\rf\tg')", json("a'b\"c\\d\ne\rf\tg")));
  CHECK(Is(R"('é€')", json("é€")));
  CHECK(Is(R"(r'a\nb')", json("a\\nb")));
  CHECK(Is(R"(R"x\y")", json("x\\y")));
  CHECK(Is("[1, 'a', [true]]", json({1, "a", {true}})));
  CHECK(Is("[1, 2,]", json({1, 2})));
  CHECK(Is("[]", json::array()));
  CHECK(Is("{'a': 1, 'b': [2]}", json({{"a", 1}, {"b", {2}}})));
  CHECK(Is("{}", json::object()));
  CHECK(Is("1 // trailing comment", json(int64_t(1))));

  CHECK(CompileFails("9223372036854775808"));     // only valid negated
  CHECK(CompileFails("18446744073709551616u"));
  CHECK(CompileFails("0x"));
  CHECK(CompileFails("0x10000000000000000"));
  CHECK(CompileFails("1e"));
  CHECK(CompileFails("123abc"));
  CHECK(CompileFails("1.5u"));
  CHECK(CompileFails("'unterminated"));
  CHECK(CompileFails(R"('\q')"));
  CHECK(CompileFails(R"('\uD800')"));               // lone surrogate
  CHECK(CompileFails(R"('\u12')"));
  CHECK(CompileFails("a = b"));
  CHECK(CompileFails("a & b"));
  CHECK(CompileFails("#"));
}

TEST(TestCelParseErrors) {
  CHECK(CompileFails(""));
  CHECK(CompileFails("(1 + 2"));
  CHECK(CompileFails("[1, 2"));
  CHECK(CompileFails("{'a': 1"));
  CHECK(CompileFails("1 + 2)"));
  CHECK(CompileFails("1 2"));
  CHECK(CompileFails("a <"));
  CHECK(CompileFails("a < b < c"));
  CHECK(CompileFails("a == b == c"));
  CHECK(CompileFails("a in b in c"));
  CHECK(CompileFails("true ? 1"));
  CHECK(CompileFails("a."));
  CHECK(CompileFails("a.in"));
  CHECK(CompileFails("has(a)"));
  CHECK(CompileFails("has(a.b, c)"));
  CHECK(CompileFails("in"));
  CHECK(CompileFails(std::string(5000, ' ') + "1"));       // too long
  CHECK(CompileFails(std::string(70, '(') + "1" + std::string(70, ')')));
  CHECK(CompileFails(std::string(70, '!') + "true"));
  CHECK(CompileFails(std::string(70, '[') + std::string(70, ']')));
  CHECK(!CompileFails(std::string(30, '(') + "1" + std::string(30, ')')));
  auto err = cel::Compile("1 + )");
  CHECK(!err && err.error().find("offset 4") != std::string::npos);
}

TEST(TestCelPrecedence) {
  CHECK(Is("1 + 2 * 3", json(int64_t(7))));
  CHECK(Is("(1 + 2) * 3", json(int64_t(9))));
  CHECK(Is("10 - 4 - 3", json(int64_t(3))));      // left associative
  CHECK(Is("100 / 10 / 5", json(int64_t(2))));
  CHECK(Is("-2 * 3", json(int64_t(-6))));
  CHECK(Is("--2", json(int64_t(2))));
  CHECK(Is("!!true", json(true)));
  CHECK(Is("1 + 2 == 3", json(true)));
  CHECK(CompileFails("1 < 2 == true")); // relational operators are non-associative
  CHECK(Is("(1 < 2) == true", json(true)));
  CHECK(Is("true || false && false", json(true)));
  CHECK(Is("(true || false) && false", json(false)));
  CHECK(Is("false ? 1 : true ? 2 : 3", json(int64_t(2))));
  CHECK(Is("1 + 1 == 2 ? 'y' : 'n'", json("y")));
  CHECK(Is("{'a': {'b': [10, 20]}}.a.b[1]", json(int64_t(20))));
}

TEST(TestCelArithmetic) {
  CHECK(Is("7 / 2", json(int64_t(3))));
  CHECK(Is("-7 / 2", json(int64_t(-3))));
  CHECK(Is("7 % 3", json(int64_t(1))));
  CHECK(Is("-7 % 3", json(int64_t(-1))));
  CHECK(Is("7.0 / 2.0", json(3.5)));
  CHECK(Is("1 + 0.5", json(1.5)));
  CHECK(Is("3u + 4u", json(int64_t(7))));
  CHECK(Is("3u - 5", json(int64_t(-2))));
  CHECK(Is("'ab' + 'cd'", json("abcd")));
  CHECK(Is("[1] + [2, 3]", json({1, 2, 3})));
  CHECK(Is("-1.5", json(-1.5)));
  CHECK(Is("1.0 / 0.0 > 1e308", json(true)));
  CHECK(Is("9223372036854775807 + 1", json(uint64_t(9223372036854775808ULL))));

  CHECK(IsError("18446744073709551615u + 1"));
  CHECK(IsError("-9223372036854775808 - 1"));
  CHECK(IsError("-(-9223372036854775808)"));
  CHECK(IsError("-18446744073709551615u"));
  CHECK(IsError("4611686018427387904 * 4611686018427387904 * 4"));
  CHECK(IsError("1 / 0"));
  CHECK(IsError("1 % 0"));
  CHECK(IsError("1.5 % 2.0"));
  CHECK(IsError("5 % 2.0"));
  CHECK(IsError("'a' + 1"));
  CHECK(IsError("'a' - 'b'"));
  CHECK(IsError("[1] * 2"));
  CHECK(IsError("-'a'"));
  CHECK(IsError("!1"));
  CHECK(IsError("true + true"));
}

TEST(TestCelComparison) {
  CHECK(Is("1 == 1.0", json(true)));
  CHECK(Is("1u == 1", json(true)));
  CHECK(Is("1 != 2", json(true)));
  CHECK(Is("'a' == 1", json(false)));      // different types: not equal
  CHECK(Is("null == null", json(true)));
  CHECK(Is("null == false", json(false)));
  CHECK(Is("[1, [2]] == [1.0, [2u]]", json(true)));
  CHECK(Is("{'a': 1} == {'a': 1.0}", json(true)));
  CHECK(Is("{'a': 1} == {'a': 1, 'b': 2}", json(false)));
  CHECK(Is("double('NaN') == double('NaN')", json(false)));
  CHECK(Is("double('NaN') < 1.0", json(false)));
  CHECK(Is("1 < 1.5", json(true)));
  CHECK(Is("9007199254740993 > 9007199254740992.0", json(true)));
  CHECK(Is("-1 < 0u", json(true)));
  CHECK(Is("'B' < 'a'", json(true)));
  CHECK(Is("'abc' <= 'abc'", json(true)));
  CHECK(Is("false < true", json(true)));
  CHECK(Is("2 >= 2", json(true)));
  CHECK(Is("3 > 4", json(false)));
  CHECK(IsError("'a' < 1"));
  CHECK(IsError("[1] < [2]"));
  CHECK(IsError("null < null"));
  CHECK(IsError("{} > {}"));
}

TEST(TestCelLogicTruthTable) {
  Env env;
  // `err` is any erroring expression.
  const std::string err = "(1 / 0 == 1)";
  CHECK(Is("false && " + err, json(false)));
  CHECK(Is(err + " && false", json(false)));
  CHECK(IsError("true && " + err));
  CHECK(IsError(err + " && true"));
  CHECK(IsError(err + " && " + err));
  CHECK(Is("true || " + err, json(true)));
  CHECK(Is(err + " || true", json(true)));
  CHECK(IsError("false || " + err));
  CHECK(IsError(err + " || false"));
  CHECK(Is("true && true", json(true)));
  CHECK(Is("true && false", json(false)));
  CHECK(Is("false || false", json(false)));
  // Non-bool operands are errors (absorbable).
  CHECK(IsError("1 && true"));
  CHECK(Is("1 && false", json(false)));
  CHECK(Is("'x' || true", json(true)));
  CHECK(IsError("'x' || false"));
  // Undeclared variable is an error value too.
  CHECK(Is("missing || true", json(true)));
  CHECK(IsError("missing && true"));
  // Ternary: only the chosen branch is evaluated.
  CHECK(Is("true ? 1 : " + err, json(int64_t(1))));
  CHECK(Is("false ? " + err + " : 2", json(int64_t(2))));
  CHECK(IsError("1 ? 2 : 3"));
  CHECK(IsError(err + " ? 1 : 2"));
}

TEST(TestCelMembersAndIn) {
  Env env;
  env.vars["m"] = {{"a", 1}, {"nested", {{"x", "y"}}}, {"list", {10, 20, 30}}};
  env.vars["n"] = nullptr;
  CHECK(Is("m.a", json(1), env));
  CHECK(Is("m.nested.x", json("y"), env));
  CHECK(Is("m['nested']['x']", json("y"), env));
  CHECK(Is("m.list[2]", json(30), env));
  CHECK(Is("m.list[1u]", json(20), env));
  CHECK(IsError("m.list[3]", env));
  CHECK(IsError("m.list[-1]", env));
  CHECK(IsError("m.list['0']", env));
  CHECK(IsError("m.list[1.0]", env));
  CHECK(IsError("m.missing", env));
  CHECK(IsError("m['missing']", env));
  CHECK(IsError("m.a.b", env));
  CHECK(IsError("n.x", env));
  CHECK(IsError("undefined_var", env));
  CHECK(Is("'a' in m", json(true), env));
  CHECK(Is("'zz' in m", json(false), env));
  CHECK(Is("20 in m.list", json(true), env));
  CHECK(Is("20.0 in m.list", json(true), env));
  CHECK(Is("21 in m.list", json(false), env));
  CHECK(Is("[1] in [[1], [2]]", json(true), env));
  CHECK(IsError("1 in m", env));
  CHECK(IsError("1 in 'abc'", env));
  CHECK(Is("has(m.a)", json(true), env));
  CHECK(Is("has(m.zz)", json(false), env));
  CHECK(Is("has(m.nested.x)", json(true), env));
  CHECK(IsError("has(m.a.b)", env));        // m.a is not a map
  CHECK(IsError("has(m.zz.b)", env));       // m.zz is an error
  CHECK(IsError("has(n.x)", env));
  CHECK(IsError("{1: 2}", env));            // non-string map key
  CHECK(IsError("{'a': 1, 'a': 2}", env));  // duplicate key
}

TEST(TestCelMacros) {
  Env env;
  env.vars["xs"] = {1, 2, 3, 4};
  env.vars["x"] = "outer";
  env.vars["m"] = {{"k1", 1}, {"k2", 2}};
  CHECK(Is("xs.all(x, x > 0)", json(true), env));
  CHECK(Is("xs.all(x, x > 1)", json(false), env));
  CHECK(Is("xs.exists(x, x == 3)", json(true), env));
  CHECK(Is("xs.exists(x, x == 9)", json(false), env));
  CHECK(Is("xs.exists_one(x, x > 3)", json(true), env));
  CHECK(Is("xs.exists_one(x, x > 2)", json(false), env));
  CHECK(Is("xs.filter(x, x % 2 == 0)", json({2, 4}), env));
  CHECK(Is("xs.map(x, x * 10)", json({int64_t(10), int64_t(20), int64_t(30), int64_t(40)}), env));
  CHECK(Is("[].all(x, false)", json(true), env));
  CHECK(Is("[].exists(x, true)", json(false), env));
  CHECK(Is("m.all(k, k.startsWith('k'))", json(true), env));
  CHECK(Is("m.map(k, m[k])", json({1, 2}), env));
  // Shadowing: x inside the predicate is the element; outside it's "outer".
  CHECK(Is("xs.map(x, x).size() == 4 && x == 'outer'", json(true), env));
  CHECK(Is("[[1, 2], [3]].map(x, x.map(x, x + 1))", json({{int64_t(2), int64_t(3)}, {int64_t(4)}}), env));
  // Error absorption: all() is false if any element is false, even with errors.
  CHECK(Is("[0, 1].all(x, 1 / x > 5)", json(false), env));
  CHECK(IsError("[0, 1].all(x, 1 / x < 5)", env));
  CHECK(Is("[0, 1].exists(x, 1 / x == 1)", json(true), env));
  CHECK(IsError("[0, 2].exists(x, 1 / x == 1)", env));
  CHECK(IsError("[0, 1].exists_one(x, 1 / x == 1)", env));
  CHECK(IsError("[0, 1].filter(x, 1 / x == 1)", env));
  CHECK(IsError("[1, 2].all(x, x)", env)); // non-bool predicate
  CHECK(IsError("'abc'.all(x, true)", env));
  CHECK(IsError("xs.unknown(x, true)", env));
}

TEST(TestCelBuiltins) {
  CHECK(Is("size('héllo')", json(int64_t(5))));
  CHECK(Is("'héllo'.size()", json(int64_t(5))));
  CHECK(Is("size([1, 2])", json(int64_t(2))));
  CHECK(Is("{'a': 1}.size()", json(int64_t(1))));
  CHECK(IsError("size(1)"));
  CHECK(IsError("size()"));
  CHECK(Is("int('42')", json(int64_t(42))));
  CHECK(Is("int('-42')", json(int64_t(-42))));
  CHECK(Is("int(3.9)", json(int64_t(3))));
  CHECK(Is("int(-3.9)", json(int64_t(-3))));
  CHECK(Is("int(7u)", json(int64_t(7))));
  CHECK(IsError("int('4x')"));
  CHECK(IsError("int('')"));
  CHECK(IsError("int(1e19)"));
  CHECK(IsError("int(double('NaN'))"));
  CHECK(IsError("int(18446744073709551615u)"));
  CHECK(Is("uint(5)", json(uint64_t(5))));
  CHECK(Is("uint('18446744073709551615')", json(std::numeric_limits<uint64_t>::max())));
  CHECK(IsError("uint(-1)"));
  CHECK(IsError("uint(-1.0)"));
  CHECK(Is("double(2)", json(2.0)));
  CHECK(Is("double('2.5')", json(2.5)));
  CHECK(IsError("double('x')"));
  CHECK(Is("string(42)", json("42")));
  CHECK(Is("string(-42)", json("-42")));
  CHECK(Is("string(1.5)", json("1.5")));
  CHECK(Is("string(true)", json("true")));
  CHECK(Is("string('s')", json("s")));
  CHECK(IsError("string(null)"));
  CHECK(IsError("string([])"));
  CHECK(Is("type(null)", json("null_type")));
  CHECK(Is("type(true)", json("bool")));
  CHECK(Is("type(1)", json("int")));
  CHECK(Is("type(18446744073709551615u)", json("uint")));
  CHECK(Is("type(1.0)", json("double")));
  CHECK(Is("type('')", json("string")));
  CHECK(Is("type([])", json("list")));
  CHECK(Is("type({})", json("map")));
  CHECK(Is("'hello'.startsWith('he')", json(true)));
  CHECK(Is("'hello'.endsWith('lo')", json(true)));
  CHECK(Is("'hello'.contains('ell')", json(true)));
  CHECK(Is("'hello'.contains('xyz')", json(false)));
  CHECK(Is("'MiXeD é'.lowerAscii()", json("mixed é")));
  CHECK(Is("'MiXeD'.upperAscii()", json("MIXED")));
  CHECK(Is("'  pad \\t'.trim()", json("pad")));
  CHECK(Is("['a', 'b', 'c'].join(',')", json("a,b,c")));
  CHECK(Is("['a', 'b'].join()", json("ab")));
  CHECK(IsError("['a', 1].join(',')"));
  CHECK(IsError("'x'.startsWith(1)"));
  CHECK(IsError("'x'.startsWith()"));
  CHECK(IsError("1.startsWith('1')") || CompileFails("1.startsWith('1')"));
  CHECK(IsError("'x'.nope()"));
  CHECK(IsError("nope(1)"));
}

TEST(TestCelRegex) {
  CHECK(Is("'user_123'.matches('^user_[0-9]+$')", json(true)));
  CHECK(Is("'xx user_1 yy'.matches('user_[0-9]')", json(true))); // search
  CHECK(Is("'abc'.matches('^b')", json(false)));
  CHECK(IsError("'abc'.matches('(')"));
  CHECK(IsError("'abc'.matches('" + std::string(300, 'a') + "')"));
  Env env;
  env.vars["big"] = std::string(5000, 'a');
  CHECK(IsError("big.matches('a')", env));
  env.vars["ok"] = std::string(4096, 'a');
  CHECK(Is("ok.matches('^a*$')", json(true), env));
}

TEST(TestCelEnvironmentFunctions) {
  Env env;
  env.vars["auth"] = {{"uid", "u1"}};
  int calls = 0;
  env.fns["get"] = [&calls](const std::vector<json> &args)
      -> std::expected<json, std::string> {
    ++calls;
    if (args.size() != 1 || !args[0].is_string())
      return std::unexpected("get() expects a path");
    if (args[0] == "users/u1")
      return json{{"role", "admin"}};
    return json(nullptr);
  };
  CHECK(Is("get('users/' + auth.uid).role == 'admin'", json(true), env));
  CHECK(Is("get('users/none') == null", json(true), env));
  CHECK(IsError("get(1)", env));
  CHECK_EQ(calls, 3);
  // Errors in arguments propagate before the function is called.
  CHECK(IsError("get(missing)", env));
  CHECK_EQ(calls, 3);
  // Built-ins cannot be shadowed by environment functions.
  env.fns["size"] = [](const std::vector<json> &) -> std::expected<json, std::string> {
    return json(-1);
  };
  CHECK(Is("size('ab')", json(int64_t(2)), env));
  // An empty Environment has no variables or functions.
  cel::Environment empty;
  auto p = cel::Compile("x");
  CHECK(p && !cel::Evaluate(**p, empty).has_value());
  auto q = cel::Compile("f()");
  CHECK(q && !cel::Evaluate(**q, empty).has_value());
}

TEST(TestCelBudget) {
  Env env;
  env.max_steps = 10000;
  std::string list = "[";
  for (int i = 0; i < 50; ++i)
    list += std::to_string(i) + ",";
  list += "]";
  // 50 * 50 * 50 inner evaluations far exceed the budget.
  CHECK(IsError(list + ".map(x, " + list + ".map(y, " + list + ".map(z, x + y + z)))", env));
  // Absorption must not hide budget exhaustion...
  const std::string expensive =
      list + ".map(x, " + list + ".map(y, " + list + ".map(z, z))).size() > 0";
  CHECK(IsError(expensive + " || true", env));
  CHECK(IsError(expensive + " && false", env));
  // ...but short-circuiting before the expensive part never spends it.
  CHECK(Is("true || " + expensive, json(true), env));
  CHECK(Is("false && " + expensive, json(false), env));
  // 1 + 2 + 3 visits exactly 5 nodes.
  env.max_steps = 4;
  CHECK(IsError("1 + 2 + 3", env));
  env.max_steps = 5;
  CHECK(Is("1 + 2 + 3", json(int64_t(6)), env));
  // Value growth limits (1 MiB strings).
  env.max_steps = 10000;
  env.vars["half"] = std::string(600 * 1024, 'h');
  CHECK(IsError("half + half", env));
  CHECK(Is("size(half + 'x')", json(int64_t(600 * 1024 + 1)), env));
}

TEST(TestCelConcurrency) {
  auto program = cel::Compile(
      "xs.filter(x, x % 3 == 0).map(x, string(x) + who).join('|')");
  CHECK(program.has_value());
  std::vector<std::string> results(8);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      Env env;
      json xs = json::array();
      for (int i = 0; i < 300; ++i)
        xs.push_back(i);
      env.vars["xs"] = xs;
      env.vars["who"] = "!";
      std::string last;
      for (int k = 0; k < 50; ++k) {
        auto r = cel::Evaluate(**program, env.Get());
        last = r ? r->get<std::string>() : "error: " + r.error();
      }
      results[static_cast<size_t>(t)] = last;
    });
  }
  for (auto &th : threads)
    th.join();
  for (const auto &r : results)
    CHECK_EQ(r, results[0]);
  CHECK(results[0].starts_with("0!|3!|6!"));
}

TEST(TestCelFirestoreStyleRule) {
  auto rule = cel::Compile(
      "auth != null && auth.uid == userId && "
      "request.resource.data.title.size() < 100 && "
      "!('admin' in request.resource.data)");
  CHECK(rule.has_value());
  Env env;
  env.vars["auth"] = {{"uid", "alice"}, {"token", {{"sub", "alice"}}}};
  env.vars["userId"] = "alice";
  env.vars["request"] = {{"resource", {{"data", {{"title", "hello"}}}}}};
  CHECK(cel::EvaluatesTrue(**rule, env.Get()));
  env.vars["userId"] = "bob";
  CHECK(!cel::EvaluatesTrue(**rule, env.Get()));
  env.vars["userId"] = "alice";
  env.vars["request"]["resource"]["data"]["admin"] = true;
  CHECK(!cel::EvaluatesTrue(**rule, env.Get()));
  env.vars["request"]["resource"]["data"].erase("admin");
  env.vars["request"]["resource"]["data"]["title"] = std::string(150, 't');
  CHECK(!cel::EvaluatesTrue(**rule, env.Get()));
  env.vars["auth"] = nullptr; // unauthenticated: short-circuits to false
  auto r = cel::Evaluate(**rule, env.Get());
  CHECK(r && *r == json(false));
  // A non-boolean result is not "true".
  auto num = cel::Compile("1");
  CHECK(num && !cel::EvaluatesTrue(**num, Env{}.Get()));
}
