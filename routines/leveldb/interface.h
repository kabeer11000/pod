#pragma once
#include "leveldb/db.h"
#include <leveldb/status.h>
#include <optional>

leveldb::DB* getDatabaseInstance(std::optional<std::string> database_file = std::nullopt);
