#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pod {

struct ClusterNode {
  std::string id;
  std::string host;
  int port;
};

struct PodConfig {
  // "standalone" (a database node) or "router" (the pods cluster router).
  std::string role = "standalone";
  int port = 3000;
  std::string storage_path = "./pod-storage";
  // Namespace used when a client connects without ?ns=...
  std::string default_namespace = "default";
  unsigned int threads = 0; // 0 = hardware concurrency
  bool sync_writes = false;
  uint64_t log_retention = 100000;
  // Security
  std::string rules_path;  // empty = open mode (no rules)
  std::string auth_secret; // HS256 secret for client tokens
  bool require_auth = false;
  // Cluster
  int internal_port = 0;         // node: port for router links (0 = off)
  std::string cluster_secret;    // shared by routers and nodes
  std::vector<ClusterNode> nodes; // router: the database nodes
  unsigned int virtual_nodes = 256;
};

// Reads the configuration from the environment (see .env.example).
// Throws std::invalid_argument on invalid values.
PodConfig LoadConfigFromEnv();

} // namespace pod
