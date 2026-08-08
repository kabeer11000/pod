#include "../routines/leveldb/interface.h"
#include "iostream"
#include "string"
#include <expected>
#include <leveldb/options.h>
#include <nlohmann/json.hpp>
#include <vector>
#include "vector"
#include "sstream"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/time_generator_v7.hpp> // Specific header for v7
#include <boost/uuid/uuid_io.hpp>             // Streaming/string serialization

using json = nlohmann::json;
// Pod handlers follow a generic type
// They return a std::expected wrapped type which resolves to an error or
// nothing
struct PodRequestGeneric {
  std::string request_id;
};
struct PodHandlerError {
  std::string reason;
};
struct InsertRequest {
  PodRequestGeneric meta;
  std::string path;
  json document_data;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InsertRequest, meta, path, document_data);
std::vector<std::string> splitPath(const std::string &path) {
  std::vector<std::string> segments;
  std::stringstream ss(path);
  std::string item;

  while (std::getline(ss, item, '/')) {
    if (!item.empty())
      segments.push_back(item);
  }
  return segments;
}

struct InsertionResponse {};

std::expected<InsertionResponse, PodHandlerError> insert_handler(json &packet) {
  const auto db = getDatabaseInstance();
  std::vector<std::string> path = splitPath(packet["path"]);
  // This validates if a path is correct or not.
  // ['users', 'user'] means user is an ID (document) since array is even. if ['users', 'user', 'music'] was the path,
  // this means users is a collection, user is a single document and music is a collection inside user hence we will
  // need to append data to end of the path to facilitate insertion
  if (path.size() % 2 == 0) {
      path.push_back()
  }
  db->Put(leveldb::WriteOptions(), path, const Slice &value)
};
