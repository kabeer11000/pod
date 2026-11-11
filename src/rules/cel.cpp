#include "rules/cel.h"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <regex>
#include <unordered_set>

namespace pod::cel {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxSourceBytes = 4096;
constexpr int kMaxDepth = 64;
constexpr size_t kMaxRegexPattern = 256;
constexpr size_t kMaxRegexInput = 4096;
constexpr size_t kMaxStringBytes = 1 << 20;
constexpr size_t kMaxListSize = 100000;

// ===========================================================================
// Lexer
// ===========================================================================

enum class Tok { End, Int, Uint, Double, String, Ident, Punct };

struct Token {
  Tok kind = Tok::End;
  size_t pos = 0;
  std::string text; // identifier / punctuation / decoded string
  int64_t i = 0;
  uint64_t u = 0;
  double d = 0;
  bool int_min = false; // decimal literal 9223372036854775808 (needs unary -)
};

struct ParseError {
  size_t pos;
  std::string message;
};

[[noreturn]] void Fail(size_t pos, std::string message) {
  throw ParseError{pos, std::move(message)};
}

bool IsIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
int HexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

void AppendUtf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

class Lexer {
public:
  explicit Lexer(std::string_view src) : _s(src) {}

  std::vector<Token> Run() {
    std::vector<Token> out;
    while (true) {
      SkipSpace();
      if (_p >= _s.size()) {
        out.push_back(Token{Tok::End, _p, "", 0, 0, 0, false});
        return out;
      }
      out.push_back(Next());
    }
  }

private:
  void SkipSpace() {
    while (_p < _s.size()) {
      const char c = _s[_p];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++_p;
      } else if (c == '/' && _p + 1 < _s.size() && _s[_p + 1] == '/') {
        while (_p < _s.size() && _s[_p] != '\n')
          ++_p;
      } else {
        break;
      }
    }
  }

  Token Next() {
    const size_t start = _p;
    const char c = _s[_p];
    if ((c == 'r' || c == 'R') && _p + 1 < _s.size() &&
        (_s[_p + 1] == '\'' || _s[_p + 1] == '"')) {
      ++_p;
      return String(start, true);
    }
    if (c == '\'' || c == '"')
      return String(start, false);
    if (IsDigit(c) || (c == '.' && _p + 1 < _s.size() && IsDigit(_s[_p + 1])))
      return Number(start);
    if (IsIdentStart(c)) {
      while (_p < _s.size() && IsIdentChar(_s[_p]))
        ++_p;
      Token t;
      t.kind = Tok::Ident;
      t.pos = start;
      t.text = std::string(_s.substr(start, _p - start));
      return t;
    }
    static const char *kTwo[] = {"==", "!=", "<=", ">=", "&&", "||"};
    for (const char *op : kTwo) {
      if (_s.substr(_p, 2) == op) {
        _p += 2;
        return Punct(start, op);
      }
    }
    static const std::string_view kOne = "<>+-*/%!?:.,()[]{}";
    if (kOne.find(c) != std::string_view::npos) {
      ++_p;
      return Punct(start, std::string(1, c));
    }
    Fail(start, std::string("unexpected character '") + c + "'");
  }

  static Token Punct(size_t pos, std::string text) {
    Token t;
    t.kind = Tok::Punct;
    t.pos = pos;
    t.text = std::move(text);
    return t;
  }

  Token Number(size_t start) {
    Token t;
    t.pos = start;
    if (_s[_p] == '0' && _p + 1 < _s.size() && (_s[_p + 1] == 'x' || _s[_p + 1] == 'X')) {
      _p += 2;
      const size_t digits = _p;
      uint64_t v = 0;
      while (_p < _s.size() && HexValue(_s[_p]) >= 0) {
        if (v > (std::numeric_limits<uint64_t>::max() >> 4))
          Fail(start, "integer literal out of range");
        v = (v << 4) | static_cast<uint64_t>(HexValue(_s[_p]));
        ++_p;
      }
      if (_p == digits)
        Fail(start, "invalid hex literal");
      return Integer(t, v, start);
    }
    while (_p < _s.size() && IsDigit(_s[_p]))
      ++_p;
    bool is_double = false;
    if (_p < _s.size() && _s[_p] == '.' && _p + 1 < _s.size() && IsDigit(_s[_p + 1])) {
      is_double = true;
      ++_p;
      while (_p < _s.size() && IsDigit(_s[_p]))
        ++_p;
    }
    if (_p < _s.size() && (_s[_p] == 'e' || _s[_p] == 'E')) {
      size_t q = _p + 1;
      if (q < _s.size() && (_s[q] == '+' || _s[q] == '-'))
        ++q;
      if (q >= _s.size() || !IsDigit(_s[q]))
        Fail(start, "invalid exponent in numeric literal");
      is_double = true;
      _p = q;
      while (_p < _s.size() && IsDigit(_s[_p]))
        ++_p;
    }
    const std::string_view text = _s.substr(start, _p - start);
    if (is_double) {
      double d = 0;
      auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), d);
      if (ec != std::errc() || ptr != text.data() + text.size())
        Fail(start, "invalid double literal");
      if (_p < _s.size() && (IsIdentChar(_s[_p]) || _s[_p] == '.'))
        Fail(start, "invalid numeric literal");
      t.kind = Tok::Double;
      t.d = d;
      return t;
    }
    uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size())
      Fail(start, "integer literal out of range");
    return Integer(t, v, start);
  }

  Token Integer(Token t, uint64_t v, size_t start) {
    if (_p < _s.size() && (_s[_p] == 'u' || _s[_p] == 'U')) {
      ++_p;
      if (_p < _s.size() && IsIdentChar(_s[_p]))
        Fail(start, "invalid numeric literal");
      t.kind = Tok::Uint;
      t.u = v;
      return t;
    }
    if (_p < _s.size() && IsIdentChar(_s[_p]))
      Fail(start, "invalid numeric literal");
    t.kind = Tok::Int;
    if (v <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      t.i = static_cast<int64_t>(v);
    } else if (v == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1) {
      t.int_min = true;
      t.i = std::numeric_limits<int64_t>::min();
    } else {
      Fail(start, "integer literal out of range");
    }
    return t;
  }

  Token String(size_t start, bool raw) {
    const char quote = _s[_p];
    ++_p;
    std::string out;
    while (true) {
      if (_p >= _s.size())
        Fail(start, "unterminated string literal");
      const char c = _s[_p];
      if (c == quote) {
        ++_p;
        break;
      }
      if (c == '\\' && !raw) {
        if (_p + 1 >= _s.size())
          Fail(start, "unterminated string literal");
        const char e = _s[_p + 1];
        _p += 2;
        switch (e) {
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case '\'': out += '\''; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (_p + 4 > _s.size())
            Fail(_p - 2, "invalid \\u escape");
          uint32_t cp = 0;
          for (int k = 0; k < 4; ++k) {
            const int h = HexValue(_s[_p + static_cast<size_t>(k)]);
            if (h < 0)
              Fail(_p - 2, "invalid \\u escape");
            cp = (cp << 4) | static_cast<uint32_t>(h);
          }
          if (cp >= 0xD800 && cp <= 0xDFFF)
            Fail(_p - 2, "\\u escape may not encode a surrogate");
          _p += 4;
          AppendUtf8(out, cp);
          break;
        }
        default:
          Fail(_p - 2, std::string("invalid escape sequence '\\") + e + "'");
        }
        continue;
      }
      out += c;
      ++_p;
    }
    Token t;
    t.kind = Tok::String;
    t.pos = start;
    t.text = std::move(out);
    return t;
  }

  std::string_view _s;
  size_t _p = 0;
};

// ===========================================================================
// AST
// ===========================================================================

enum class K {
  Lit,
  Ident,
  Select,
  Index,
  Call,
  List,
  Map,
  Neg,
  Not,
  Bin,
  And,
  Or,
  Cond,
  Has,
  Macro
};

struct Node {
  K kind;
  size_t pos = 0;
  json lit;
  std::string name; // ident, field, function, macro name
  std::string op;   // binary operator, or macro variable
  bool method = false;
  std::vector<std::unique_ptr<Node>> c;
};
using NodeP = std::unique_ptr<Node>;

NodeP Make(K kind, size_t pos) {
  auto n = std::make_unique<Node>();
  n->kind = kind;
  n->pos = pos;
  return n;
}

const std::unordered_set<std::string> kMacros = {"all", "exists", "exists_one",
                                                 "filter", "map"};
const std::unordered_set<std::string> kReserved = {"true", "false", "null",
                                                   "in"};

class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : _t(std::move(tokens)) {}

  NodeP Parse() {
    NodeP e = Expr();
    if (Peek().kind != Tok::End)
      Fail(Peek().pos, "unexpected token '" + Describe(Peek()) + "'");
    return e;
  }

private:
  struct Depth {
    explicit Depth(Parser &p) : _p(p) {
      if (++_p._depth > kMaxDepth)
        Fail(_p.Peek().pos, "expression nested too deeply");
    }
    ~Depth() { --_p._depth; }
    Parser &_p;
  };

  const Token &Peek() const { return _t[_i]; }
  const Token &Advance() { return _t[_i++]; }
  bool IsPunct(std::string_view p) const {
    return Peek().kind == Tok::Punct && Peek().text == p;
  }
  bool IsIdent(std::string_view p) const {
    return Peek().kind == Tok::Ident && Peek().text == p;
  }
  void Expect(std::string_view p) {
    if (!IsPunct(p))
      Fail(Peek().pos, "expected '" + std::string(p) + "' but found '" +
                           Describe(Peek()) + "'");
    ++_i;
  }
  static std::string Describe(const Token &t) {
    switch (t.kind) {
    case Tok::End:
      return "end of input";
    case Tok::String:
      return "string literal";
    case Tok::Int:
    case Tok::Uint:
    case Tok::Double:
      return "number";
    default:
      return t.text;
    }
  }

  NodeP Expr() {
    Depth d(*this);
    NodeP cond = Or();
    if (IsPunct("?")) {
      const size_t pos = Advance().pos;
      NodeP a = Expr();
      Expect(":");
      NodeP b = Expr();
      NodeP n = Make(K::Cond, pos);
      n->c.push_back(std::move(cond));
      n->c.push_back(std::move(a));
      n->c.push_back(std::move(b));
      return n;
    }
    return cond;
  }

  NodeP Or() {
    NodeP left = And();
    while (IsPunct("||")) {
      const size_t pos = Advance().pos;
      NodeP n = Make(K::Or, pos);
      n->c.push_back(std::move(left));
      n->c.push_back(And());
      left = std::move(n);
    }
    return left;
  }

  NodeP And() {
    NodeP left = Rel();
    while (IsPunct("&&")) {
      const size_t pos = Advance().pos;
      NodeP n = Make(K::And, pos);
      n->c.push_back(std::move(left));
      n->c.push_back(Rel());
      left = std::move(n);
    }
    return left;
  }

  bool IsRelOp() const {
    if (Peek().kind == Tok::Ident && Peek().text == "in")
      return true;
    if (Peek().kind != Tok::Punct)
      return false;
    const auto &t = Peek().text;
    return t == "==" || t == "!=" || t == "<" || t == "<=" || t == ">" ||
           t == ">=";
  }

  NodeP Rel() {
    NodeP left = Add();
    if (IsRelOp()) {
      const Token &op = Advance();
      NodeP n = Make(K::Bin, op.pos);
      n->op = op.text;
      n->c.push_back(std::move(left));
      n->c.push_back(Add());
      if (IsRelOp())
        Fail(Peek().pos, "relational operators are non-associative; use "
                         "parentheses");
      return n;
    }
    return left;
  }

  NodeP Add() {
    NodeP left = Mul();
    while (IsPunct("+") || IsPunct("-")) {
      const Token &op = Advance();
      NodeP n = Make(K::Bin, op.pos);
      n->op = op.text;
      n->c.push_back(std::move(left));
      n->c.push_back(Mul());
      left = std::move(n);
    }
    return left;
  }

  NodeP Mul() {
    NodeP left = Unary();
    while (IsPunct("*") || IsPunct("/") || IsPunct("%")) {
      const Token &op = Advance();
      NodeP n = Make(K::Bin, op.pos);
      n->op = op.text;
      n->c.push_back(std::move(left));
      n->c.push_back(Unary());
      left = std::move(n);
    }
    return left;
  }

  NodeP Unary() {
    Depth d(*this);
    if (IsPunct("!")) {
      const size_t pos = Advance().pos;
      NodeP n = Make(K::Not, pos);
      n->c.push_back(Unary());
      return n;
    }
    if (IsPunct("-")) {
      const size_t pos = Advance().pos;
      // -9223372036854775808 is the only way to spell INT64_MIN.
      if (Peek().kind == Tok::Int && Peek().int_min) {
        Advance();
        NodeP lit = Make(K::Lit, pos);
        lit->lit = std::numeric_limits<int64_t>::min();
        return Member(std::move(lit));
      }
      NodeP n = Make(K::Neg, pos);
      n->c.push_back(Unary());
      return n;
    }
    return Member(Primary());
  }

  std::vector<NodeP> Args(std::string_view close) {
    std::vector<NodeP> args;
    if (IsPunct(close)) {
      ++_i;
      return args;
    }
    while (true) {
      args.push_back(Expr());
      if (IsPunct(",")) {
        ++_i;
        if (IsPunct(close)) { // trailing comma
          ++_i;
          return args;
        }
        continue;
      }
      Expect(close);
      return args;
    }
  }

  NodeP Member(NodeP operand) {
    while (true) {
      if (IsPunct(".")) {
        const size_t pos = Advance().pos;
        if (Peek().kind != Tok::Ident || kReserved.contains(Peek().text))
          Fail(Peek().pos, "expected field name after '.'");
        std::string name = Advance().text;
        if (IsPunct("(")) {
          ++_i;
          std::vector<NodeP> args = Args(")");
          if (kMacros.contains(name) && args.size() == 2 &&
              args[0]->kind == K::Ident) {
            NodeP m = Make(K::Macro, pos);
            m->name = name;
            m->op = args[0]->name;
            m->c.push_back(std::move(operand));
            m->c.push_back(std::move(args[1]));
            operand = std::move(m);
          } else {
            NodeP call = Make(K::Call, pos);
            call->name = name;
            call->method = true;
            call->c.push_back(std::move(operand));
            for (auto &a : args)
              call->c.push_back(std::move(a));
            operand = std::move(call);
          }
        } else {
          NodeP sel = Make(K::Select, pos);
          sel->name = std::move(name);
          sel->c.push_back(std::move(operand));
          operand = std::move(sel);
        }
      } else if (IsPunct("[")) {
        const size_t pos = Advance().pos;
        NodeP idx = Make(K::Index, pos);
        idx->c.push_back(std::move(operand));
        idx->c.push_back(Expr());
        Expect("]");
        operand = std::move(idx);
      } else {
        return operand;
      }
    }
  }

  NodeP Primary() {
    const Token &t = Peek();
    switch (t.kind) {
    case Tok::Int: {
      if (t.int_min)
        Fail(t.pos, "integer literal out of range");
      NodeP n = Make(K::Lit, t.pos);
      n->lit = t.i;
      ++_i;
      return n;
    }
    case Tok::Uint: {
      NodeP n = Make(K::Lit, t.pos);
      n->lit = t.u;
      ++_i;
      return n;
    }
    case Tok::Double: {
      NodeP n = Make(K::Lit, t.pos);
      n->lit = t.d;
      ++_i;
      return n;
    }
    case Tok::String: {
      NodeP n = Make(K::Lit, t.pos);
      n->lit = t.text;
      ++_i;
      return n;
    }
    case Tok::End:
      Fail(t.pos, "unexpected end of input");
    case Tok::Ident: {
      const size_t pos = t.pos;
      std::string name = Advance().text;
      if (name == "true" || name == "false") {
        NodeP n = Make(K::Lit, pos);
        n->lit = (name == "true");
        return n;
      }
      if (name == "null") {
        NodeP n = Make(K::Lit, pos);
        n->lit = nullptr;
        return n;
      }
      if (name == "in")
        Fail(pos, "unexpected 'in'");
      if (IsPunct("(")) {
        ++_i;
        std::vector<NodeP> args = Args(")");
        if (name == "has") {
          if (args.size() != 1 || args[0]->kind != K::Select)
            Fail(pos, "has() requires a single field selection argument");
          NodeP h = Make(K::Has, pos);
          h->name = args[0]->name;
          h->c.push_back(std::move(args[0]->c[0]));
          return h;
        }
        NodeP call = Make(K::Call, pos);
        call->name = std::move(name);
        for (auto &a : args)
          call->c.push_back(std::move(a));
        return call;
      }
      NodeP n = Make(K::Ident, pos);
      n->name = std::move(name);
      return n;
    }
    case Tok::Punct:
      break;
    }
    if (IsPunct("(")) {
      ++_i;
      NodeP e = Expr();
      Expect(")");
      return e;
    }
    if (IsPunct("[")) {
      const size_t pos = Advance().pos;
      NodeP list = Make(K::List, pos);
      list->c = Args("]");
      return list;
    }
    if (IsPunct("{")) {
      const size_t pos = Advance().pos;
      Depth d(*this);
      NodeP map = Make(K::Map, pos);
      if (IsPunct("}")) {
        ++_i;
        return map;
      }
      while (true) {
        map->c.push_back(Expr());
        Expect(":");
        map->c.push_back(Expr());
        if (IsPunct(",")) {
          ++_i;
          if (IsPunct("}")) {
            ++_i;
            return map;
          }
          continue;
        }
        Expect("}");
        return map;
      }
    }
    Fail(t.pos, "unexpected token '" + Describe(t) + "'");
  }

  std::vector<Token> _t;
  size_t _i = 0;
  int _depth = 0;
};

// ===========================================================================
// Values
// ===========================================================================

// A value either borrowed (pointer into data that outlives the evaluation
// step) or owned.
class V {
public:
  explicit V(const json *ref) : _ref(ref) {}
  explicit V(json value) : _own(std::move(value)), _ref(nullptr) {}
  const json &operator*() const { return _ref ? *_ref : _own; }
  const json *operator->() const { return &**this; }
  bool Borrowed() const { return _ref != nullptr; }
  json Take() && { return _ref ? *_ref : std::move(_own); }

private:
  json _own;
  const json *_ref;
};

using R = std::expected<V, std::string>;

R Err(std::string message) { return std::unexpected(std::move(message)); }
R Ok(json v) { return V(std::move(v)); }

using I128 = __int128;

bool IsInt(const json &v) { return v.is_number_integer(); } // incl. unsigned
I128 ToI128(const json &v) {
  return v.is_number_unsigned() ? static_cast<I128>(v.get<uint64_t>())
                                : static_cast<I128>(v.get<int64_t>());
}
R FromI128(I128 x) {
  if (x >= std::numeric_limits<int64_t>::min() &&
      x <= std::numeric_limits<int64_t>::max())
    return Ok(json(static_cast<int64_t>(x)));
  if (x >= 0 && x <= static_cast<I128>(std::numeric_limits<uint64_t>::max()))
    return Ok(json(static_cast<uint64_t>(x)));
  return Err("integer overflow");
}
double ToDouble(const json &v) {
  return IsInt(v) ? static_cast<double>(ToI128(v)) : v.get<double>();
}

int Sign(I128 a, I128 b) { return a < b ? -1 : (a > b ? 1 : 0); }

// Exact comparison of an integer with a non-NaN double.
int CmpIntDouble(I128 i, double d) {
  constexpr double kTwo127 = 170141183460469231731687303715884105728.0;
  if (d >= kTwo127)
    return -1;
  if (d < -kTwo127)
    return 1;
  const double t = std::trunc(d);
  const I128 ti = static_cast<I128>(t);
  if (const int c = Sign(i, ti); c != 0)
    return c;
  const double frac = d - t;
  return frac > 0 ? -1 : (frac < 0 ? 1 : 0);
}

// nullopt when either side is NaN (unordered).
std::optional<int> CompareNumbers(const json &a, const json &b) {
  if (IsInt(a) && IsInt(b))
    return Sign(ToI128(a), ToI128(b));
  if (!IsInt(a) && !IsInt(b)) {
    const double x = a.get<double>(), y = b.get<double>();
    if (std::isnan(x) || std::isnan(y))
      return std::nullopt;
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (IsInt(a)) {
    const double y = b.get<double>();
    if (std::isnan(y))
      return std::nullopt;
    return CmpIntDouble(ToI128(a), y);
  }
  const double x = a.get<double>();
  if (std::isnan(x))
    return std::nullopt;
  return -CmpIntDouble(ToI128(b), x);
}

bool Equal(const json &a, const json &b) {
  if (a.is_number() && b.is_number()) {
    const auto c = CompareNumbers(a, b);
    return c && *c == 0;
  }
  if (a.type() != b.type())
    return false;
  if (a.is_array()) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!Equal(a[i], b[i]))
        return false;
    return true;
  }
  if (a.is_object()) {
    if (a.size() != b.size())
      return false;
    for (auto it = a.begin(); it != a.end(); ++it) {
      auto jt = b.find(it.key());
      if (jt == b.end() || !Equal(it.value(), *jt))
        return false;
    }
    return true;
  }
  return a == b;
}

std::string TypeName(const json &v) {
  switch (v.type()) {
  case json::value_t::null:
  case json::value_t::discarded:
    return "null_type";
  case json::value_t::boolean:
    return "bool";
  case json::value_t::number_integer:
    return "int";
  case json::value_t::number_unsigned:
    return v.get<uint64_t>() >
                   static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? "uint"
               : "int";
  case json::value_t::number_float:
    return "double";
  case json::value_t::string:
    return "string";
  case json::value_t::array:
    return "list";
  case json::value_t::object:
    return "map";
  case json::value_t::binary:
    return "bytes";
  }
  return "unknown";
}

size_t CodePoints(const std::string &s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80)
      ++n;
  return n;
}

std::string NoOverload(std::string_view op, const json &a) {
  return "no matching overload for '" + std::string(op) + "' applied to (" +
         TypeName(a) + ")";
}
std::string NoOverload(std::string_view op, const json &a, const json &b) {
  return "no matching overload for '" + std::string(op) + "' applied to (" +
         TypeName(a) + ", " + TypeName(b) + ")";
}

// ===========================================================================
// Evaluator
// ===========================================================================

struct Ctx {
  const Environment &env;
  size_t steps = 0;
  bool exhausted = false;
  std::vector<std::pair<std::string_view, const json *>> scope;
};

R Eval(const Node &n, Ctx &ctx);

R Arith(const std::string &op, const json &a, const json &b) {
  if (op == "+") {
    if (a.is_string() && b.is_string()) {
      if (a.get_ref<const std::string &>().size() +
              b.get_ref<const std::string &>().size() >
          kMaxStringBytes)
        return Err("string too large");
      return Ok(a.get<std::string>() + b.get<std::string>());
    }
    if (a.is_array() && b.is_array()) {
      if (a.size() + b.size() > kMaxListSize)
        return Err("list too large");
      json out = a;
      for (const auto &e : b)
        out.push_back(e);
      return Ok(std::move(out));
    }
  }
  if (!a.is_number() || !b.is_number())
    return Err(NoOverload(op, a, b));
  if (IsInt(a) && IsInt(b)) {
    const I128 x = ToI128(a), y = ToI128(b);
    if (op == "+")
      return FromI128(x + y);
    if (op == "-")
      return FromI128(x - y);
    if (op == "*") {
      I128 r;
      if (__builtin_mul_overflow(x, y, &r))
        return Err("integer overflow");
      return FromI128(r);
    }
    if (op == "/") {
      if (y == 0)
        return Err("division by zero");
      return FromI128(x / y);
    }
    if (op == "%") {
      if (y == 0)
        return Err("modulus by zero");
      return FromI128(x % y);
    }
  } else {
    if (op == "%")
      return Err(NoOverload(op, a, b));
    const double x = ToDouble(a), y = ToDouble(b);
    if (op == "+")
      return Ok(x + y);
    if (op == "-")
      return Ok(x - y);
    if (op == "*")
      return Ok(x * y);
    if (op == "/")
      return Ok(x / y);
  }
  return Err(NoOverload(op, a, b));
}

R Relational(const std::string &op, const json &a, const json &b) {
  if (op == "==")
    return Ok(Equal(a, b));
  if (op == "!=")
    return Ok(!Equal(a, b));
  if (op == "in") {
    if (b.is_array()) {
      for (const auto &e : b)
        if (Equal(a, e))
          return Ok(true);
      return Ok(false);
    }
    if (b.is_object()) {
      if (!a.is_string())
        return Err(NoOverload("in", a, b));
      return Ok(b.contains(a.get_ref<const std::string &>()));
    }
    return Err(NoOverload("in", a, b));
  }
  int c;
  if (a.is_number() && b.is_number()) {
    const auto cmp = CompareNumbers(a, b);
    if (!cmp)
      return Ok(false); // NaN is unordered
    c = *cmp;
  } else if (a.is_string() && b.is_string()) {
    const int r = a.get_ref<const std::string &>().compare(
        b.get_ref<const std::string &>());
    c = r < 0 ? -1 : (r > 0 ? 1 : 0);
  } else if (a.is_boolean() && b.is_boolean()) {
    c = static_cast<int>(a.get<bool>()) - static_cast<int>(b.get<bool>());
  } else {
    return Err(NoOverload(op, a, b));
  }
  if (op == "<")
    return Ok(c < 0);
  if (op == "<=")
    return Ok(c <= 0);
  if (op == ">")
    return Ok(c > 0);
  return Ok(c >= 0);
}

R ToInt(const json &v) {
  if (IsInt(v)) {
    const I128 x = ToI128(v);
    if (x > std::numeric_limits<int64_t>::max())
      return Err("int() range error");
    return Ok(json(static_cast<int64_t>(x)));
  }
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
      return Err("int() range error");
    return Ok(json(static_cast<int64_t>(d)));
  }
  if (v.is_string()) {
    const auto &s = v.get_ref<const std::string &>();
    int64_t out = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (s.empty() || ec != std::errc() || ptr != s.data() + s.size())
      return Err("int() cannot convert '" + s + "'");
    return Ok(json(out));
  }
  return Err(NoOverload("int", v));
}

R ToUint(const json &v) {
  if (IsInt(v)) {
    const I128 x = ToI128(v);
    if (x < 0)
      return Err("uint() range error");
    return Ok(json(static_cast<uint64_t>(x)));
  }
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (!(d > -1.0 && d < 18446744073709551616.0))
      return Err("uint() range error");
    return Ok(json(static_cast<uint64_t>(d)));
  }
  if (v.is_string()) {
    const auto &s = v.get_ref<const std::string &>();
    uint64_t out = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (s.empty() || ec != std::errc() || ptr != s.data() + s.size())
      return Err("uint() cannot convert '" + s + "'");
    return Ok(json(out));
  }
  return Err(NoOverload("uint", v));
}

R ToDoubleFn(const json &v) {
  if (v.is_number())
    return Ok(json(ToDouble(v)));
  if (v.is_string()) {
    const auto &s = v.get_ref<const std::string &>();
    double out = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (s.empty() || ec != std::errc() || ptr != s.data() + s.size())
      return Err("double() cannot convert '" + s + "'");
    return Ok(json(out));
  }
  return Err(NoOverload("double", v));
}

R ToStringFn(const json &v) {
  if (v.is_string())
    return Ok(v);
  if (v.is_boolean())
    return Ok(json(v.get<bool>() ? "true" : "false"));
  if (v.is_number_unsigned())
    return Ok(json(std::to_string(v.get<uint64_t>())));
  if (v.is_number_integer())
    return Ok(json(std::to_string(v.get<int64_t>())));
  if (v.is_number_float())
    return Ok(json(std::format("{}", v.get<double>())));
  return Err(NoOverload("string", v));
}

R Size(const json &v) {
  if (v.is_string())
    return Ok(json(static_cast<int64_t>(
        CodePoints(v.get_ref<const std::string &>()))));
  if (v.is_array() || v.is_object())
    return Ok(json(static_cast<int64_t>(v.size())));
  return Err(NoOverload("size", v));
}

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

R CallMethod(const std::string &name, const json &target,
             const std::vector<json> &args) {
  auto arity = [&](size_t n) -> std::optional<std::string> {
    if (args.size() != n)
      return "'" + name + "' expects " + std::to_string(n) + " argument(s)";
    return std::nullopt;
  };
  if (name == "size") {
    if (auto e = arity(0))
      return Err(*e);
    return Size(target);
  }
  if (target.is_string()) {
    const auto &s = target.get_ref<const std::string &>();
    if (name == "startsWith" || name == "endsWith" || name == "contains" ||
        name == "matches") {
      if (auto e = arity(1))
        return Err(*e);
      if (!args[0].is_string())
        return Err(NoOverload(name, target, args[0]));
      const auto &p = args[0].get_ref<const std::string &>();
      if (name == "startsWith")
        return Ok(s.starts_with(p));
      if (name == "endsWith")
        return Ok(s.ends_with(p));
      if (name == "contains")
        return Ok(s.find(p) != std::string::npos);
      if (p.size() > kMaxRegexPattern)
        return Err("matches(): pattern too long");
      if (s.size() > kMaxRegexInput)
        return Err("matches(): input too long");
      try {
        const std::regex re(p, std::regex::ECMAScript);
        return Ok(std::regex_search(s, re));
      } catch (const std::exception &e) {
        return Err(std::string("matches(): invalid regex: ") + e.what());
      }
    }
    if (name == "lowerAscii" || name == "upperAscii") {
      if (auto e = arity(0))
        return Err(*e);
      std::string out = s;
      for (char &c : out) {
        if (name == "lowerAscii" && c >= 'A' && c <= 'Z')
          c = static_cast<char>(c - 'A' + 'a');
        else if (name == "upperAscii" && c >= 'a' && c <= 'z')
          c = static_cast<char>(c - 'a' + 'A');
      }
      return Ok(json(std::move(out)));
    }
    if (name == "trim") {
      if (auto e = arity(0))
        return Err(*e);
      size_t b = 0, e = s.size();
      while (b < e && IsAsciiSpace(s[b]))
        ++b;
      while (e > b && IsAsciiSpace(s[e - 1]))
        --e;
      return Ok(json(s.substr(b, e - b)));
    }
  }
  if (target.is_array() && name == "join") {
    if (args.size() > 1)
      return Err("'join' expects 0 or 1 argument(s)");
    std::string sep;
    if (!args.empty()) {
      if (!args[0].is_string())
        return Err(NoOverload("join", target, args[0]));
      sep = args[0].get<std::string>();
    }
    std::string out;
    for (size_t i = 0; i < target.size(); ++i) {
      if (!target[i].is_string())
        return Err("join() requires a list of strings");
      if (i)
        out += sep;
      out += target[i].get_ref<const std::string &>();
      if (out.size() > kMaxStringBytes)
        return Err("string too large");
    }
    return Ok(json(std::move(out)));
  }
  return Err("no such method '" + name + "' for " + TypeName(target));
}

R CallGlobal(const std::string &name, const std::vector<json> &args,
             const Ctx &ctx) {
  auto one = [&]() -> std::optional<std::string> {
    if (args.size() != 1)
      return "'" + name + "' expects 1 argument";
    return std::nullopt;
  };
  if (name == "size") {
    if (auto e = one())
      return Err(*e);
    return Size(args[0]);
  }
  if (name == "int") {
    if (auto e = one())
      return Err(*e);
    return ToInt(args[0]);
  }
  if (name == "uint") {
    if (auto e = one())
      return Err(*e);
    return ToUint(args[0]);
  }
  if (name == "double") {
    if (auto e = one())
      return Err(*e);
    return ToDoubleFn(args[0]);
  }
  if (name == "string") {
    if (auto e = one())
      return Err(*e);
    return ToStringFn(args[0]);
  }
  if (name == "type") {
    if (auto e = one())
      return Err(*e);
    return Ok(json(TypeName(args[0])));
  }
  if (ctx.env.function) {
    if (const Function *fn = ctx.env.function(name); fn && *fn) {
      auto r = (*fn)(args);
      if (!r)
        return Err(r.error());
      return Ok(std::move(*r));
    }
  }
  return Err("unknown function '" + name + "'");
}

R EvalMacro(const Node &n, Ctx &ctx) {
  R target = Eval(*n.c[0], ctx);
  if (!target)
    return target;
  // Elements to iterate: list elements, or map keys.
  std::vector<json> keys;
  std::vector<const json *> items;
  if ((*target)->is_array()) {
    for (const auto &e : **target)
      items.push_back(&e);
  } else if ((*target)->is_object()) {
    keys.reserve((*target)->size());
    for (auto it = (*target)->begin(); it != (*target)->end(); ++it)
      keys.emplace_back(it.key());
    for (const auto &k : keys)
      items.push_back(&k);
  } else {
    return Err(NoOverload(n.name, **target));
  }

  const Node &body = *n.c[1];
  auto run = [&](const json *item) {
    ctx.scope.emplace_back(n.op, item);
    R r = Eval(body, ctx);
    ctx.scope.pop_back();
    return r;
  };

  if (n.name == "all" || n.name == "exists") {
    const bool absorbing = n.name == "exists"; // value that decides the result
    std::optional<std::string> error;
    for (const json *item : items) {
      R r = run(item);
      if (ctx.exhausted)
        return Err("evaluation budget exceeded");
      if (r && (*r)->is_boolean()) {
        if ((*r)->get<bool>() == absorbing)
          return Ok(absorbing);
      } else if (!error) {
        error = r ? NoOverload(n.name, **r) : r.error();
      }
    }
    if (error)
      return Err(*error);
    return Ok(!absorbing);
  }
  if (n.name == "exists_one") {
    int count = 0;
    for (const json *item : items) {
      R r = run(item);
      if (!r)
        return r;
      if (!(*r)->is_boolean())
        return Err(NoOverload(n.name, **r));
      if ((*r)->get<bool>())
        ++count;
    }
    return Ok(count == 1);
  }
  if (n.name == "filter") {
    json out = json::array();
    for (const json *item : items) {
      R r = run(item);
      if (!r)
        return r;
      if (!(*r)->is_boolean())
        return Err(NoOverload(n.name, **r));
      if ((*r)->get<bool>())
        out.push_back(*item);
    }
    return Ok(std::move(out));
  }
  // map
  json out = json::array();
  for (const json *item : items) {
    R r = run(item);
    if (!r)
      return r;
    out.push_back(std::move(*r).Take());
  }
  return Ok(std::move(out));
}

R Eval(const Node &n, Ctx &ctx) {
  if (ctx.exhausted || ++ctx.steps > ctx.env.max_steps) {
    ctx.exhausted = true;
    return Err("evaluation budget exceeded");
  }
  switch (n.kind) {
  case K::Lit:
    return V(&n.lit);

  case K::Ident: {
    for (auto it = ctx.scope.rbegin(); it != ctx.scope.rend(); ++it)
      if (it->first == n.name)
        return V(it->second);
    if (ctx.env.variable)
      if (const json *v = ctx.env.variable(n.name))
        return V(v);
    return Err("undeclared reference to '" + n.name + "'");
  }

  case K::Select: {
    R base = Eval(*n.c[0], ctx);
    if (!base)
      return base;
    if (!(*base)->is_object())
      return Err("cannot select field '" + n.name + "' from " +
                 TypeName(**base));
    auto it = (*base)->find(n.name);
    if (it == (*base)->end())
      return Err("no such key: " + n.name);
    if (base->Borrowed())
      return V(&*it);
    return Ok(*it);
  }

  case K::Index: {
    R base = Eval(*n.c[0], ctx);
    if (!base)
      return base;
    R idx = Eval(*n.c[1], ctx);
    if (!idx)
      return idx;
    const json *found = nullptr;
    if ((*base)->is_array()) {
      if (!IsInt(**idx))
        return Err(NoOverload("[]", **base, **idx));
      const I128 i = ToI128(**idx);
      if (i < 0 || i >= static_cast<I128>((*base)->size()))
        return Err("index out of range: " + (*idx)->dump());
      found = &(**base)[static_cast<size_t>(i)];
    } else if ((*base)->is_object()) {
      if (!(*idx)->is_string())
        return Err(NoOverload("[]", **base, **idx));
      auto it = (*base)->find((*idx)->get_ref<const std::string &>());
      if (it == (*base)->end())
        return Err("no such key: " + (*idx)->get<std::string>());
      found = &*it;
    } else {
      return Err(NoOverload("[]", **base, **idx));
    }
    if (base->Borrowed())
      return V(found);
    return Ok(*found);
  }

  case K::Call: {
    std::optional<V> target;
    size_t first_arg = 0;
    if (n.method) {
      R t = Eval(*n.c[0], ctx);
      if (!t)
        return t;
      target.emplace(std::move(*t));
      first_arg = 1;
    }
    std::vector<json> args;
    args.reserve(n.c.size() - first_arg);
    for (size_t i = first_arg; i < n.c.size(); ++i) {
      R a = Eval(*n.c[i], ctx);
      if (!a)
        return a;
      args.push_back(std::move(*a).Take());
    }
    if (n.method)
      return CallMethod(n.name, **target, args);
    return CallGlobal(n.name, args, ctx);
  }

  case K::List: {
    json out = json::array();
    for (const auto &c : n.c) {
      R v = Eval(*c, ctx);
      if (!v)
        return v;
      out.push_back(std::move(*v).Take());
    }
    return Ok(std::move(out));
  }

  case K::Map: {
    json out = json::object();
    for (size_t i = 0; i + 1 < n.c.size(); i += 2) {
      R k = Eval(*n.c[i], ctx);
      if (!k)
        return k;
      if (!(*k)->is_string())
        return Err("map keys must be strings");
      R v = Eval(*n.c[i + 1], ctx);
      if (!v)
        return v;
      const std::string key = (*k)->get<std::string>();
      if (out.contains(key))
        return Err("duplicate map key: " + key);
      out[key] = std::move(*v).Take();
    }
    return Ok(std::move(out));
  }

  case K::Neg: {
    R v = Eval(*n.c[0], ctx);
    if (!v)
      return v;
    if (IsInt(**v)) {
      // CEL ints are int64: -INT64_MIN overflows. Negating a uint yields
      // a negative int, which must still fit int64.
      if ((*v)->is_number_integer() && !(*v)->is_number_unsigned() &&
          (*v)->get<int64_t>() == std::numeric_limits<int64_t>::min())
        return Err("integer overflow");
      const I128 neg = -ToI128(**v);
      if (neg < std::numeric_limits<int64_t>::min())
        return Err("integer overflow");
      return FromI128(neg);
    }
    if ((*v)->is_number_float())
      return Ok(-(*v)->get<double>());
    return Err(NoOverload("-", **v));
  }

  case K::Not: {
    R v = Eval(*n.c[0], ctx);
    if (!v)
      return v;
    if (!(*v)->is_boolean())
      return Err(NoOverload("!", **v));
    return Ok(!(*v)->get<bool>());
  }

  case K::And:
  case K::Or: {
    const bool absorbing = n.kind == K::Or; // true for ||, false for &&
    const char *op = n.kind == K::Or ? "||" : "&&";
    R l = Eval(*n.c[0], ctx);
    if (l && (*l)->is_boolean() && (*l)->get<bool>() == absorbing)
      return Ok(absorbing);
    R r = Eval(*n.c[1], ctx);
    if (r && (*r)->is_boolean() && (*r)->get<bool>() == absorbing)
      return Ok(absorbing);
    if (!l)
      return l;
    if (!r)
      return r;
    if (!(*l)->is_boolean() || !(*r)->is_boolean())
      return Err(NoOverload(op, **l, **r));
    return Ok(!absorbing);
  }

  case K::Cond: {
    R c = Eval(*n.c[0], ctx);
    if (!c)
      return c;
    if (!(*c)->is_boolean())
      return Err(NoOverload("?:", **c));
    return Eval((*c)->get<bool>() ? *n.c[1] : *n.c[2], ctx);
  }

  case K::Bin: {
    R l = Eval(*n.c[0], ctx);
    if (!l)
      return l;
    R r = Eval(*n.c[1], ctx);
    if (!r)
      return r;
    const auto &op = n.op;
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
      return Arith(op, **l, **r);
    return Relational(op, **l, **r);
  }

  case K::Has: {
    R base = Eval(*n.c[0], ctx);
    if (!base)
      return base;
    if (!(*base)->is_object())
      return Err("has() requires a map, got " + TypeName(**base));
    return Ok((*base)->contains(n.name));
  }

  case K::Macro:
    return EvalMacro(n, ctx);
  }
  return Err("internal error: unknown node");
}

} // namespace

class Program {
public:
  explicit Program(NodeP root) : root(std::move(root)) {}
  NodeP root;
};

std::expected<std::shared_ptr<const Program>, std::string>
Compile(std::string_view source) {
  if (source.size() > kMaxSourceBytes)
    return std::unexpected("expression exceeds " +
                           std::to_string(kMaxSourceBytes) + " bytes");
  try {
    Lexer lexer(source);
    Parser parser(lexer.Run());
    return std::make_shared<const Program>(parser.Parse());
  } catch (const ParseError &e) {
    return std::unexpected("parse error at offset " + std::to_string(e.pos) +
                           ": " + e.message);
  }
}

std::expected<json, std::string> Evaluate(const Program &program,
                                          const Environment &env) {
  Ctx ctx{env, 0, false, {}};
  R r = Eval(*program.root, ctx);
  if (ctx.exhausted)
    return std::unexpected("evaluation budget exceeded");
  if (!r)
    return std::unexpected(r.error());
  return std::move(*r).Take();
}

bool EvaluatesTrue(const Program &program, const Environment &env) {
  auto r = Evaluate(program, env);
  return r && r->is_boolean() && r->get<bool>();
}

} // namespace pod::cel
