#include "db/index.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

namespace pod {

using json = nlohmann::json;

namespace {

// Type tags, ordered like CompareValues' type ranks. 0x00 is reserved for
// the end marker of arrays/objects and the escape scheme.
constexpr char kTagNull = 0x05;
constexpr char kTagFalse = 0x0A;
constexpr char kTagTrue = 0x0B;
constexpr char kTagNumber = 0x14;
constexpr char kTagString = 0x1E;
constexpr char kTagArray = 0x28;
constexpr char kTagObject = 0x32;
constexpr char kTagBinary = 0x3C;
constexpr char kEnd = 0x00;

void EncodeDouble(std::string &out, double d) {
  uint64_t bits;
  if (std::isnan(d)) {
    bits = 0; // below every encoded number (NaN sorts first)
  } else {
    if (d == 0.0)
      d = 0.0; // -0.0 == 0.0
    bits = std::bit_cast<uint64_t>(d);
    bits = (bits & 0x8000000000000000ULL) ? ~bits
                                          : (bits | 0x8000000000000000ULL);
    if (bits == 0)
      bits = 1; // keep 0 exclusively for NaN (-inf maps to 0x000F... anyway)
  }
  for (int i = 7; i >= 0; --i)
    out.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
}

std::string EscapeComponent(std::string_view s) {
  // For index ids built from user field names: keep it readable but
  // unambiguous.
  std::string out;
  for (char c : s) {
    if (c == '%' || c == ',' || c == ':' || c == '[' || c == ']') {
      static const char *hex = "0123456789ABCDEF";
      out += '%';
      out += hex[(static_cast<uint8_t>(c) >> 4) & 0xF];
      out += hex[static_cast<uint8_t>(c) & 0xF];
    } else {
      out += c;
    }
  }
  return out;
}

std::string HeaderPrefix(std::string_view ns) {
  std::string k;
  k += 'i';
  k += '\0';
  k += ns;
  k += '\0';
  return k;
}

} // namespace

void EncodeEscaped(std::string &out, std::string_view bytes) {
  for (char c : bytes) {
    if (c == '\0') {
      out += '\0';
      out += '\xFF';
    } else {
      out += c;
    }
  }
  out += '\0';
  out += '\x01';
}

void EncodeIndexValue(std::string &out, const json &v) {
  switch (v.type()) {
  case json::value_t::null:
  case json::value_t::discarded:
    out += kTagNull;
    return;
  case json::value_t::boolean:
    out += v.get<bool>() ? kTagTrue : kTagFalse;
    return;
  case json::value_t::number_integer:
    out += kTagNumber;
    EncodeDouble(out, static_cast<double>(v.get<int64_t>()));
    return;
  case json::value_t::number_unsigned:
    out += kTagNumber;
    EncodeDouble(out, static_cast<double>(v.get<uint64_t>()));
    return;
  case json::value_t::number_float:
    out += kTagNumber;
    EncodeDouble(out, v.get<double>());
    return;
  case json::value_t::string:
    out += kTagString;
    EncodeEscaped(out, v.get_ref<const std::string &>());
    return;
  case json::value_t::array:
    out += kTagArray;
    for (const auto &e : v)
      EncodeIndexValue(out, e);
    out += kEnd;
    return;
  case json::value_t::object:
    out += kTagObject;
    for (auto it = v.begin(); it != v.end(); ++it) {
      out += kTagString;
      EncodeEscaped(out, it.key());
      EncodeIndexValue(out, it.value());
    }
    out += kEnd;
    return;
  case json::value_t::binary: {
    out += kTagBinary;
    const auto &b = v.get_binary();
    EncodeEscaped(out, std::string_view(
                           reinterpret_cast<const char *>(b.data()), b.size()));
    return;
  }
  }
}

json IndexDefinition::ToJson() const {
  json f = json::array();
  for (const auto &field : fields)
    f.push_back({{"field", FieldPathToString(field.path)},
                 {"mode", field.mode == IndexMode::Value ? "VALUE"
                                                         : "ARRAY_CONTAINS"}});
  return json{{"id", id}, {"collection", collection_id}, {"fields", f}};
}

std::expected<IndexDefinition, DbError>
MakeIndexDefinition(const std::string &collection_id, const json &fields) {
  auto coll = ParsePath(collection_id);
  if (!coll || coll->Segments().size() != 1)
    return std::unexpected(DbError::InvalidArgument(
        "'collection' must be a single collection id, e.g. \"posts\"."));
  if (!fields.is_array() || fields.empty())
    return std::unexpected(
        DbError::InvalidArgument("'fields' must be a non-empty array."));
  if (fields.size() > kMaxIndexFields)
    return std::unexpected(DbError::InvalidArgument(
        "An index may have at most " + std::to_string(kMaxIndexFields) +
        " fields."));
  IndexDefinition def;
  def.collection_id = collection_id;
  std::string id = EscapeComponent(collection_id) + ":";
  int contains = 0;
  for (size_t i = 0; i < fields.size(); ++i) {
    const json &f = fields[i];
    if (!f.is_object() || !f.contains("field") || !f["field"].is_string())
      return std::unexpected(DbError::InvalidArgument(
          "Each index field must be {\"field\": <path>, \"mode\"?: ...}."));
    auto fp = ParseFieldPath(f["field"].get<std::string>());
    if (!fp)
      return std::unexpected(fp.error());
    IndexMode mode = IndexMode::Value;
    if (f.contains("mode") && !f["mode"].is_null()) {
      if (!f["mode"].is_string())
        return std::unexpected(
            DbError::InvalidArgument("Index field 'mode' must be a string."));
      const auto m = f["mode"].get<std::string>();
      if (m == "ARRAY_CONTAINS")
        mode = IndexMode::ArrayContains;
      else if (m != "VALUE")
        return std::unexpected(DbError::InvalidArgument(
            "Index field mode must be VALUE or ARRAY_CONTAINS."));
    }
    if (mode == IndexMode::ArrayContains && ++contains > 1)
      return std::unexpected(DbError::InvalidArgument(
          "An index may have at most one ARRAY_CONTAINS field."));
    for (const auto &existing : def.fields)
      if (existing.path == *fp)
        return std::unexpected(DbError::InvalidArgument(
            "Field '" + FieldPathToString(*fp) +
            "' appears twice in the index."));
    if (i)
      id += ',';
    id += EscapeComponent(FieldPathToString(*fp));
    if (mode == IndexMode::ArrayContains)
      id += "[]";
    def.fields.push_back(IndexField{std::move(*fp), mode});
  }
  def.id = std::move(id);
  return def;
}

std::expected<IndexDefinition, DbError>
IndexDefinitionFromJson(const json &j) {
  if (!j.is_object() || !j.contains("collection") ||
      !j["collection"].is_string() || !j.contains("fields"))
    return std::unexpected(
        DbError::DataCorruption("Stored index definition is malformed."));
  auto def = MakeIndexDefinition(j["collection"].get<std::string>(),
                                 j["fields"]);
  if (!def)
    return std::unexpected(DbError::DataCorruption(
        "Stored index definition is invalid: " + def.error().message));
  return def;
}

namespace index_keys {

std::string DefinitionsPrefix(std::string_view ns) {
  std::string k;
  k += 'x';
  k += '\0';
  k += ns;
  k += '\0';
  return k;
}

std::string DefinitionKey(std::string_view ns, std::string_view index_id) {
  std::string k = DefinitionsPrefix(ns);
  k += index_id;
  return k;
}

std::string IndexPrefix(std::string_view ns, std::string_view index_id) {
  std::string k = HeaderPrefix(ns);
  k += index_id;
  k += '\0';
  return k;
}

std::string CollectionPrefix(std::string_view ns, std::string_view index_id,
                             std::string_view collection_path) {
  std::string k = IndexPrefix(ns, index_id);
  EncodeEscaped(k, collection_path);
  return k;
}

} // namespace index_keys

std::vector<std::string> IndexEntries(std::string_view ns,
                                      const IndexDefinition &def,
                                      std::string_view collection_path,
                                      const Document &doc) {
  // Encoded alternatives per field (one for VALUE fields, one per distinct
  // element for ARRAY_CONTAINS).
  std::vector<std::vector<std::string>> parts;
  parts.reserve(def.fields.size());
  for (const auto &f : def.fields) {
    const json *v = GetField(doc.data, f.path);
    if (!v)
      return {};
    std::vector<std::string> alts;
    if (f.mode == IndexMode::Value) {
      std::string e;
      EncodeIndexValue(e, *v);
      alts.push_back(std::move(e));
    } else {
      if (!v->is_array() || v->empty())
        return {};
      std::set<std::string> distinct;
      for (const auto &el : *v) {
        std::string e;
        EncodeIndexValue(e, el);
        distinct.insert(std::move(e));
      }
      alts.assign(distinct.begin(), distinct.end());
    }
    parts.push_back(std::move(alts));
  }
  const std::string prefix =
      index_keys::CollectionPrefix(ns, def.id, collection_path);
  std::string suffix;
  EncodeEscaped(suffix, doc.id);

  std::vector<std::string> keys = {prefix};
  for (const auto &alts : parts) {
    std::vector<std::string> next;
    next.reserve(keys.size() * alts.size());
    for (const auto &k : keys)
      for (const auto &a : alts)
        next.push_back(k + a);
    keys = std::move(next);
  }
  for (auto &k : keys)
    k += suffix;
  return keys;
}

} // namespace pod
