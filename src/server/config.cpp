#include "server/config.h"
#include "common/log.h"
#include "db/path.h"
#include <charconv>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace pod {

namespace {

std::string GetEnv(const char *name) {
  const char *v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

template <typename T> T ParseNumber(const std::string &raw, const char *name) {
  T value{};
  auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
  if (raw.empty() || ec != std::errc() || ptr != raw.data() + raw.size())
    throw std::invalid_argument(std::string(name) + " must be a number.");
  return value;
}

int ParsePort(const std::string &raw, const char *name) {
  const int port = ParseNumber<int>(raw, name);
  if (port < 1 || port > 65535)
    throw std::invalid_argument(std::string(name) +
                                " must be between 1 and 65535.");
  return port;
}

bool ParseBool(const std::string &v) { return v == "1" || v == "true"; }

// "id=host:port,id=host:port"
std::vector<ClusterNode> ParseNodes(const std::string &raw) {
  std::vector<ClusterNode> nodes;
  std::set<std::string> ids;
  size_t pos = 0;
  while (pos <= raw.size()) {
    const size_t comma = raw.find(',', pos);
    const std::string item = raw.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!item.empty()) {
      const size_t eq = item.find('=');
      const size_t colon = item.rfind(':');
      if (eq == std::string::npos || colon == std::string::npos ||
          colon < eq || eq == 0 || colon == eq + 1)
        throw std::invalid_argument(
            "POD_NODES entries must look like id=host:port (got '" + item +
            "').");
      ClusterNode n{item.substr(0, eq), item.substr(eq + 1, colon - eq - 1),
                    ParsePort(item.substr(colon + 1), "POD_NODES port")};
      if (!ids.insert(n.id).second)
        throw std::invalid_argument("POD_NODES has duplicate id '" + n.id +
                                    "'.");
      nodes.push_back(std::move(n));
    }
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return nodes;
}

} // namespace

PodConfig LoadConfigFromEnv() {
  PodConfig c;
  if (auto v = GetEnv("POD_ROLE"); !v.empty()) {
    if (v != "standalone" && v != "router")
      throw std::invalid_argument("POD_ROLE must be standalone or router.");
    c.role = v;
  }
  if (auto v = GetEnv("POD_PORT"); !v.empty())
    c.port = ParsePort(v, "POD_PORT");
  if (auto v = GetEnv("POD_STORAGE_PATH"); !v.empty())
    c.storage_path = v;
  if (auto v = GetEnv("DEFAULT_NAMESPACE"); !v.empty()) {
    auto ns = ValidateNamespace(v);
    if (!ns)
      throw std::invalid_argument("DEFAULT_NAMESPACE is invalid: " +
                                  ns.error().message);
    c.default_namespace = *ns;
  } else {
    c.default_namespace = std::string(kFallbackNamespace);
  }
  if (auto v = GetEnv("POD_THREADS"); !v.empty())
    c.threads = ParseNumber<unsigned int>(v, "POD_THREADS");
  if (auto v = GetEnv("POD_SYNC_WRITES"); !v.empty())
    c.sync_writes = ParseBool(v);
  if (auto v = GetEnv("POD_LOG_RETENTION"); !v.empty())
    c.log_retention = ParseNumber<uint64_t>(v, "POD_LOG_RETENTION");
  if (auto v = GetEnv("POD_LOG_LEVEL"); !v.empty()) {
    LogLevel level;
    if (!ParseLogLevel(v, level))
      throw std::invalid_argument(
          "POD_LOG_LEVEL must be one of debug, info, warn, error.");
    SetMinLogLevel(level);
  }
  c.rules_path = GetEnv("POD_RULES_PATH");
  c.auth_secret = GetEnv("POD_AUTH_SECRET");
  if (auto v = GetEnv("POD_REQUIRE_AUTH"); !v.empty())
    c.require_auth = ParseBool(v);
  if (c.require_auth && c.auth_secret.empty())
    throw std::invalid_argument(
        "POD_REQUIRE_AUTH needs POD_AUTH_SECRET to verify tokens.");
  if (auto v = GetEnv("POD_INTERNAL_PORT"); !v.empty())
    c.internal_port = ParsePort(v, "POD_INTERNAL_PORT");
  c.cluster_secret = GetEnv("POD_CLUSTER_SECRET");
  if (auto v = GetEnv("POD_NODES"); !v.empty())
    c.nodes = ParseNodes(v);
  if (auto v = GetEnv("POD_VIRTUAL_NODES"); !v.empty()) {
    c.virtual_nodes = ParseNumber<unsigned int>(v, "POD_VIRTUAL_NODES");
    if (c.virtual_nodes == 0 || c.virtual_nodes > 10000)
      throw std::invalid_argument("POD_VIRTUAL_NODES must be 1..10000.");
  }

  if ((c.internal_port || c.role == "router") && c.cluster_secret.size() < 16)
    throw std::invalid_argument(
        "Cluster links require POD_CLUSTER_SECRET (at least 16 characters).");
  if (c.role == "router" && c.nodes.empty())
    throw std::invalid_argument("POD_ROLE=router requires POD_NODES.");
  return c;
}

} // namespace pod
