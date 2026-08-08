#pragma once
#include "leveldb/db.h"
#include <leveldb/status.h>
#include <optional>

leveldb::DB* getLevelDBInstance(std::optional<std::string> database_file = std::nullopt);
