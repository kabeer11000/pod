#include "storage/driver_leveldb.h"
#include "storage/types.h"
#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <leveldb/write_batch.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
DBStatus convert_leveldb_status(const leveldb::Status &s) {
  if (s.ok())
    return DBStatus::Ok();
  if (s.IsNotFound())
    return DBStatus::NotFound();
  return DBStatus::Error(s.ToString());
}

// Level DB likes its Slice as const character pointer bytes,
// What we have is a custom byteview (std::span). We recast this binary
// pointer to const char*
leveldb::Slice to_slice(std::string_view s) { return {s.data(), s.size()}; }
leveldb::Slice to_slice(ByteView v) {
  return {reinterpret_cast<const char *>(v.data()), v.size()};
}

std::pair<DBStatus, ByteVector> DoGet(leveldb::DB *db,
                                      const leveldb::ReadOptions &ro,
                                      std::string_view key) {
  std::string v;
  auto s = db->Get(ro, to_slice(key), &v);
  return {convert_leveldb_status(s), ByteVector(v.begin(), v.end())};
}

DBStatus DoIterate(leveldb::DB *db, const leveldb::ReadOptions &ro,
                   std::string_view prefix, std::string_view start,
                   bool reverse, const ScanCallback &cb) {
  // An iterator reads from an implicit (or the given explicit) snapshot, so
  // the iteration is consistent even while other threads write.
  std::unique_ptr<leveldb::Iterator> it(db->NewIterator(ro));
  const leveldb::Slice p = to_slice(prefix);
  auto emit = [&]() {
    const leveldb::Slice k = it->key();
    const leveldb::Slice v = it->value();
    return cb(std::string_view(k.data(), k.size()),
              ByteView(reinterpret_cast<const uint8_t *>(v.data()), v.size()));
  };

  if (!reverse) {
    // Seek to max(prefix, start).
    const bool use_start = !start.empty() && start > prefix;
    it->Seek(use_start ? to_slice(start) : p);
    for (; it->Valid() && it->key().starts_with(p); it->Next())
      if (!emit())
        break;
    return convert_leveldb_status(it->status());
  }

  // Reverse: position on the last key <= upper bound, where the upper bound
  // is `start` (inclusive) if given, otherwise the end of the prefix range.
  std::string upper;
  bool inclusive;
  if (!start.empty()) {
    upper.assign(start);
    inclusive = true;
  } else {
    // Smallest key strictly greater than every key with this prefix.
    upper.assign(prefix);
    while (!upper.empty() && static_cast<uint8_t>(upper.back()) == 0xFF)
      upper.pop_back();
    if (!upper.empty())
      upper.back() = static_cast<char>(static_cast<uint8_t>(upper.back()) + 1);
    inclusive = false;
  }
  if (upper.empty()) {
    it->SeekToLast();
  } else {
    it->Seek(to_slice(std::string_view(upper)));
    if (!it->Valid())
      it->SeekToLast();
    else if (!(inclusive && it->key() == to_slice(std::string_view(upper))))
      it->Prev(); // landed on the first key > bound (or == bound, exclusive)
  }
  for (; it->Valid(); it->Prev()) {
    if (it->key().compare(p) < 0)
      break; // moved below the prefix range
    if (!it->key().starts_with(p))
      continue; // above the prefix range but <= start
    if (!emit())
      break;
  }
  return convert_leveldb_status(it->status());
}

class LevelDBSnapshot : public StorageReader {
public:
  explicit LevelDBSnapshot(leveldb::DB *db) : _db(db), _snap(db->GetSnapshot()) {
    _ro.snapshot = _snap;
  }
  ~LevelDBSnapshot() override { _db->ReleaseSnapshot(_snap); }
  LevelDBSnapshot(const LevelDBSnapshot &) = delete;
  LevelDBSnapshot &operator=(const LevelDBSnapshot &) = delete;

  std::pair<DBStatus, ByteVector> Get(std::string_view key) override {
    return DoGet(_db, _ro, key);
  }
  DBStatus Scan(std::string_view prefix, const ScanCallback &cb) override {
    return DoIterate(_db, _ro, prefix, {}, false, cb);
  }
  DBStatus Iterate(std::string_view prefix, std::string_view start,
                   bool reverse, const ScanCallback &cb) override {
    return DoIterate(_db, _ro, prefix, start, reverse, cb);
  }

private:
  leveldb::DB *_db;
  const leveldb::Snapshot *_snap;
  leveldb::ReadOptions _ro;
};
} // namespace

DBStatus LevelDBStorageDriver::Put(std::string_view key, ByteView value) {
  leveldb::WriteOptions wo;
  wo.sync = _sync_writes;
  return convert_leveldb_status(_db->Put(wo, to_slice(key), to_slice(value)));
}

std::pair<DBStatus, ByteVector>
LevelDBStorageDriver::Get(std::string_view key) {
  return DoGet(_db, leveldb::ReadOptions(), key);
}

DBStatus LevelDBStorageDriver::Delete(std::string_view key) {
  leveldb::WriteOptions wo;
  wo.sync = _sync_writes;
  return convert_leveldb_status(_db->Delete(wo, to_slice(key)));
}

DBStatus LevelDBStorageDriver::Write(const std::vector<WriteOp> &ops) {
  if (ops.empty())
    return DBStatus::Ok();
  leveldb::WriteBatch batch;
  for (const auto &op : ops) {
    if (op.kind == WriteOp::Kind::Put)
      batch.Put(to_slice(std::string_view(op.key)),
                to_slice(ByteView(op.value)));
    else
      batch.Delete(to_slice(std::string_view(op.key)));
  }
  leveldb::WriteOptions wo;
  wo.sync = _sync_writes;
  return convert_leveldb_status(_db->Write(wo, &batch));
}

DBStatus LevelDBStorageDriver::Scan(std::string_view prefix,
                                    const ScanCallback &cb) {
  return DoIterate(_db, leveldb::ReadOptions(), prefix, {}, false, cb);
}

DBStatus LevelDBStorageDriver::Iterate(std::string_view prefix,
                                       std::string_view start, bool reverse,
                                       const ScanCallback &cb) {
  return DoIterate(_db, leveldb::ReadOptions(), prefix, start, reverse, cb);
}

std::unique_ptr<StorageReader> LevelDBStorageDriver::Snapshot() {
  return std::make_unique<LevelDBSnapshot>(_db);
}

// Constructors and Destructors on LevelDBStorageDriver
LevelDBStorageDriver::LevelDBStorageDriver(const std::string &db_path,
                                           bool sync_writes)
    : _sync_writes(sync_writes) {
  leveldb::Options o;
  o.create_if_missing = true;
  leveldb::DB *d = nullptr;
  auto status = leveldb::DB::Open(o, db_path, &d);
  if (!status.ok())
    throw std::runtime_error(
        "[error][driver_leveldb] Failed to open a new Level DB database. (" +
        status.ToString() + ")");
  _db = d;
}
LevelDBStorageDriver::~LevelDBStorageDriver() { delete _db; }
