#pragma once

#include "savannah/storage/backend.h"
#include "savannah/storage/canopy.h"
#include "savannah/storage/memory.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace savannah::storage {

enum class CanopyOpCode : std::uint8_t {
  Insert = 1,
  Update = 2,
  Erase = 3,
  CreateIndex = 4,
  DropIndex = 5,
};

class CanopyBackend final : public IStorageBackend {
 public:
  explicit CanopyBackend(std::filesystem::path root_dir);

  jungle::storage::v1::Collection& collection(
      std::string_view db, std::string_view coll) override;

 private:
  class CollectionProxy final : public jungle::storage::v1::Collection {
   public:
    CollectionProxy(canopy::Layout layout, std::string db_name,
                    std::string coll_name, MemoryCollection& inner);

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
    using jungle::storage::v1::Collection::create_index;
    jungle::storage::v1::IndexMutationResult drop_index(
        std::string_view name) override;
    index::IndexManager& indexes() override { return inner_.indexes(); }
    bool backfill_index(std::string_view name) override {
      return inner_.backfill_index(name);
    }

    void load_from_log();

   private:
    std::filesystem::path log_path() const;
    std::filesystem::path state_path() const;
    void load_from_state();
    void apply_insert_payload(std::span<const std::uint8_t> payload);
    void apply_update_payload(std::span<const std::uint8_t> payload);
    void apply_erase_payload(std::span<const std::uint8_t> payload);
    void apply_create_index_payload(std::span<const std::uint8_t> payload);
    void apply_drop_index_payload(std::span<const std::uint8_t> payload);
    void maybe_checkpoint();
    void maybe_checkpoint_best_effort() noexcept;
    void checkpoint();
    void reload_from_durable_state();

    canopy::Layout layout_;
    std::string db_name_;
    std::string coll_name_;
    MemoryCollection& inner_;
    // Serializes mutations through the WAL. The bookkeeping (next_log_seq_,
    // pending_ops_) and the multi-fwrite log append are not atomic on their
    // own; without this mutex, two concurrent writes from different threads
    // would interleave inside ops.bin and corrupt the log frame on replay.
    std::mutex write_mutex_;
    bool loaded_{false};
    std::uint64_t last_checkpoint_seq_{0};
    std::uint64_t next_log_seq_{1};
    std::size_t pending_ops_{0};
  };

  void ensure_root_layout() const;

  canopy::Layout layout_;
  MemoryBackend memory_;
  std::unordered_map<std::string, std::unique_ptr<CollectionProxy>> collections_;
};

}  // namespace savannah::storage
