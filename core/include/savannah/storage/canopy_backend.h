#pragma once

#include "savannah/storage/backend.h"
#include "savannah/storage/canopy.h"
#include "savannah/storage/memory.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace savannah::storage {

// How hard Canopy works to get each write onto the physical medium.
//
// Full reopened-and-fsynced the log per record, which cost ~16 ms per
// operation (63 single-doc inserts/sec). Batched keeps the log handle open and
// pushes each record to the OS with fflush, fsyncing at checkpoint boundaries
// and on close.
//
// The difference is only visible on power loss or an OS crash: a Batched write
// that has reached the OS page cache still survives the *process* dying
// (uncaught exception, process.exit, SIGKILL), because the kernel owns the
// bytes by then. This mirrors SQLite's PRAGMA synchronous=NORMAL and
// Postgres's synchronous_commit=off.
enum class SyncPolicy : std::uint8_t {
  Batched = 0,
  Full = 1,
};

enum class CanopyOpCode : std::uint8_t {
  Insert = 1,
  Update = 2,
  Erase = 3,
  CreateIndex = 4,
  DropIndex = 5,
};

class CanopyBackend final : public IStorageBackend {
 public:
  explicit CanopyBackend(std::filesystem::path root_dir,
                         SyncPolicy sync = SyncPolicy::Batched);

  jungle::storage::v1::Collection& collection(
      std::string_view db, std::string_view coll) override;

 private:
  class CollectionProxy final : public jungle::storage::v1::Collection {
   public:
    CollectionProxy(canopy::Layout layout, std::string db_name,
                    std::string coll_name, MemoryCollection& inner,
                    SyncPolicy sync);
    ~CollectionProxy();

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
    // Appends through a handle held open across records. Callers must already
    // hold write_mutex_.
    void append_record_locked(std::uint64_t seq, CanopyOpCode op,
                              std::span<const std::uint8_t> payload);
    void open_log_locked();
    // Closes the handle so the log file can be atomically replaced --
    // MoveFileEx cannot replace a target that is still open on Windows.
    void close_log_locked() noexcept;
    void sync_log_locked();
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
    SyncPolicy sync_;
    // Held open across appends. Reopening per record cost two syscalls on
    // every write for no benefit; nullptr simply means "not opened yet".
    std::FILE* log_file_{nullptr};
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
    // Bytes appended since the last checkpoint. Checkpointing rewrites a
    // snapshot of the whole collection, so triggering it on an operation
    // count made write cost grow with collection size: every N writes paid an
    // O(collection) snapshot. Sizing the trigger in log bytes instead keeps
    // that cost amortized and bounds replay work on open.
    std::uint64_t pending_bytes_{0};
  };

  void ensure_root_layout() const;

  canopy::Layout layout_;
  SyncPolicy sync_;
  MemoryBackend memory_;
  std::unordered_map<std::string, std::unique_ptr<CollectionProxy>> collections_;
};

}  // namespace savannah::storage
