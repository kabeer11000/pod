#pragma once
#include "server/config.h"
#include "storage/interface.h"

// A database node: serves clients over WebSockets and, when
// config.internal_port is set, routers over cluster links.
int entry_standalone(StorageDriver &d, const pod::PodConfig &config);

// The pods router: authenticates clients and proxies each connection to the
// node owning its namespace. Stores no data.
int entry_router(const pod::PodConfig &config);
