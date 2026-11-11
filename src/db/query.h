// Structured query model and evaluation.
//
// Filter semantics (all filters are AND-ed; a missing field never matches):
//   EQUAL            field == value (deep equality; 1 == 1.0)
//   GREATER / LESSER / GREATER_EQUAL / LESSER_EQUAL
//                    ordered comparison; only matches when field and value
//                    are of the same type class (number/string/bool/...)
//   ARRAY.IN         field is an array containing an element equal to value
//   ARRAY.NOT_IN     field is an array with no element equal to value
//   ARRAY.EQUALS     field is an array deep-equal to value (value: array)
//   ARRAY.ELEMENT_AT field is an array and field[index] == value, where the
//                    filter value is {"index": <int>, "value": <any>}.
//                    Negative indexes count from the end (-1 = last).
//
// Value ordering (used by comparisons and orderBy), lowest first:
//   null < boolean < number < string < array < object < binary
// Numbers compare by exact mathematical value across int/uint/float; NaN
// sorts before every other number. Strings compare by UTF-8 bytes (code
// point order), arrays and objects lexicographically.
//
// orderBy: documents missing any orderBy field are excluded from results.
// Ties are broken by document id, in the direction of the last orderBy
// clause (ascending when there is none).
#pragma once
#include "db/document.h"
#include "db/error.h"
#include "db/field_path.h"
#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pod {

enum class FilterOp {
  Equal,
  Greater,
  Lesser,
  GreaterEqual,
  LesserEqual,
  ArrayIn,
  ArrayNotIn,
  ArrayEquals,
  ArrayElementAt,
};

struct Filter {
  FieldPath field;
  FilterOp op;
  nlohmann::json value;
  // ARRAY.ELEMENT_AT only: parsed from value.index / value.value
  int64_t element_index = 0;
};

struct OrderBy {
  FieldPath field;
  bool descending = false;
};

struct QuerySpec {
  std::vector<Filter> where;
  std::vector<OrderBy> order_by;
  std::optional<uint32_t> limit;
  std::vector<FieldPath> project;
};

// Builds a validated filter from its wire representation.
std::expected<Filter, DbError> MakeFilter(const std::string &field,
                                          const std::string &op,
                                          const nlohmann::json &value);
std::expected<OrderBy, DbError> MakeOrderBy(const std::string &field,
                                            const std::string &direction);

// Total ordering over JSON values: <0, 0, >0.
int CompareValues(const nlohmann::json &a, const nlohmann::json &b);

bool MatchesFilter(const nlohmann::json &data, const Filter &f);
// True if the document passes every filter and has every orderBy field.
bool MatchesQuery(const Document &doc, const QuerySpec &spec);
// Strict weak ordering of documents for the query's orderBy.
bool DocumentLess(const Document &a, const Document &b, const QuerySpec &spec);
void SortDocuments(std::vector<Document> &docs, const QuerySpec &spec);

} // namespace pod
