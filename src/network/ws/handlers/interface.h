#pragma once
#include "storage/interface.h"
#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct PodRequestMetaGeneric {
  std::string request_id;
  // READ | QUERY | WRITE | DELETE | MUTATE_MANY | SUBSCRIBE | ACK
  // (ACK is client sending a acknoledgement for the subscription EVENT response)
  std::string type;
};
// You must define a macro for nested structs too!
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PodRequestMetaGeneric, request_id, type)

struct PodResponseGeneric {
  std::string request_id;
  // ACK
  std::string action;
};

struct PodHandlerError {
  std::string reason;
  // insert
  std::string action;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PodHandlerError, reason)

// Insert Handler types
struct InsertRequest {
  PodRequestGeneric meta;
  std::string path;
  nlohmann::json document_data; // Use explicit namespace here
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InsertRequest, meta, path, document_data)

struct InsertResponse {
  PodRequestGeneric meta;
  int error;
  int status;
  std::string path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InsertResponse, meta, error, status, path)

// Get Handler types
struct GetRequest {
  PodRequestGeneric meta;
  std::string path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetRequest, meta, path)

struct GetResponse {
  PodRequestGeneric meta;
  int error;
  int status;
  std::string path;
  nlohmann::json data;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetResponse, meta, error, status, path, data)

// ============================================================================
// FUNCTION DECLARATIONS (Signatures only)
// ============================================================================

std::vector<std::string> splitPath(const std::string &path);

std::string joinPath(const std::vector<std::string> &segments);

std::expected<InsertResponse, PodHandlerError>
insert_handler(StorageDriver &d, InsertRequest &packet);
std::expected<GetResponse, PodHandlerError> get_handler(StorageDriver &d,
                                                        GetRequest &packet);
