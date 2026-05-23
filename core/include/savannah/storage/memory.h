#pragma once

#include "savannah/index/manager.h"
#include "savannah/storage/arena.h"
#include "savannah/storage/backend.h"
#include "savannah/storage/record_id.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savannah::storage {

// Tombstoned slot: deleted docs leave the slot in place so live cursors
// (which carry an index) keep a stable view. The vector only ever grows;
// memory is reclaimed at process restart for now (revisit at LMDB swap).
struct DocSlot {
  RecordId record_id{kInvalidRecordId};
  bson::BsonView view;
  bool deleted{false};
};

class MemoryCollection final : public jungle::storage::v1::Collection {
 public:
  struct SnapshotEntry {
    RecordId record_id{kInvalidRecordId};
    std::vector<std::uint8_t> bytes;
  };

  MemoryCollection(Arena& arena, IStorageBackend& owner, std::string db_name);

  jungle::storage::v1::InsertResult insert(
      std::span<const bson::BsonView> docs) override;

  std::unique_ptr<jungle::storage::v1::Iterator> find(
      std::span<const std::uint8_t> filter_bytes,
      std::span<const std::uint8_t> sort_bytes,
      std::size_t skip, std::size_t limit) override;

  std::unique_ptr<jungle::storage::v1::Iterator> aggregate(
      std::span<const std::uint8_t> pipeline_bytes) override;

  jungle::storage::v1::UpdateBatchResult update(
      std::span<const std::uint8_t> filter_bytes,
      std::span<const std::uint8_t> spec_bytes,
      bool multi, bool upsert) override;

  jungle::storage::v1::EraseResult erase(
      std::span<const std::uint8_t> filter_bytes, bool single) override;

  jungle::storage::v1::IndexMutationResult create_index(
      std::string_view name,
      std::span<const std::string> field_paths,
      jungle::storage::v1::CreateIndexOptions options = {}) override;
  using jungle::storage::v1::Collection::create_index;  // expose string overload
  jungle::storage::v1::IndexMutationResult drop_index(
      std::string_view name) override;
  // Index access — collection owns the manager. Always present (empty
  // when no indexes have been declared) so callers don't branch on null.
  index::IndexManager& indexes() override { return indexes_; }
  bool backfill_index(std::string_view name) override;
  const std::vector<DocSlot>& slots() const { return slots_; }
  std::vector<SnapshotEntry> snapshot_entries() const;
  RecordId next_record_id() const { return next_record_id_; }
  void set_next_record_id(RecordId next_record_id) {
    next_record_id_ = next_record_id;
  }
  void clear_for_reload();
  void restore_record(RecordId record_id,
                      std::span<const std::uint8_t> doc_bytes);

 private:
  Arena& arena_;
  IStorageBackend& owner_;
  std::string db_name_;
  std::vector<DocSlot> slots_;
  std::unordered_map<RecordId, std::size_t> slot_by_id_;
  RecordId next_record_id_{1};
  index::IndexManager indexes_;
};

class MemoryBackend final : public IStorageBackend {
 public:
  MemoryBackend() = default;

  jungle::storage::v1::Collection& collection(
      std::string_view db, std::string_view coll) override;

 private:
  Arena arena_{};
  std::unordered_map<std::string, std::unique_ptr<MemoryCollection>> collections_;
};

}  // namespace savannah::storage
