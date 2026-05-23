#include "savannah/storage/canopy_backend.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace savannah::storage {

namespace {

constexpr char kManifestBody[] = "{\"engine\":\"canopy\",\"format\":3}\n";
// Format bump (LOG4, ST12) adds an options byte per index entry (currently
// only the `unique` flag occupies bit 0; remaining bits reserved for
// future options like partial/sparse).
constexpr std::uint8_t kLogHeader[] = {'C', 'N', 'P', 'Y', 'L', 'O', 'G', '4'};
constexpr std::uint8_t kStateHeader[] = {'C', 'N', 'P', 'Y', 'S', 'T', '1', '2'};
constexpr std::size_t kCheckpointIntervalOps = 4;
constexpr std::uint8_t kIndexFlagUnique = 1 << 0;

struct IndexDef {
  std::string name;
  std::vector<std::string> field_paths;
  bool unique{false};
};

std::string make_ns(std::string_view db, std::string_view coll) {
  std::string ns;
  ns.reserve(db.size() + 1 + coll.size());
  ns.append(db);
  ns.push_back('.');
  ns.append(coll);
  return ns;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(value));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(value));
}

void append_bytes(std::vector<std::uint8_t>& out,
                  std::span<const std::uint8_t> bytes) {
  append_u32(out, static_cast<std::uint32_t>(bytes.size()));
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void append_string(std::vector<std::uint8_t>& out, std::string_view value) {
  append_u32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

std::uint32_t read_u32(const std::uint8_t* data, std::size_t size,
                       std::size_t* offset) {
  if (*offset + sizeof(std::uint32_t) > size) {
    throw std::runtime_error("Canopy payload truncated while reading u32");
  }
  std::uint32_t out = 0;
  std::memcpy(&out, data + *offset, sizeof(out));
  *offset += sizeof(out);
  return out;
}

std::uint64_t read_u64(const std::uint8_t* data, std::size_t size,
                       std::size_t* offset) {
  if (*offset + sizeof(std::uint64_t) > size) {
    throw std::runtime_error("Canopy payload truncated while reading u64");
  }
  std::uint64_t out = 0;
  std::memcpy(&out, data + *offset, sizeof(out));
  *offset += sizeof(out);
  return out;
}

std::vector<std::uint8_t> read_bytes(const std::uint8_t* data, std::size_t size,
                                     std::size_t* offset) {
  const std::uint32_t len = read_u32(data, size, offset);
  if (*offset + len > size) {
    throw std::runtime_error("Canopy payload truncated while reading bytes");
  }
  std::vector<std::uint8_t> out(data + *offset, data + *offset + len);
  *offset += len;
  return out;
}

std::string read_string(const std::uint8_t* data, std::size_t size,
                        std::size_t* offset) {
  auto bytes = read_bytes(data, size, offset);
  return std::string(bytes.begin(), bytes.end());
}

FILE* cross_fopen(const char* path, const char* mode) {
#ifdef _WIN32
  FILE* file = nullptr;
  if (fopen_s(&file, path, mode) != 0) return nullptr;
  return file;
#else
  return std::fopen(path, mode);
#endif
}

bool cross_fsync(FILE* file) {
  if (std::fflush(file) != 0) return false;
#ifdef _WIN32
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

void atomic_replace_file(const std::filesystem::path& source,
                         const std::filesystem::path& target) {
#ifdef _WIN32
  if (!MoveFileExA(
          source.string().c_str(), target.string().c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("Canopy failed to atomically replace file");
  }
#else
  std::error_code ec;
  std::filesystem::rename(source, target, ec);
  if (ec) {
    throw std::runtime_error("Canopy failed to atomically replace file: " + ec.message());
  }
#endif
}

void ensure_log_header(std::filesystem::path path) {
  std::filesystem::create_directories(path.parent_path());
  if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 0) {
    return;
  }
  FILE* file = cross_fopen(path.string().c_str(), "wb");
  if (!file) {
    throw std::runtime_error("Canopy failed to create log file");
  }
  const bool ok =
      std::fwrite(kLogHeader, 1, sizeof(kLogHeader), file) == sizeof(kLogHeader) &&
      cross_fsync(file);
  std::fclose(file);
  if (!ok) throw std::runtime_error("Canopy failed to initialize log header");
}

void reset_log(std::filesystem::path path) {
  const auto tmp = path.string() + ".tmp";
  FILE* file = cross_fopen(tmp.c_str(), "wb");
  if (!file) {
    throw std::runtime_error("Canopy failed to reset log");
  }
  const bool ok =
      std::fwrite(kLogHeader, 1, sizeof(kLogHeader), file) == sizeof(kLogHeader) &&
      cross_fsync(file);
  std::fclose(file);
  if (!ok) throw std::runtime_error("Canopy failed while truncating log");
  atomic_replace_file(tmp, path);
}

void append_record(std::filesystem::path path, std::uint64_t seq, CanopyOpCode op,
                   std::span<const std::uint8_t> payload) {
  ensure_log_header(path);

  FILE* file = cross_fopen(path.string().c_str(), "ab");
  if (!file) {
    throw std::runtime_error("Canopy failed to open log for append");
  }

  const std::uint32_t frame_size = static_cast<std::uint32_t>(
      sizeof(std::uint64_t) + sizeof(std::uint8_t) + payload.size());
  const std::uint8_t opcode = static_cast<std::uint8_t>(op);
  const bool ok =
      std::fwrite(&frame_size, sizeof(frame_size), 1, file) == 1 &&
      std::fwrite(&seq, sizeof(seq), 1, file) == 1 &&
      std::fwrite(&opcode, sizeof(opcode), 1, file) == 1 &&
      (payload.empty() ||
       std::fwrite(payload.data(), 1, payload.size(), file) == payload.size()) &&
      cross_fsync(file);
  std::fclose(file);
  if (!ok) {
    throw std::runtime_error("Canopy failed to durably append log record");
  }
}

std::vector<std::uint8_t> build_insert_payload(
    storage::RecordId first_record_id, std::span<const bson::BsonView> docs) {
  std::vector<std::uint8_t> out;
  append_u32(out, static_cast<std::uint32_t>(docs.size()));
  for (std::size_t i = 0; i < docs.size(); ++i) {
    append_u64(out, first_record_id + i);
    append_bytes(out, docs[i].span());
  }
  return out;
}

std::vector<std::uint8_t> build_update_payload(
    std::span<const std::uint8_t> filter_bytes,
    std::span<const std::uint8_t> spec_bytes, bool multi, bool upsert) {
  std::vector<std::uint8_t> out;
  append_bytes(out, filter_bytes);
  append_bytes(out, spec_bytes);
  out.push_back(multi ? 1 : 0);
  out.push_back(upsert ? 1 : 0);
  return out;
}

std::vector<std::uint8_t> build_erase_payload(
    std::span<const std::uint8_t> filter_bytes, bool single) {
  std::vector<std::uint8_t> out;
  append_bytes(out, filter_bytes);
  out.push_back(single ? 1 : 0);
  return out;
}

std::vector<std::uint8_t> build_create_index_payload(
    std::string_view name, std::span<const std::string> field_paths,
    bool unique) {
  std::vector<std::uint8_t> out;
  append_string(out, name);
  append_u32(out, static_cast<std::uint32_t>(field_paths.size()));
  for (const auto& p : field_paths) append_string(out, p);
  out.push_back(unique ? kIndexFlagUnique : 0);
  return out;
}

std::vector<std::uint8_t> build_drop_index_payload(std::string_view name) {
  std::vector<std::uint8_t> out;
  append_string(out, name);
  return out;
}

std::vector<IndexDef> snapshot_index_defs(const index::IndexManager& indexes) {
  std::vector<IndexDef> defs;
  for (const auto& info : indexes.list()) {
    IndexDef d;
    d.name = info.name;
    d.field_paths = info.field_paths;
    d.unique = info.unique;
    defs.push_back(std::move(d));
  }
  return defs;
}

template <typename Result>
Result storage_error(std::string_view action, const std::exception& ex) {
  Result out;
  out.err_code = 8;
  out.err_name = "StorageError";
  out.err_message =
      std::string("Canopy ") + std::string(action) + " failed: " + ex.what();
  return out;
}

std::vector<std::uint8_t> build_state_blob(
    std::uint64_t checkpoint_seq, storage::RecordId next_record_id,
    const std::vector<MemoryCollection::SnapshotEntry>& docs,
    const std::vector<IndexDef>& indexes) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(kStateHeader), std::end(kStateHeader));
  append_u64(out, checkpoint_seq);
  append_u64(out, next_record_id);
  append_u32(out, static_cast<std::uint32_t>(docs.size()));
  for (const auto& doc : docs) {
    append_u64(out, doc.record_id);
    append_bytes(out, doc.bytes);
  }
  append_u32(out, static_cast<std::uint32_t>(indexes.size()));
  for (const auto& idx : indexes) {
    append_string(out, idx.name);
    append_u32(out, static_cast<std::uint32_t>(idx.field_paths.size()));
    for (const auto& p : idx.field_paths) append_string(out, p);
    out.push_back(idx.unique ? kIndexFlagUnique : 0);
  }
  return out;
}

struct LoadedState {
  std::uint64_t checkpoint_seq{0};
  storage::RecordId next_record_id{1};
  std::vector<MemoryCollection::SnapshotEntry> docs;
  std::vector<IndexDef> indexes;
};

LoadedState parse_state_blob(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < sizeof(kStateHeader) ||
      std::memcmp(bytes.data(), kStateHeader, sizeof(kStateHeader)) != 0) {
    throw std::runtime_error("Canopy state header mismatch");
  }

  LoadedState state;
  std::size_t offset = sizeof(kStateHeader);
  state.checkpoint_seq = read_u64(bytes.data(), bytes.size(), &offset);
  state.next_record_id = read_u64(bytes.data(), bytes.size(), &offset);

  const std::uint32_t doc_count = read_u32(bytes.data(), bytes.size(), &offset);
  state.docs.reserve(doc_count);
  for (std::uint32_t i = 0; i < doc_count; ++i) {
    MemoryCollection::SnapshotEntry entry;
    entry.record_id = read_u64(bytes.data(), bytes.size(), &offset);
    entry.bytes = read_bytes(bytes.data(), bytes.size(), &offset);
    state.docs.push_back(std::move(entry));
  }

  const std::uint32_t index_count = read_u32(bytes.data(), bytes.size(), &offset);
  state.indexes.reserve(index_count);
  for (std::uint32_t i = 0; i < index_count; ++i) {
    IndexDef def;
    def.name = read_string(bytes.data(), bytes.size(), &offset);
    const std::uint32_t path_count = read_u32(bytes.data(), bytes.size(), &offset);
    def.field_paths.reserve(path_count);
    for (std::uint32_t p = 0; p < path_count; ++p) {
      def.field_paths.push_back(read_string(bytes.data(), bytes.size(), &offset));
    }
    if (offset + 1 > bytes.size()) {
      throw std::runtime_error("Canopy state index entry truncated (missing flags)");
    }
    const std::uint8_t flags = bytes[offset++];
    def.unique = (flags & kIndexFlagUnique) != 0;
    state.indexes.push_back(std::move(def));
  }
  return state;
}

}  // namespace

CanopyBackend::CollectionProxy::CollectionProxy(
    canopy::Layout layout, std::string db_name, std::string coll_name,
    MemoryCollection& inner)
    : layout_(std::move(layout)),
      db_name_(std::move(db_name)),
      coll_name_(std::move(coll_name)),
      inner_(inner) {}

jungle::storage::v1::InsertResult CanopyBackend::CollectionProxy::insert(
    std::span<const bson::BsonView> docs) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  const storage::RecordId first_record_id = inner_.next_record_id();
  auto result = inner_.insert(docs);
  try {
    append_record(
        log_path(), next_log_seq_++, CanopyOpCode::Insert,
        build_insert_payload(first_record_id, docs));
    pending_ops_ += 1;
  } catch (const std::exception& ex) {
    reload_from_durable_state();
    return storage_error<jungle::storage::v1::InsertResult>("insert", ex);
  }
  maybe_checkpoint_best_effort();
  return result;
}

std::unique_ptr<jungle::storage::v1::Iterator>
CanopyBackend::CollectionProxy::find(
    std::span<const std::uint8_t> filter_bytes,
    std::span<const std::uint8_t> sort_bytes, std::size_t skip,
    std::size_t limit) {
  return inner_.find(filter_bytes, sort_bytes, skip, limit);
}

std::unique_ptr<jungle::storage::v1::Iterator>
CanopyBackend::CollectionProxy::aggregate(
    std::span<const std::uint8_t> pipeline_bytes) {
  return inner_.aggregate(pipeline_bytes);
}

jungle::storage::v1::UpdateBatchResult CanopyBackend::CollectionProxy::update(
    std::span<const std::uint8_t> filter_bytes,
    std::span<const std::uint8_t> spec_bytes, bool multi, bool upsert) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto result = inner_.update(filter_bytes, spec_bytes, multi, upsert);
  if (result.err_code != 0) return result;
  try {
    append_record(
        log_path(), next_log_seq_++, CanopyOpCode::Update,
        build_update_payload(filter_bytes, spec_bytes, multi, upsert));
    pending_ops_ += 1;
  } catch (const std::exception& ex) {
    reload_from_durable_state();
    return storage_error<jungle::storage::v1::UpdateBatchResult>(
        "update", ex);
  }
  maybe_checkpoint_best_effort();
  return result;
}

jungle::storage::v1::EraseResult CanopyBackend::CollectionProxy::erase(
    std::span<const std::uint8_t> filter_bytes, bool single) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto result = inner_.erase(filter_bytes, single);
  if (result.deleted == 0) return result;
  try {
    append_record(
        log_path(), next_log_seq_++, CanopyOpCode::Erase,
        build_erase_payload(filter_bytes, single));
    pending_ops_ += 1;
  } catch (const std::exception& ex) {
    reload_from_durable_state();
    return storage_error<jungle::storage::v1::EraseResult>("erase", ex);
  }
  maybe_checkpoint_best_effort();
  return result;
}

jungle::storage::v1::IndexMutationResult CanopyBackend::CollectionProxy::create_index(
    std::string_view name, std::span<const std::string> field_paths,
    jungle::storage::v1::Collection::CreateIndexOptions options) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto result = inner_.create_index(name, field_paths, options);
  if (result.err_code != 0 || !result.changed) return result;
  try {
    append_record(
        log_path(), next_log_seq_++, CanopyOpCode::CreateIndex,
        build_create_index_payload(name, field_paths, options.unique));
    pending_ops_ += 1;
  } catch (const std::exception& ex) {
    reload_from_durable_state();
    return storage_error<jungle::storage::v1::IndexMutationResult>(
        "create index", ex);
  }
  maybe_checkpoint_best_effort();
  return result;
}

jungle::storage::v1::IndexMutationResult CanopyBackend::CollectionProxy::drop_index(
    std::string_view name) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto result = inner_.drop_index(name);
  if (result.err_code != 0 || !result.changed) return result;
  try {
    append_record(
        log_path(), next_log_seq_++, CanopyOpCode::DropIndex,
        build_drop_index_payload(name));
    pending_ops_ += 1;
  } catch (const std::exception& ex) {
    reload_from_durable_state();
    return storage_error<jungle::storage::v1::IndexMutationResult>(
        "drop index", ex);
  }
  maybe_checkpoint_best_effort();
  return result;
}

void CanopyBackend::CollectionProxy::load_from_log() {
  if (loaded_) return;
  std::filesystem::create_directories(
      layout_.collection_dir(db_name_, coll_name_));
  load_from_state();

  ensure_log_header(log_path());
  std::ifstream in(log_path(), std::ios::binary);
  if (!in) throw std::runtime_error("Canopy failed to read collection log");
  std::vector<std::uint8_t> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < sizeof(kLogHeader) ||
      std::memcmp(bytes.data(), kLogHeader, sizeof(kLogHeader)) != 0) {
    throw std::runtime_error("Canopy log header mismatch");
  }

  std::size_t offset = sizeof(kLogHeader);
  while (offset < bytes.size()) {
    const std::uint32_t frame_size = read_u32(bytes.data(), bytes.size(), &offset);
    if (frame_size < sizeof(std::uint64_t) + sizeof(std::uint8_t) ||
        offset + frame_size > bytes.size()) {
      throw std::runtime_error("Canopy log frame truncated");
    }
    const std::uint64_t seq = read_u64(bytes.data(), bytes.size(), &offset);
    const auto op = static_cast<CanopyOpCode>(bytes[offset++]);
    const std::size_t payload_size =
        frame_size - sizeof(std::uint64_t) - sizeof(std::uint8_t);
    const std::span<const std::uint8_t> payload(
        bytes.data() + offset, payload_size);

    if (seq > last_checkpoint_seq_) {
      switch (op) {
        case CanopyOpCode::Insert:
          apply_insert_payload(payload);
          break;
        case CanopyOpCode::Update:
          apply_update_payload(payload);
          break;
        case CanopyOpCode::Erase:
          apply_erase_payload(payload);
          break;
        case CanopyOpCode::CreateIndex:
          apply_create_index_payload(payload);
          break;
        case CanopyOpCode::DropIndex:
          apply_drop_index_payload(payload);
          break;
        default:
          // Unknown opcode means the log is corrupted or written by a newer
          // version. Refuse to silently skip; the caller's recovery path
          // (reload_from_durable_state) will surface this to clients.
          throw std::runtime_error("Canopy log contains unknown opcode");
      }
    }

    if (seq >= next_log_seq_) next_log_seq_ = seq + 1;
    offset += payload_size;
  }

  loaded_ = true;
}

std::filesystem::path CanopyBackend::CollectionProxy::log_path() const {
  return layout_.collection_log_path(db_name_, coll_name_);
}

std::filesystem::path CanopyBackend::CollectionProxy::state_path() const {
  return layout_.collection_state_path(db_name_, coll_name_);
}

void CanopyBackend::CollectionProxy::load_from_state() {
  if (!std::filesystem::exists(state_path())) return;

  std::ifstream in(state_path(), std::ios::binary);
  if (!in) throw std::runtime_error("Canopy failed to read collection state");
  std::vector<std::uint8_t> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto state = parse_state_blob(bytes);
  last_checkpoint_seq_ = state.checkpoint_seq;
  next_log_seq_ = last_checkpoint_seq_ + 1;

  for (const auto& doc : state.docs) {
    inner_.restore_record(doc.record_id, doc.bytes);
  }
  inner_.set_next_record_id(state.next_record_id);
  for (const auto& idx : state.indexes) {
    inner_.create_index(idx.name, std::span<const std::string>(idx.field_paths));
  }
}

void CanopyBackend::CollectionProxy::apply_insert_payload(
    std::span<const std::uint8_t> payload) {
  std::size_t offset = 0;
  const std::uint32_t count = read_u32(payload.data(), payload.size(), &offset);
  for (std::uint32_t i = 0; i < count; ++i) {
    const RecordId record_id = read_u64(payload.data(), payload.size(), &offset);
    const auto bytes = read_bytes(payload.data(), payload.size(), &offset);
    inner_.restore_record(record_id, bytes);
  }
}

void CanopyBackend::CollectionProxy::apply_update_payload(
    std::span<const std::uint8_t> payload) {
  std::size_t offset = 0;
  const auto filter = read_bytes(payload.data(), payload.size(), &offset);
  const auto spec = read_bytes(payload.data(), payload.size(), &offset);
  if (offset + 2 > payload.size()) {
    throw std::runtime_error("Canopy update payload truncated");
  }
  const bool multi = payload[offset++] != 0;
  const bool upsert = payload[offset++] != 0;
  auto result = inner_.update(filter, spec, multi, upsert);
  if (result.err_code != 0) {
    throw std::runtime_error("Canopy replayed update failed");
  }
}

void CanopyBackend::CollectionProxy::apply_erase_payload(
    std::span<const std::uint8_t> payload) {
  std::size_t offset = 0;
  const auto filter = read_bytes(payload.data(), payload.size(), &offset);
  if (offset + 1 > payload.size()) {
    throw std::runtime_error("Canopy erase payload truncated");
  }
  inner_.erase(filter, payload[offset] != 0);
}

void CanopyBackend::CollectionProxy::apply_create_index_payload(
    std::span<const std::uint8_t> payload) {
  std::size_t offset = 0;
  const std::string name = read_string(payload.data(), payload.size(), &offset);
  const std::uint32_t count = read_u32(payload.data(), payload.size(), &offset);
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    paths.push_back(read_string(payload.data(), payload.size(), &offset));
  }
  if (offset + 1 > payload.size()) {
    throw std::runtime_error("Canopy create-index payload truncated (missing flags)");
  }
  const std::uint8_t flags = payload[offset++];
  jungle::storage::v1::Collection::CreateIndexOptions options;
  options.unique = (flags & kIndexFlagUnique) != 0;
  inner_.create_index(name, std::span<const std::string>(paths), options);
}

void CanopyBackend::CollectionProxy::apply_drop_index_payload(
    std::span<const std::uint8_t> payload) {
  std::size_t offset = 0;
  const std::string name = read_string(payload.data(), payload.size(), &offset);
  inner_.drop_index(name);
}

void CanopyBackend::CollectionProxy::maybe_checkpoint() {
  if (pending_ops_ < kCheckpointIntervalOps) return;
  checkpoint();
}

void CanopyBackend::CollectionProxy::maybe_checkpoint_best_effort() noexcept {
  try {
    maybe_checkpoint();
  } catch (...) {
    // The write is already durable in the append-only log at this point.
    // Checkpointing is maintenance; surfacing it as a write failure would
    // report an error for a committed write and break retry semantics.
  }
}

void CanopyBackend::CollectionProxy::checkpoint() {
  const auto docs = inner_.snapshot_entries();
  const auto indexes = snapshot_index_defs(inner_.indexes());
  const auto blob = build_state_blob(
      next_log_seq_ - 1, inner_.next_record_id(), docs, indexes);

  const auto tmp_path = state_path().string() + ".tmp";
  std::filesystem::create_directories(state_path().parent_path());
  {
    FILE* file = cross_fopen(tmp_path.c_str(), "wb");
    if (!file) {
      throw std::runtime_error("Canopy failed to open state tmp file");
    }
    const bool ok =
        std::fwrite(blob.data(), 1, blob.size(), file) == blob.size() &&
        cross_fsync(file);
    std::fclose(file);
    if (!ok) throw std::runtime_error("Canopy failed to write state snapshot");
  }

  atomic_replace_file(tmp_path, state_path());
  last_checkpoint_seq_ = next_log_seq_ - 1;
  pending_ops_ = 0;
  reset_log(log_path());
}

void CanopyBackend::CollectionProxy::reload_from_durable_state() {
  inner_.clear_for_reload();
  loaded_ = false;
  last_checkpoint_seq_ = 0;
  next_log_seq_ = 1;
  pending_ops_ = 0;
  load_from_log();
}

CanopyBackend::CanopyBackend(std::filesystem::path root_dir)
    : layout_(std::move(root_dir)) {
  ensure_root_layout();
}

jungle::storage::v1::Collection& CanopyBackend::collection(
    std::string_view db, std::string_view coll) {
  const std::string ns = make_ns(db, coll);
  auto it = collections_.find(ns);
  if (it == collections_.end()) {
    auto& inner = dynamic_cast<MemoryCollection&>(memory_.collection(db, coll));
    auto proxy = std::make_unique<CollectionProxy>(
        layout_, std::string(db), std::string(coll), inner);
    proxy->load_from_log();
    it = collections_.emplace(ns, std::move(proxy)).first;
  }
  return *it->second;
}

void CanopyBackend::ensure_root_layout() const {
  std::filesystem::create_directories(layout_.wal_dir());
  std::filesystem::create_directories(layout_.collections_dir());

  const auto manifest = layout_.manifest_path();
  if (!std::filesystem::exists(manifest)) {
    std::ofstream out(manifest, std::ios::binary);
    if (!out) {
      throw std::runtime_error("Canopy failed to create manifest");
    }
    out << kManifestBody;
    out.flush();
    return;
  }

  std::ifstream in(manifest, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Canopy failed to read manifest");
  }
  const std::string body(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (body.find("\"engine\":\"canopy\"") == std::string::npos ||
      body.find("\"format\":3") == std::string::npos) {
    throw std::runtime_error(
        "Canopy manifest version mismatch (expected format 3). "
        "Wipe the storage root to start fresh; older data is not migrated.");
  }
}

}  // namespace savannah::storage
