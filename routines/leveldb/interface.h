#pragma once
#include "leveldb/db.h"
#include <optional>

leveldb::DB* getDatabaseInstance(std::optional<std::string> database_file = std::nullopt);
