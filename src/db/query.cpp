#include "db/query.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pod {

using json = nlohmann::json;

namespace {

int TypeRank(const json &v) {
  switch (v.type()) {
  case json::value_t::null:
  case json::value_t::discarded:
    return 0;
  case json::value_t::boolean:
    return 1;
  case json::value_t::number_integer:
  case json::value_t::number_unsigned:
  case json::value_t::number_float:
    return 2;
  case json::value_t::string:
    return 3;
  case json::value_t::array:
    return 4;
  case json::value_t::object:
    return 5;
  case json::value_t::binary:
    return 6;
  }
  return 7;
}

template <typename T> int Sign(T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }

// Exact integer representation: sign + magnitude covers the full range of
// both int64 and uint64.
struct ExactInt {
  bool negative;
  uint64_t magnitude;
};

ExactInt FromSigned(int64_t v) {
  if (v >= 0)
    return {false, static_cast<uint64_t>(v)};
  // -(v+1)+1 avoids overflow on INT64_MIN.
  return {true, static_cast<uint64_t>(-(v + 1)) + 1u};
}

int CompareExact(ExactInt a, ExactInt b) {
  if (a.negative != b.negative)
    return a.negative ? -1 : 1;
  const int c = Sign(a.magnitude, b.magnitude);
  return a.negative ? -c : c;
}

// Compares an exact integer with a finite or infinite (non-NaN) double.
int CompareIntDouble(ExactInt i, double d) {
  constexpr double kTwo64 = 18446744073709551616.0; // 2^64
  if (d >= kTwo64)
    return -1;
  if (d <= -kTwo64)
    return 1;
  const double t = std::trunc(d);
  // |t| < 2^64 so its magnitude fits in uint64 exactly.
  const ExactInt ti{t < 0, static_cast<uint64_t>(std::fabs(t))};
  const ExactInt ti_norm = ti.magnitude == 0 ? ExactInt{false, 0} : ti;
  if (const int c = CompareExact(i, ti_norm); c != 0)
    return c;
  const double frac = d - t;
  if (frac > 0)
    return -1;
  if (frac < 0)
    return 1;
  return 0;
}

int CompareNumbers(const json &a, const json &b) {
  const bool af = a.is_number_float(), bf = b.is_number_float();
  if (af && bf) {
    const double x = a.get<double>(), y = b.get<double>();
    const bool xn = std::isnan(x), yn = std::isnan(y);
    if (xn || yn)
      return xn && yn ? 0 : (xn ? -1 : 1);
    return Sign(x, y);
  }
  auto exact = [](const json &v) {
    return v.is_number_unsigned() ? ExactInt{false, v.get<uint64_t>()}
                                  : FromSigned(v.get<int64_t>());
  };
  if (!af && !bf)
    return CompareExact(exact(a), exact(b));
  if (af) {
    const double x = a.get<double>();
    if (std::isnan(x))
      return -1;
    return -CompareIntDouble(exact(b), x);
  }
  const double y = b.get<double>();
  if (std::isnan(y))
    return 1;
  return CompareIntDouble(exact(a), y);
}

int CompareStrings(const std::string &a, const std::string &b) {
  // std::string::compare uses char_traits<char>, which compares as unsigned
  // char, i.e. raw UTF-8 bytes -> code point order.
  const int c = a.compare(b);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

bool SameTypeClass(const json &a, const json &b) {
  return TypeRank(a) == TypeRank(b);
}

const std::vector<std::string> kOpNames = {
    "EQUAL",         "GREATER",      "LESSER",
    "GREATER_EQUAL", "LESSER_EQUAL", "ARRAY.IN",
    "ARRAY.NOT_IN",  "ARRAY.EQUALS", "ARRAY.ELEMENT_AT"};

} // namespace

int CompareValues(const json &a, const json &b) {
  const int ra = TypeRank(a), rb = TypeRank(b);
  if (ra != rb)
    return Sign(ra, rb);
  switch (ra) {
  case 0:
    return 0;
  case 1:
    return Sign(a.get<bool>(), b.get<bool>());
  case 2:
    return CompareNumbers(a, b);
  case 3:
    return CompareStrings(a.get_ref<const std::string &>(),
                          b.get_ref<const std::string &>());
  case 4: {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
      if (const int c = CompareValues(a[i], b[i]); c != 0)
        return c;
    return Sign(a.size(), b.size());
  }
  case 5: {
    // nlohmann::json objects are std::map-backed, so iteration is key-sorted.
    auto ia = a.begin(), ib = b.begin();
    for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
      if (const int c = CompareStrings(ia.key(), ib.key()); c != 0)
        return c;
      if (const int c = CompareValues(ia.value(), ib.value()); c != 0)
        return c;
    }
    return Sign(a.size(), b.size());
  }
  case 6: {
    const auto &x = a.get_binary();
    const auto &y = b.get_binary();
    const size_t n = std::min(x.size(), y.size());
    if (n) {
      const int c = std::memcmp(x.data(), y.data(), n);
      if (c != 0)
        return c < 0 ? -1 : 1;
    }
    return Sign(x.size(), y.size());
  }
  }
  return 0;
}

std::expected<Filter, DbError> MakeFilter(const std::string &field,
                                          const std::string &op,
                                          const json &value) {
  auto fp = ParseFieldPath(field);
  if (!fp)
    return std::unexpected(fp.error());
  Filter f{.field = std::move(*fp), .op = FilterOp::Equal, .value = value};
  const auto it = std::find(kOpNames.begin(), kOpNames.end(), op);
  if (it == kOpNames.end())
    return std::unexpected(DbError::InvalidArgument(
        "Unknown filter operation '" + op + "'."));
  f.op = static_cast<FilterOp>(it - kOpNames.begin());

  if (f.op == FilterOp::ArrayEquals && !value.is_array())
    return std::unexpected(DbError::InvalidArgument(
        "ARRAY.EQUALS requires an array value."));
  if (f.op == FilterOp::ArrayElementAt) {
    if (!value.is_object() || !value.contains("index") ||
        !value.contains("value") || !value["index"].is_number_integer())
      return std::unexpected(DbError::InvalidArgument(
          "ARRAY.ELEMENT_AT requires a value of the form "
          "{\"index\": <integer>, \"value\": <any>}."));
    if (value["index"].is_number_unsigned() &&
        value["index"].get<uint64_t>() >
            static_cast<uint64_t>(INT64_MAX))
      return std::unexpected(
          DbError::InvalidArgument("ARRAY.ELEMENT_AT index is out of range."));
    f.element_index = value["index"].get<int64_t>();
  }
  return f;
}

std::expected<OrderBy, DbError> MakeOrderBy(const std::string &field,
                                            const std::string &direction) {
  auto fp = ParseFieldPath(field);
  if (!fp)
    return std::unexpected(fp.error());
  if (direction != "ASCENDING" && direction != "DESCENDING")
    return std::unexpected(DbError::InvalidArgument(
        "orderBy direction must be ASCENDING or DESCENDING."));
  return OrderBy{.field = std::move(*fp),
                 .descending = direction == "DESCENDING"};
}

bool MatchesFilter(const json &data, const Filter &f) {
  const json *v = GetField(data, f.field);
  if (!v)
    return false;
  switch (f.op) {
  case FilterOp::Equal:
    return CompareValues(*v, f.value) == 0;
  case FilterOp::Greater:
    return SameTypeClass(*v, f.value) && CompareValues(*v, f.value) > 0;
  case FilterOp::Lesser:
    return SameTypeClass(*v, f.value) && CompareValues(*v, f.value) < 0;
  case FilterOp::GreaterEqual:
    return SameTypeClass(*v, f.value) && CompareValues(*v, f.value) >= 0;
  case FilterOp::LesserEqual:
    return SameTypeClass(*v, f.value) && CompareValues(*v, f.value) <= 0;
  case FilterOp::ArrayIn:
  case FilterOp::ArrayNotIn: {
    if (!v->is_array())
      return false;
    const bool found = std::any_of(v->begin(), v->end(), [&](const json &e) {
      return CompareValues(e, f.value) == 0;
    });
    return f.op == FilterOp::ArrayIn ? found : !found;
  }
  case FilterOp::ArrayEquals:
    return v->is_array() && CompareValues(*v, f.value) == 0;
  case FilterOp::ArrayElementAt: {
    if (!v->is_array())
      return false;
    const int64_t size = static_cast<int64_t>(v->size());
    const int64_t idx = f.element_index < 0 ? size + f.element_index
                                            : f.element_index;
    if (idx < 0 || idx >= size)
      return false;
    return CompareValues((*v)[static_cast<size_t>(idx)], f.value["value"]) ==
           0;
  }
  }
  return false;
}

bool MatchesQuery(const Document &doc, const QuerySpec &spec) {
  for (const auto &o : spec.order_by)
    if (!GetField(doc.data, o.field))
      return false;
  for (const auto &f : spec.where)
    if (!MatchesFilter(doc.data, f))
      return false;
  return true;
}

bool DocumentLess(const Document &a, const Document &b, const QuerySpec &spec) {
  for (const auto &o : spec.order_by) {
    const json *va = GetField(a.data, o.field);
    const json *vb = GetField(b.data, o.field);
    // Documents without the field are filtered out by MatchesQuery; treat a
    // missing value as lowest so the ordering stays strict-weak regardless.
    int c;
    if (!va || !vb)
      c = (va ? 1 : 0) - (vb ? 1 : 0);
    else
      c = CompareValues(*va, *vb);
    if (c != 0)
      return o.descending ? c > 0 : c < 0;
  }
  const bool desc = !spec.order_by.empty() && spec.order_by.back().descending;
  const int c = CompareStrings(a.id, b.id);
  return desc ? c > 0 : c < 0;
}

void SortDocuments(std::vector<Document> &docs, const QuerySpec &spec) {
  std::sort(docs.begin(), docs.end(),
            [&](const Document &a, const Document &b) {
              return DocumentLess(a, b, spec);
            });
}

} // namespace pod
