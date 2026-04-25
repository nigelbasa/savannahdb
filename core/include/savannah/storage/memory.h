#pragma once

#include "savannah/index/manager.h"
#include "savannah/storage/arena.h"
#include "savannah/storage/backend.h"

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
  bson::BsonView view;
  bool deleted{false};
};

class MemoryCollection final : public jungle::storage::v1::Collection {
 public:
  MemoryCollection(Arena& arena, IStorageBackend& owner, std::string db_name)
      : arena_(arena), owner_(owner), db_name_(std::move(db_name)) {}

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

  // Index access — collection owns the manager. Always present (empty
  // when no indexes have been declared) so callers don't branch on null.
  index::IndexManager& indexes() override { return indexes_; }
  bool backfill_index(std::string_view name) override;
  const std::vector<DocSlot>& slots() const { return slots_; }

 private:
  Arena& arena_;
  IStorageBackend& owner_;
  std::string db_name_;
  std::vector<DocSlot> slots_;
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
