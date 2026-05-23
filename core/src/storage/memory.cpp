#include "savannah/storage/memory.h"

#include "savannah/query/expression.h"
#include "savannah/query/filter.h"
#include "savannah/query/pipeline.h"
#include "savannah/query/planner.h"
#include "savannah/query/sort.h"
#include "savannah/query/value.h"
#include "savannah/query/update.h"
#include "savannah/storage/vector_iterator.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Read at process start, never re-checked. SavannahDB is intended for use
// inside a single Node process; flipping the env var mid-run shouldn't
// silently switch index behavior on later collection creations.
bool implicit_id_index_enabled() {
  static const bool enabled = [] {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, "SAVANNAH_DISABLE_ID_INDEX") != 0 || !value) {
      return true;
    }
    const bool disabled = len > 0 && value[0] != '\0' && value[0] != '0';
    std::free(value);
    return !disabled;
#else
    const char* value = std::getenv("SAVANNAH_DISABLE_ID_INDEX");
    if (!value) return true;
    return !(value[0] != '\0' && value[0] != '0');
#endif
  }();
  return enabled;
}

}  // namespace

namespace savannah::storage {

MemoryCollection::MemoryCollection(Arena& arena, IStorageBackend& owner,
                                   std::string db_name)
    : arena_(arena), owner_(owner), db_name_(std::move(db_name)) {
  // Implicit _id index. Without this every {_id: x} lookup is O(N) over
  // the slot vector; with it the planner picks the index and lookups are
  // O(log N). Idempotent — Canopy's replay path also calls create_index
  // for any persisted indexes, and this one collides cleanly with a
  // no-op return. Opt-out via SAVANNAH_DISABLE_ID_INDEX=1 for workloads
  // that never read by _id and want the ~30% insert-throughput back.
  if (implicit_id_index_enabled()) {
    indexes_.create("_id_", "_id");
  }
}

std::span<std::uint8_t> Arena::allocate(std::size_t bytes) {
  if (chunks_.empty() || chunks_.back().used + bytes > chunks_.back().size) {
    const std::size_t size = std::max(chunk_size_, bytes);
    Chunk c;
    c.data = std::make_unique<std::uint8_t[]>(size);
    c.size = size;
    c.used = 0;
    chunks_.push_back(std::move(c));
  }
  auto& back = chunks_.back();
  std::uint8_t* p = back.data.get() + back.used;
  back.used += bytes;
  used_total_ += bytes;
  return {p, bytes};
}

std::span<std::uint8_t> Arena::copy(std::span<const std::uint8_t> src) {
  auto out = allocate(src.size());
  std::memcpy(out.data(), src.data(), src.size());
  return out;
}

jungle::storage::v1::InsertResult MemoryCollection::insert(
    std::span<const bson::BsonView> docs) {
  slots_.reserve(slots_.size() + docs.size());
  jungle::storage::v1::InsertResult result{};
  for (const auto& d : docs) {
    auto owned = arena_.copy(d.span());
    bson::BsonView view(
        std::span<const std::uint8_t>{owned.data(), owned.size()});
    // Unique check first — at this point no record_id is consumed, so we
    // can short-circuit without leaving holes in the id sequence. Mongo's
    // ordered:true semantics: stop at the first error.
    const std::string violator = indexes_.would_violate_unique(
        kInvalidRecordId, view);
    if (!violator.empty()) {
      result.err_code = 11000;
      result.err_name = "DuplicateKey";
      result.err_message = "duplicate key in unique index " + violator;
      return result;
    }
    const RecordId record_id = next_record_id_++;
    const std::size_t slot_idx = slots_.size();
    slots_.push_back(DocSlot{record_id, view, false});
    slot_by_id_[record_id] = slot_idx;
    indexes_.on_insert(record_id, view);
    result.inserted_count += 1;
  }
  return result;
}

namespace {

// Walks the slots vector, skipping tombstoned entries and applying the
// MQL filter. Index-based + end snapshot so concurrent inserts/deletes
// don't shift the cursor's view (delete tombstones; insert grows past end).
class MemoryIterator final : public jungle::storage::v1::Iterator {
 public:
  MemoryIterator(const std::vector<DocSlot>& slots,
                 std::span<const std::uint8_t> filter_bytes)
      : slots_(slots),
        filter_(filter_bytes.begin(), filter_bytes.end()),
        end_(slots.size()) {}

  bool has_next() override {
    while (index_ < end_) {
      const auto& slot = slots_[index_];
      if (!slot.deleted &&
          jungle::query::v1::matches(slot.view, filter_)) {
        return true;
      }
      ++index_;
    }
    return false;
  }

  bson::BsonView next() override { return slots_[index_++].view; }

 private:
  const std::vector<DocSlot>& slots_;
  std::vector<std::uint8_t> filter_;
  std::size_t index_{0};
  std::size_t end_{0};
};

// Snapshot the candidate slot ids at iterator creation so getMore keeps the
// same view even if later inserts/updates mutate the underlying index vector.
class IndexLookupIterator final : public jungle::storage::v1::Iterator {
 public:
  IndexLookupIterator(const std::vector<DocSlot>& slots,
                      const std::unordered_map<RecordId, std::size_t>& slot_by_id,
                      std::vector<RecordId> record_ids,
                      std::span<const std::uint8_t> filter_bytes)
      : slots_(slots),
        slot_by_id_(slot_by_id),
        record_ids_(std::move(record_ids)),
        filter_(filter_bytes.begin(), filter_bytes.end()) {}

  bool has_next() override {
    while (index_ < record_ids_.size()) {
      const RecordId record_id = record_ids_[index_];
      auto slot_it = slot_by_id_.find(record_id);
      if (slot_it == slot_by_id_.end()) {
        ++index_;
        continue;
      }
      const std::size_t slot_idx = slot_it->second;
      if (slot_idx >= slots_.size()) {
        ++index_;
        continue;
      }
      const auto& slot = slots_[slot_idx];
      // Full filter re-check preserves exact scan parity even when the
      // index only satisfies one clause or the slot has since tombstoned.
      if (!slot.deleted &&
          jungle::query::v1::matches(slot.view, filter_)) {
        return true;
      }
      ++index_;
    }
    return false;
  }

  bson::BsonView next() override {
    const RecordId record_id = record_ids_[index_++];
    return slots_[slot_by_id_.at(record_id)].view;
  }

 private:
  const std::vector<DocSlot>& slots_;
  const std::unordered_map<RecordId, std::size_t>& slot_by_id_;
  std::vector<RecordId> record_ids_;
  std::vector<std::uint8_t> filter_;
  std::size_t index_{0};
};

// LookupPlan / SortPlan / planner functions live in query/planner.{h,cpp}.
// memory.cpp keeps only the storage-coupled glue: turning a plan into a
// concrete iterator over the slot vector.

std::unique_ptr<jungle::storage::v1::Iterator> make_slice_iterator(
    std::unique_ptr<jungle::storage::v1::Iterator> inner,
    std::size_t skip, std::size_t limit) {
  if (skip == 0 && limit == 0) return inner;
  return std::make_unique<SliceIterator>(
      std::move(inner), skip, limit == 0 ? static_cast<std::size_t>(-1) : limit);
}

std::unique_ptr<jungle::storage::v1::Iterator> make_filter_iterator(
    const std::vector<DocSlot>& slots, const index::IndexManager& indexes,
    const std::unordered_map<RecordId, std::size_t>& slot_by_id,
    std::span<const std::uint8_t> filter_bytes) {
  auto plan = jungle::query::v1::plan_index_lookup(indexes, filter_bytes);
  if (!plan) return std::make_unique<MemoryIterator>(slots, filter_bytes);
  return std::make_unique<IndexLookupIterator>(
      slots, slot_by_id,
      jungle::query::v1::snapshot_from_lookup_plan(indexes, *plan),
      filter_bytes);
}

// owned_bytes is the only expression-module helper memory.cpp still needs
// (for snapshot_live_docs). Everything else moved to query/pipeline.cpp.
using jungle::query::v1::owned_bytes;

// Snapshot live (non-tombstoned) docs from the slot vector as owned BSON
// byte buffers. Aggregation operates on owned bytes — it must not see
// arena-internal pointers because pipeline stages produce new docs that
// outlive the original arena bytes.
std::vector<std::vector<std::uint8_t>> snapshot_live_docs(
    const std::vector<DocSlot>& slots) {
  std::vector<std::vector<std::uint8_t>> docs;
  docs.reserve(slots.size());
  for (const auto& slot : slots) {
    if (slot.deleted) continue;
    docs.push_back(owned_bytes(slot.view));
  }
  return docs;
}

}  // namespace

std::unique_ptr<jungle::storage::v1::Iterator> MemoryCollection::find(
    std::span<const std::uint8_t> filter_bytes,
    std::span<const std::uint8_t> sort_bytes,
    std::size_t skip, std::size_t limit) {
  if (auto sort_plan = jungle::query::v1::plan_index_sort(indexes_, sort_bytes)) {
    std::vector<RecordId> snapshot;
    if (auto filter_plan = jungle::query::v1::plan_index_lookup(indexes_, filter_bytes);
        filter_plan && filter_plan->field_path == sort_plan->field_path) {
      if (filter_plan->exact_key) {
        const auto* record_ids = indexes_.lookup_exact(
            filter_plan->field_path, *filter_plan->exact_key);
        if (record_ids) snapshot = *record_ids;
      } else {
        snapshot = indexes_.lookup_range(
            filter_plan->field_path,
            filter_plan->lower_bound ? &filter_plan->lower_bound->key : nullptr,
            filter_plan->lower_bound ? filter_plan->lower_bound->inclusive : true,
            filter_plan->upper_bound ? &filter_plan->upper_bound->key : nullptr,
            filter_plan->upper_bound ? filter_plan->upper_bound->inclusive : true,
            !sort_plan->ascending);
      }
    } else {
      snapshot = indexes_.lookup_range(
          sort_plan->field_path, nullptr, true, nullptr, true,
          !sort_plan->ascending);
    }
    auto iter = std::make_unique<IndexLookupIterator>(
        slots_, slot_by_id_, std::move(snapshot), filter_bytes);
    return make_slice_iterator(std::move(iter), skip, limit);
  }

  if (jungle::query::v1::has_sort_spec(sort_bytes)) {
    std::vector<bson::BsonView> all;
    auto base = make_filter_iterator(slots_, indexes_, slot_by_id_, filter_bytes);
    while (base->has_next()) all.push_back(base->next());

    std::stable_sort(all.begin(), all.end(),
                     [&](const bson::BsonView& a, const bson::BsonView& b) {
                       return jungle::query::v1::sort_less(a, b, sort_bytes);
                     });
    if (skip > 0) {
      if (skip >= all.size()) all.clear();
      else all.erase(all.begin(), all.begin() + skip);
    }
    if (limit > 0 && all.size() > limit) all.resize(limit);
    return std::make_unique<VectorIterator>(std::move(all));
  }

  auto iter = make_filter_iterator(slots_, indexes_, slot_by_id_, filter_bytes);
  return make_slice_iterator(std::move(iter), skip, limit);
}

std::unique_ptr<jungle::storage::v1::Iterator> MemoryCollection::aggregate(
    std::span<const std::uint8_t> pipeline_bytes) {
  // Snapshot live docs first (storage-coupled), then hand off to the
  // backend-agnostic pipeline runner. Pipeline never sees DocSlot or arena
  // memory directly — it operates on owned BSON byte buffers.
  auto docs = snapshot_live_docs(slots_);
  docs = jungle::query::v1::run_pipeline(
      std::move(docs), pipeline_bytes, owner_, db_name_);
  return std::make_unique<OwnedBytesIterator>(std::move(docs));
}

jungle::storage::v1::IndexMutationResult MemoryCollection::create_index(
    std::string_view name, std::span<const std::string> field_paths,
    jungle::storage::v1::CreateIndexOptions options) {
  if (field_paths.empty()) {
    jungle::storage::v1::IndexMutationResult res;
    res.changed = false;
    res.err_code = 197;  // InvalidIndexSpecificationOption
    res.err_name = "InvalidIndexSpecification";
    res.err_message = "createIndex requires at least one field path";
    return res;
  }
  std::vector<std::string> paths(field_paths.begin(), field_paths.end());
  index::IndexOptions idx_opts;
  idx_opts.unique = options.unique;
  const bool created = indexes_.create(std::string(name), std::move(paths), idx_opts);
  if (!created) {
    jungle::storage::v1::IndexMutationResult res;
    res.changed = false;
    return res;
  }

  // For unique indexes on existing data, scan live slots first to detect any
  // pre-existing duplicate. If found, roll back the index registration so the
  // caller's listIndexes doesn't show a half-created index.
  if (idx_opts.unique) {
    std::unordered_map<std::string, storage::RecordId> seen;
    for (const auto& slot : slots_) {
      if (slot.deleted) continue;
      const std::string violator =
          indexes_.would_violate_unique(slot.record_id, slot.view);
      if (violator == std::string(name)) {
        indexes_.drop(name);
        jungle::storage::v1::IndexMutationResult res;
        res.changed = false;
        res.err_code = 11000;  // DuplicateKey
        res.err_name = "DuplicateKey";
        res.err_message = "existing data violates uniqueness for index " +
                       std::string(name);
        return res;
      }
      // Insert into the index incrementally so subsequent docs see prior
      // keys for the dup check. backfill_index would do this in one pass
      // without checking; we need the per-doc check.
      indexes_.on_insert(slot.record_id, slot.view);
    }
    jungle::storage::v1::IndexMutationResult res;
    res.changed = true;
    return res;
  }

  backfill_index(name);
  jungle::storage::v1::IndexMutationResult res;
  res.changed = true;
  return res;
}

jungle::storage::v1::IndexMutationResult MemoryCollection::drop_index(
    std::string_view name) {
  if (name == "_id_" && implicit_id_index_enabled()) {
    // Mongo also refuses to drop the _id index. Returning an error here
    // (rather than silently no-op'ing) is what drivers expect so they can
    // surface it correctly. Suppressed when the implicit index is disabled
    // via env — in that case `_id_` isn't ours to protect.
    jungle::storage::v1::IndexMutationResult res;
    res.changed = false;
    res.err_code = 72;  // InvalidIndexSpecificationOption — close enough
    res.err_name = "InvalidIndexSpecification";
    res.err_message = "cannot drop _id index";
    return res;
  }
  jungle::storage::v1::IndexMutationResult res;
  res.changed = indexes_.drop(name);
  return res;
}

bool MemoryCollection::backfill_index(std::string_view name) {
  return indexes_.backfill_one(name, slots_);
}

std::vector<MemoryCollection::SnapshotEntry> MemoryCollection::snapshot_entries() const {
  std::vector<SnapshotEntry> out;
  out.reserve(slots_.size());
  for (const auto& slot : slots_) {
    if (slot.deleted) continue;
    out.push_back(SnapshotEntry{
        slot.record_id,
        std::vector<std::uint8_t>(slot.view.data(), slot.view.data() + slot.view.size()),
    });
  }
  return out;
}

void MemoryCollection::restore_record(
    RecordId record_id, std::span<const std::uint8_t> doc_bytes) {
  auto owned = arena_.copy(doc_bytes);
  bson::BsonView view(
      std::span<const std::uint8_t>{owned.data(), owned.size()});
  const std::size_t slot_idx = slots_.size();
  slots_.push_back(DocSlot{record_id, view, false});
  slot_by_id_[record_id] = slot_idx;
  indexes_.on_insert(record_id, view);
  if (record_id >= next_record_id_) next_record_id_ = record_id + 1;
}

void MemoryCollection::clear_for_reload() {
  slots_.clear();
  slot_by_id_.clear();
  next_record_id_ = 1;
  indexes_ = index::IndexManager{};
  // Restore the implicit _id index after the IndexManager reset; otherwise
  // the next batch of replayed inserts wouldn't populate it.
  if (implicit_id_index_enabled()) {
    indexes_.create("_id_", "_id");
  }
}

jungle::storage::v1::UpdateBatchResult MemoryCollection::update(
    std::span<const std::uint8_t> filter_bytes,
    std::span<const std::uint8_t> spec_bytes,
    bool multi, bool upsert) {
  jungle::storage::v1::UpdateBatchResult res;
  const std::size_t end = slots_.size();  // snapshot — don't touch upsert insert.

  for (std::size_t i = 0; i < end; ++i) {
    auto& slot = slots_[i];
    if (slot.deleted) continue;
    if (!jungle::query::v1::matches(slot.view, filter_bytes)) continue;

    auto outcome = jungle::query::v1::apply_update(slot.view, spec_bytes);
    if (outcome.err_code != 0) {
      res.err_code = outcome.err_code;
      res.err_name = std::move(outcome.err_name);
      res.err_message = std::move(outcome.err_message);
      return res;  // First-error stops the batch (ordered semantics).
    }
    res.matched += 1;
    if (outcome.changed) {
      // Check uniqueness on the new doc shape BEFORE swapping it in. We
      // skip this record's own id so updating without changing the indexed
      // value doesn't false-positive against the doc's own pre-update key.
      const bson::BsonView candidate(std::span<const std::uint8_t>{
          outcome.bytes.data(), outcome.bytes.size()});
      const std::string violator =
          indexes_.would_violate_unique(slot.record_id, candidate);
      if (!violator.empty()) {
        res.err_code = 11000;
        res.err_name = "DuplicateKey";
        res.err_message = "duplicate key in unique index " + violator;
        return res;
      }
      // Update = erase old + insert new on the index side. Cheaper than
      // diffing per-index: the manager already handles missing values.
      const bson::BsonView old_view = slot.view;
      auto owned = arena_.copy(std::span<const std::uint8_t>{
          outcome.bytes.data(), outcome.bytes.size()});
      slot.view = bson::BsonView(
          std::span<const std::uint8_t>{owned.data(), owned.size()});
      indexes_.on_erase(slot.record_id, old_view);
      indexes_.on_insert(slot.record_id, slot.view);
      res.modified += 1;
    }
    if (!multi) break;
  }

  if (res.matched == 0 && upsert) {
    auto seeded = jungle::query::v1::seed_upsert(filter_bytes, spec_bytes);
    if (seeded.err_code != 0) {
      res.err_code = seeded.err_code;
      res.err_name = std::move(seeded.err_name);
      res.err_message = std::move(seeded.err_message);
      return res;
    }
    auto owned = arena_.copy(std::span<const std::uint8_t>{
        seeded.bytes.data(), seeded.bytes.size()});
    bson::BsonView new_view(
        std::span<const std::uint8_t>{owned.data(), owned.size()});
    // Unique check: an upsert must not introduce a duplicate key.
    const std::string violator =
        indexes_.would_violate_unique(kInvalidRecordId, new_view);
    if (!violator.empty()) {
      res.err_code = 11000;
      res.err_name = "DuplicateKey";
      res.err_message = "duplicate key in unique index " + violator;
      return res;
    }
    const RecordId record_id = next_record_id_++;
    const std::size_t new_slot_idx = slots_.size();
    slots_.push_back(DocSlot{record_id, new_view, false});
    slot_by_id_[record_id] = new_slot_idx;
    indexes_.on_insert(record_id, new_view);

    // Extract the upserted _id so the wire layer can echo it back.
    bson_t b;
    if (bson_init_static(&b, owned.data(), owned.size())) {
      bson_iter_t it;
      if (bson_iter_init(&it, &b) && bson_iter_find(&it, "_id")) {
        const bson_value_t* v = bson_iter_value(&it);
        // Simplest stable encoding: re-serialize `{_id: <value>}` so the
        // wire layer can deserialize and pluck the value out without us
        // duplicating libbson's value-encoding logic.
        bson_t wrap;
        bson_init(&wrap);
        bson_append_value(&wrap, "_id", -1, v);
        const std::uint8_t* wd = bson_get_data(&wrap);
        res.upserted_ids.emplace_back(wd, wd + wrap.len);
        bson_destroy(&wrap);
      }
    }
  }

  return res;
}

jungle::storage::v1::EraseResult MemoryCollection::erase(
    std::span<const std::uint8_t> filter_bytes, bool single) {
  jungle::storage::v1::EraseResult res;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    auto& slot = slots_[i];
    if (slot.deleted) continue;
    if (!jungle::query::v1::matches(slot.view, filter_bytes)) continue;
    indexes_.on_erase(slot.record_id, slot.view);
    slot.deleted = true;  // tombstone — live cursors stay valid.
    res.deleted += 1;
    if (single) break;
  }
  return res;
}

jungle::storage::v1::Collection& MemoryBackend::collection(
    std::string_view db, std::string_view coll) {
  std::string key;
  key.reserve(db.size() + 1 + coll.size());
  key.append(db);
  key.push_back('.');
  key.append(coll);

  auto it = collections_.find(key);
  if (it == collections_.end()) {
    auto inserted = collections_.emplace(
        std::move(key), std::make_unique<MemoryCollection>(arena_, *this, std::string(db)));
    it = inserted.first;
  }
  return *it->second;
}

}  // namespace savannah::storage
