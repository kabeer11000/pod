#pragma once
#include "storage/interface.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Ordered-map reads shared by the live store and its snapshots, with the same
// semantics as the LevelDB driver.
class MemoryReader : public StorageReader {
public:
  explicit MemoryReader(std::map<std::string, ByteVector> data)
      : data(std::move(data)) {}
  std::pair<DBStatus, ByteVector> Get(std::string_view key) override {
    auto it = data.find(std::string(key));
    if (it == data.end())
      return {DBStatus::NotFound(), {}};
    return {DBStatus::Ok(), it->second};
  }
  DBStatus Scan(std::string_view prefix, const ScanCallback &cb) override {
    return Iterate(prefix, {}, false, cb);
  }
  DBStatus Iterate(std::string_view prefix, std::string_view start,
                   bool reverse, const ScanCallback &cb) override {
    auto in_range = [&](const std::string &k) {
      return std::string_view(k).starts_with(prefix);
    };
    if (!reverse) {
      auto it = data.lower_bound(
          std::string(!start.empty() && start > prefix ? start : prefix));
      for (; it != data.end() && in_range(it->first); ++it)
        if (!cb(it->first, it->second))
          break;
      return DBStatus::Ok();
    }
    std::vector<std::pair<const std::string *, const ByteVector *>> keys;
    for (const auto &[k, v] : data)
      if (in_range(k) && (start.empty() || std::string_view(k) <= start))
        keys.emplace_back(&k, &v);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
      if (!cb(*it->first, *it->second))
        break;
    return DBStatus::Ok();
  }
  std::map<std::string, ByteVector> data;
};

// In-memory StorageDriver with the same semantics as LevelDB (ordered keys).
class MemoryStorage : public StorageDriver {
public:
  DBStatus Put(std::string_view key, ByteView value) override {
    std::lock_guard<std::mutex> l(mu);
    data[std::string(key)] = ByteVector(value.begin(), value.end());
    return DBStatus::Ok();
  }
  std::pair<DBStatus, ByteVector> Get(std::string_view key) override {
    return Copy().Get(key);
  }
  DBStatus Delete(std::string_view key) override {
    std::lock_guard<std::mutex> l(mu);
    data.erase(std::string(key));
    return DBStatus::Ok();
  }
  DBStatus Write(const std::vector<WriteOp> &ops) override {
    std::lock_guard<std::mutex> l(mu);
    if (fail_writes)
      return DBStatus::Error("injected failure");
    for (const auto &op : ops) {
      if (op.kind == WriteOp::Kind::Put)
        data[op.key] = op.value;
      else
        data.erase(op.key);
    }
    return DBStatus::Ok();
  }
  DBStatus Scan(std::string_view prefix, const ScanCallback &cb) override {
    return Copy().Scan(prefix, cb);
  }
  DBStatus Iterate(std::string_view prefix, std::string_view start,
                   bool reverse, const ScanCallback &cb) override {
    return Copy().Iterate(prefix, start, reverse, cb);
  }
  std::unique_ptr<StorageReader> Snapshot() override {
    return std::make_unique<MemoryReader>(Copy());
  }

  std::mutex mu;
  std::map<std::string, ByteVector> data;
  bool fail_writes = false;

private:
  MemoryReader Copy() {
    std::lock_guard<std::mutex> l(mu);
    return MemoryReader(data);
  }
};
