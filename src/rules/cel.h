// A subset of the Common Expression Language (CEL) used by Pod's rules
// engine. Values are nlohmann::json: null, bool, int (int64), uint (uint64),
// double, string, list (array) and map (object, string keys only).
//
// Supported: literals (decimal/hex ints, `u` suffix, doubles, '..'/".."
// strings with escapes, r'raw' strings, lists, maps), operators
// ! - * / % + - == != < <= > >= in && || ?:, member access, indexing,
// macros has() all() exists() exists_one() filter() map(), and the builtins
// size int uint double string type startsWith endsWith contains matches
// lowerAscii upperAscii trim join. Errors are values and are absorbed by
// && / || like in CEL.
#pragma once
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pod::cel {

// Opaque, immutable after Compile. Evaluate may be called concurrently from
// many threads on the same Program.
class Program;

// The error string includes the character offset of the problem.
std::expected<std::shared_ptr<const Program>, std::string>
Compile(std::string_view source);

using Function = std::function<std::expected<nlohmann::json, std::string>(
    const std::vector<nlohmann::json> &args)>;

struct Environment {
  // Resolves a variable; nullptr => "undeclared reference to 'x'" error.
  std::function<const nlohmann::json *(std::string_view name)> variable;
  // Extra GLOBAL functions (e.g. get, exists); may be empty.
  std::function<const Function *(std::string_view name)> function;
  // Evaluation budget: AST node visits, including inside macros.
  size_t max_steps = 10000;
};

std::expected<nlohmann::json, std::string> Evaluate(const Program &program,
                                                    const Environment &env);

// True iff evaluation succeeds AND the result is boolean true.
bool EvaluatesTrue(const Program &program, const Environment &env);

} // namespace pod::cel
