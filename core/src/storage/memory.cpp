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
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace savannah::storage {

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
  for (const auto& d : docs) {
    auto owned = arena_.copy(d.span());
    bson::BsonView view(
        std::span<const std::uint8_t>{owned.data(), owned.size()});
    const std::size_t slot_idx = slots_.size();
    slots_.push_back(DocSlot{view, false});
    indexes_.on_insert(slot_idx, view);
  }
  return {docs.size()};
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
                      std::vector<std::size_t> slot_ids,
                      std::span<const std::uint8_t> filter_bytes)
      : slots_(slots),
        slot_ids_(std::move(slot_ids)),
        filter_(filter_bytes.begin(), filter_bytes.end()) {}

  bool has_next() override {
    while (index_ < slot_ids_.size()) {
      const std::size_t slot_idx = slot_ids_[index_];
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

  bson::BsonView next() override { return slots_[slot_ids_[index_++]].view; }

 private:
  const std::vector<DocSlot>& slots_;
  std::vector<std::size_t> slot_ids_;
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
    std::span<const std::uint8_t> filter_bytes) {
  auto plan = jungle::query::v1::plan_index_lookup(indexes, filter_bytes);
  if (!plan) return std::make_unique<MemoryIterator>(slots, filter_bytes);
  return std::make_unique<IndexLookupIterator>(
      slots, jungle::query::v1::snapshot_from_lookup_plan(indexes, *plan),
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
    std::vector<std::size_t> snapshot;
    if (auto filter_plan = jungle::query::v1::plan_index_lookup(indexes_, filter_bytes);
        filter_plan && filter_plan->field_path == sort_plan->field_path) {
      if (filter_plan->exact_key) {
        const auto* slot_ids = indexes_.lookup_exact(
            filter_plan->field_path, *filter_plan->exact_key);
        if (slot_ids) snapshot = *slot_ids;
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
        slots_, std::move(snapshot), filter_bytes);
    return make_slice_iterator(std::move(iter), skip, limit);
  }

  if (jungle::query::v1::has_sort_spec(sort_bytes)) {
    std::vector<bson::BsonView> all;
    auto base = make_filter_iterator(slots_, indexes_, filter_bytes);
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

  auto iter = make_filter_iterator(slots_, indexes_, filter_bytes);
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

bool MemoryCollection::backfill_index(std::string_view name) {
  return indexes_.backfill_one(name, slots_);
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
      // Update = erase old + insert new on the index side. Cheaper than
      // diffing per-index: the manager already handles missing values.
      const bson::BsonView old_view = slot.view;
      auto owned = arena_.copy(std::span<const std::uint8_t>{
          outcome.bytes.data(), outcome.bytes.size()});
      slot.view = bson::BsonView(
          std::span<const std::uint8_t>{owned.data(), owned.size()});
      indexes_.on_erase(i, old_view);
      indexes_.on_insert(i, slot.view);
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
    const std::size_t new_slot_idx = slots_.size();
    slots_.push_back(DocSlot{new_view, false});
    indexes_.on_insert(new_slot_idx, new_view);

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
    indexes_.on_erase(i, slot.view);
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
