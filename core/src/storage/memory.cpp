#include "savannah/storage/memory.h"

#include "savannah/query/filter.h"
#include "savannah/query/sort.h"
#include "savannah/query/value.h"
#include "savannah/query/update.h"
#include "savannah/storage/vector_iterator.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

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

class SliceIterator final : public jungle::storage::v1::Iterator {
 public:
  SliceIterator(std::unique_ptr<jungle::storage::v1::Iterator> inner,
                std::size_t skip, std::size_t limit)
      : inner_(std::move(inner)), skip_(skip), remaining_(limit) {}

  bool has_next() override {
    if (!prepared_) {
      while (skip_ > 0 && inner_->has_next()) {
        inner_->next();
        --skip_;
      }
      prepared_ = true;
    }
    if (remaining_ == 0) return false;
    return inner_->has_next();
  }

  bson::BsonView next() override {
    auto out = inner_->next();
    if (remaining_ != unlimited()) --remaining_;
    return out;
  }

 private:
  static constexpr std::size_t unlimited() {
    return static_cast<std::size_t>(-1);
  }

  std::unique_ptr<jungle::storage::v1::Iterator> inner_;
  std::size_t skip_{0};
  std::size_t remaining_{unlimited()};
  bool prepared_{false};
};

bool is_operator_subdoc(bson_iter_t value) {
  if (bson_iter_type(&value) != BSON_TYPE_DOCUMENT) return false;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_document(&value, &len, &data);
  bson_t sub;
  if (!bson_init_static(&sub, data, len)) return false;
  bson_iter_t it;
  if (!bson_iter_init(&it, &sub)) return false;
  if (!bson_iter_next(&it)) return false;
  const char* key = bson_iter_key(&it);
  return key && key[0] == '$';
}

struct RangeBound {
  index::IndexedValue key;
  bool inclusive{false};
};

struct LookupPlan {
  std::string field_path;
  std::optional<index::IndexedValue> exact_key;
  std::optional<RangeBound> lower_bound;
  std::optional<RangeBound> upper_bound;
};

bool tighten_lower(std::optional<RangeBound>& current, bson_iter_t candidate,
                   bool inclusive) {
  RangeBound next{index::IndexedValue::from_iter(candidate), inclusive};
  if (!current) {
    current = std::move(next);
    return true;
  }

  bson_iter_t current_it, next_it;
  if (!current->key.get_iter(&current_it) || !next.key.get_iter(&next_it)) {
    return false;
  }
  auto cmp = jungle::query::v1::value_compare(next_it, current_it);
  if (!cmp) return false;
  if (*cmp > 0 || (*cmp == 0 && !inclusive && current->inclusive)) {
    current = std::move(next);
  }
  return true;
}

bool tighten_upper(std::optional<RangeBound>& current, bson_iter_t candidate,
                   bool inclusive) {
  RangeBound next{index::IndexedValue::from_iter(candidate), inclusive};
  if (!current) {
    current = std::move(next);
    return true;
  }

  bson_iter_t current_it, next_it;
  if (!current->key.get_iter(&current_it) || !next.key.get_iter(&next_it)) {
    return false;
  }
  auto cmp = jungle::query::v1::value_compare(next_it, current_it);
  if (!cmp) return false;
  if (*cmp < 0 || (*cmp == 0 && !inclusive && current->inclusive)) {
    current = std::move(next);
  }
  return true;
}

bool parse_range_operator_doc(bson_iter_t value, LookupPlan* plan) {
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_document(&value, &len, &data);
  bson_t sub;
  if (!bson_init_static(&sub, data, len)) return false;
  bson_iter_t it;
  if (!bson_iter_init(&it, &sub)) return false;

  bool saw_range = false;
  while (bson_iter_next(&it)) {
    const char* op = bson_iter_key(&it);
    if (!op || op[0] != '$') return false;
    const bson_type_t type = bson_iter_type(&it);
    if (type == BSON_TYPE_ARRAY) continue;  // multikey deferred.
    if (std::string_view(op) == "$gt") {
      if (!tighten_lower(plan->lower_bound, it, false)) return false;
      saw_range = true;
    } else if (std::string_view(op) == "$gte") {
      if (!tighten_lower(plan->lower_bound, it, true)) return false;
      saw_range = true;
    } else if (std::string_view(op) == "$lt") {
      if (!tighten_upper(plan->upper_bound, it, false)) return false;
      saw_range = true;
    } else if (std::string_view(op) == "$lte") {
      if (!tighten_upper(plan->upper_bound, it, true)) return false;
      saw_range = true;
    }
  }
  if (!saw_range) return false;

  if (plan->lower_bound && plan->upper_bound) {
    bson_iter_t lower_it, upper_it;
    if (!plan->lower_bound->key.get_iter(&lower_it) ||
        !plan->upper_bound->key.get_iter(&upper_it)) {
      return false;
    }
    auto cmp = jungle::query::v1::value_compare(lower_it, upper_it);
    if (!cmp) return true;  // Cross-type rejected later by matches().
    if (*cmp > 0) return false;
    if (*cmp == 0 &&
        (!plan->lower_bound->inclusive || !plan->upper_bound->inclusive)) {
      return false;
    }
  }
  return true;
}

std::optional<LookupPlan> plan_index_lookup(const index::IndexManager& indexes,
                                            std::span<const std::uint8_t> filter_bytes) {
  bson_t filter;
  if (filter_bytes.size() < 5 ||
      !bson_init_static(&filter, filter_bytes.data(), filter_bytes.size())) {
    return std::nullopt;
  }

  bson_iter_t it;
  if (!bson_iter_init(&it, &filter)) return std::nullopt;

  std::optional<LookupPlan> plan;
  std::size_t indexable_count = 0;
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key || key[0] == '$') continue;
    if (!indexes.has_path(key)) continue;
    if (bson_iter_type(&it) == BSON_TYPE_ARRAY) continue;  // multikey deferred.

    LookupPlan candidate;
    candidate.field_path = key;

    if (is_operator_subdoc(it)) {
      if (!parse_range_operator_doc(it, &candidate)) continue;
    } else {
      candidate.exact_key = index::IndexedValue::from_iter(it);
    }

    ++indexable_count;
    if (indexable_count > 1) return std::nullopt;
    plan = std::move(candidate);
  }

  if (indexable_count != 1) return std::nullopt;
  return plan;
}

struct SortPlan {
  std::string field_path;
  bool ascending{true};
};

bool has_sort_spec(std::span<const std::uint8_t> sort_bytes) {
  if (sort_bytes.size() < 5) return false;
  bson_t sort;
  if (!bson_init_static(&sort, sort_bytes.data(), sort_bytes.size())) return false;
  bson_iter_t it;
  return bson_iter_init(&it, &sort) && bson_iter_next(&it);
}

std::optional<SortPlan> plan_index_sort(const index::IndexManager& indexes,
                                        std::span<const std::uint8_t> sort_bytes) {
  bson_t sort;
  if (sort_bytes.size() < 5 ||
      !bson_init_static(&sort, sort_bytes.data(), sort_bytes.size())) {
    return std::nullopt;
  }

  bson_iter_t it;
  if (!bson_iter_init(&it, &sort) || !bson_iter_next(&it)) return std::nullopt;
  const char* key = bson_iter_key(&it);
  if (!key || !indexes.has_path(key)) return std::nullopt;

  bool ascending = true;
  if (jungle::query::v1::is_numeric(bson_iter_type(&it))) {
    ascending = bson_iter_as_int64(&it) >= 0;
  }

  // F7 only supports single-key index-backed sorts.
  if (bson_iter_next(&it)) return std::nullopt;
  return SortPlan{std::string(key), ascending};
}

std::unique_ptr<jungle::storage::v1::Iterator> make_slice_iterator(
    std::unique_ptr<jungle::storage::v1::Iterator> inner,
    std::size_t skip, std::size_t limit) {
  if (skip == 0 && limit == 0) return inner;
  return std::make_unique<SliceIterator>(
      std::move(inner), skip, limit == 0 ? static_cast<std::size_t>(-1) : limit);
}

std::vector<std::size_t> snapshot_from_lookup_plan(
    const index::IndexManager& indexes, const LookupPlan& plan) {
  if (plan.exact_key) {
    const auto* slot_ids = indexes.lookup_exact(plan.field_path, *plan.exact_key);
    return slot_ids ? *slot_ids : std::vector<std::size_t>{};
  }
  return indexes.lookup_range(
      plan.field_path,
      plan.lower_bound ? &plan.lower_bound->key : nullptr,
      plan.lower_bound ? plan.lower_bound->inclusive : true,
      plan.upper_bound ? &plan.upper_bound->key : nullptr,
      plan.upper_bound ? plan.upper_bound->inclusive : true,
      false);
}

std::unique_ptr<jungle::storage::v1::Iterator> make_filter_iterator(
    const std::vector<DocSlot>& slots, const index::IndexManager& indexes,
    std::span<const std::uint8_t> filter_bytes) {
  auto plan = plan_index_lookup(indexes, filter_bytes);
  if (!plan) return std::make_unique<MemoryIterator>(slots, filter_bytes);
  return std::make_unique<IndexLookupIterator>(
      slots, snapshot_from_lookup_plan(indexes, *plan), filter_bytes);
}

}  // namespace

std::unique_ptr<jungle::storage::v1::Iterator> MemoryCollection::find(
    std::span<const std::uint8_t> filter_bytes,
    std::span<const std::uint8_t> sort_bytes,
    std::size_t skip, std::size_t limit) {
  if (auto sort_plan = plan_index_sort(indexes_, sort_bytes)) {
    std::vector<std::size_t> snapshot;
    if (auto filter_plan = plan_index_lookup(indexes_, filter_bytes);
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

  if (has_sort_spec(sort_bytes)) {
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
        std::move(key), std::make_unique<MemoryCollection>(arena_));
    it = inserted.first;
  }
  return *it->second;
}

}  // namespace savannah::storage
