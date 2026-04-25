#include "savannah/storage/memory.h"

#include "savannah/query/filter.h"
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

constexpr std::uint8_t kEmptyBsonBytes[] = {5, 0, 0, 0, 0};

std::span<const std::uint8_t> empty_bson() {
  return {kEmptyBsonBytes, sizeof(kEmptyBsonBytes)};
}

bool init_static_bson(std::span<const std::uint8_t> bytes, bson_t* out) {
  return bytes.size() >= 5 && bson_init_static(out, bytes.data(), bytes.size());
}

std::vector<std::uint8_t> copy_bytes(std::span<const std::uint8_t> bytes) {
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> owned_bytes(bson::BsonView view) {
  return copy_bytes(view.span());
}

std::vector<std::uint8_t> bytes_from_bson(const bson_t& doc) {
  const auto* data = bson_get_data(&doc);
  return std::vector<std::uint8_t>(data, data + doc.len);
}

std::vector<std::uint8_t> wrap_iter_value(bson_iter_t iter) {
  bson_t wrap;
  bson_init(&wrap);
  bson_append_iter(&wrap, "v", -1, &iter);
  auto out = bytes_from_bson(wrap);
  bson_destroy(&wrap);
  return out;
}

template <typename Fn>
std::vector<std::uint8_t> make_wrapped_value(Fn&& append) {
  bson_t wrap;
  bson_init(&wrap);
  append(&wrap);
  auto out = bytes_from_bson(wrap);
  bson_destroy(&wrap);
  return out;
}

std::vector<std::uint8_t> wrap_null() {
  return make_wrapped_value([](bson_t* wrap) { bson_append_null(wrap, "v", -1); });
}

std::vector<std::uint8_t> wrap_utf8(std::string_view value) {
  return make_wrapped_value([&](bson_t* wrap) {
    bson_append_utf8(wrap, "v", -1, value.data(), static_cast<int>(value.size()));
  });
}

std::vector<std::uint8_t> wrap_int64(std::int64_t value) {
  return make_wrapped_value([&](bson_t* wrap) {
    bson_append_int64(wrap, "v", -1, value);
  });
}

bool append_wrapped_value(bson_t* out, std::string_view key,
                          const std::vector<std::uint8_t>& wrapped);
bool append_wrapped_array_item(bson_t* out, std::size_t index,
                               const std::vector<std::uint8_t>& wrapped);

bool unwrap_iter(const std::vector<std::uint8_t>& wrapped, bson_t* holder,
                 bson_iter_t* out) {
  if (!init_static_bson(wrapped, holder)) return false;
  return bson_iter_init_find(out, holder, "v");
}

bool is_truthy_projection_value(bson_iter_t iter) {
  switch (bson_iter_type(&iter)) {
    case BSON_TYPE_BOOL:
      return bson_iter_bool(&iter);
    case BSON_TYPE_INT32:
      return bson_iter_int32(&iter) != 0;
    case BSON_TYPE_INT64:
      return bson_iter_int64(&iter) != 0;
    case BSON_TYPE_DOUBLE:
      return bson_iter_double(&iter) != 0.0;
    default:
      return false;
  }
}

bool field_ref_from_iter(bson_iter_t iter, std::string* out) {
  if (bson_iter_type(&iter) != BSON_TYPE_UTF8) return false;
  std::uint32_t len = 0;
  const char* text = bson_iter_utf8(&iter, &len);
  if (!text || len == 0 || text[0] != '$') return false;
  out->assign(text + 1, len - 1);
  return true;
}

bool nullish_wrapped(const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return true;
  const auto type = bson_iter_type(&iter);
  return type == BSON_TYPE_NULL || type == BSON_TYPE_UNDEFINED;
}

std::optional<std::vector<std::uint8_t>> evaluate_expression(
    const bson_t& source_doc, const std::vector<std::uint8_t>& expr_bytes);

std::vector<std::vector<std::uint8_t>> evaluate_array_elements(
    const bson_t& source_doc, bson_iter_t array_iter) {
  std::vector<std::vector<std::uint8_t>> values;
  if (bson_iter_type(&array_iter) != BSON_TYPE_ARRAY) return values;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_array(&array_iter, &len, &data);
  bson_t arr;
  if (!bson_init_static(&arr, data, len)) return values;
  bson_iter_t it;
  if (!bson_iter_init(&it, &arr)) return values;
  while (bson_iter_next(&it)) {
    auto value = evaluate_expression(source_doc, wrap_iter_value(it));
    if (value) values.push_back(*value);
    else values.push_back({});
  }
  return values;
}

std::optional<std::vector<std::uint8_t>> evaluate_operator_expression(
    const bson_t& source_doc, const bson_t& expr_doc) {
  bson_iter_t op_it;
  if (!bson_iter_init(&op_it, &expr_doc) || !bson_iter_next(&op_it)) return std::nullopt;
  const char* op = bson_iter_key(&op_it);
  if (!op || op[0] != '$') return std::nullopt;

  if (std::string_view(op) == "$literal") {
    return wrap_iter_value(op_it);
  }

  if (std::string_view(op) == "$ifNull") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    for (const auto& arg : args) {
      if (!arg.empty() && !nullish_wrapped(arg)) return arg;
    }
    return wrap_null();
  }

  if (std::string_view(op) == "$concat") {
    const auto parts = evaluate_array_elements(source_doc, op_it);
    std::string out;
    for (const auto& part : parts) {
      if (part.empty() || nullish_wrapped(part)) return wrap_null();
      bson_t holder;
      bson_iter_t iter;
      if (!unwrap_iter(part, &holder, &iter)) return wrap_null();
      if (bson_iter_type(&iter) != BSON_TYPE_UTF8) return wrap_null();
      std::uint32_t len = 0;
      const char* text = bson_iter_utf8(&iter, &len);
      out.append(text, len);
    }
    return wrap_utf8(out);
  }

  if (std::string_view(op) == "$toString") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value || nullish_wrapped(*value)) return wrap_null();
    bson_t holder;
    bson_iter_t iter;
    if (!unwrap_iter(*value, &holder, &iter)) return wrap_null();
    switch (bson_iter_type(&iter)) {
      case BSON_TYPE_UTF8: {
        std::uint32_t len = 0;
        const char* text = bson_iter_utf8(&iter, &len);
        return wrap_utf8(std::string_view(text, len));
      }
      case BSON_TYPE_INT32:
        return wrap_utf8(std::to_string(bson_iter_int32(&iter)));
      case BSON_TYPE_INT64:
        return wrap_utf8(std::to_string(bson_iter_int64(&iter)));
      case BSON_TYPE_DOUBLE:
        return wrap_utf8(std::to_string(bson_iter_double(&iter)));
      case BSON_TYPE_BOOL:
        return wrap_utf8(bson_iter_bool(&iter) ? "true" : "false");
      case BSON_TYPE_OID: {
        char oid[25] = {};
        bson_oid_to_string(bson_iter_oid(&iter), oid);
        return wrap_utf8(oid);
      }
      default:
        return wrap_null();
    }
  }

  if (std::string_view(op) == "$size") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value) return std::nullopt;
    bson_t holder;
    bson_iter_t iter;
    if (!unwrap_iter(*value, &holder, &iter)) return std::nullopt;
    if (bson_iter_type(&iter) != BSON_TYPE_ARRAY) return std::nullopt;
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&iter, &len, &data);
    bson_t arr;
    if (!bson_init_static(&arr, data, len)) return std::nullopt;
    bson_iter_t arr_it;
    std::int64_t count = 0;
    if (bson_iter_init(&arr_it, &arr)) {
      while (bson_iter_next(&arr_it)) ++count;
    }
    return wrap_int64(count);
  }

  if (std::string_view(op) == "$arrayElemAt") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() < 2 || args[0].empty() || args[1].empty()) return std::nullopt;

    bson_t array_holder;
    bson_iter_t array_value;
    bson_t index_holder;
    bson_iter_t index_value;
    if (!unwrap_iter(args[0], &array_holder, &array_value) ||
        !unwrap_iter(args[1], &index_holder, &index_value)) {
      return std::nullopt;
    }
    if (bson_iter_type(&array_value) != BSON_TYPE_ARRAY ||
        !jungle::query::v1::is_numeric(bson_iter_type(&index_value))) {
      return std::nullopt;
    }

    const std::int64_t target = bson_iter_as_int64(&index_value);
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&array_value, &len, &data);
    bson_t arr;
    if (!bson_init_static(&arr, data, len)) return std::nullopt;
    bson_iter_t it;
    if (!bson_iter_init(&it, &arr)) return std::nullopt;

    std::vector<std::vector<std::uint8_t>> values;
    while (bson_iter_next(&it)) values.push_back(wrap_iter_value(it));
    if (values.empty()) return std::nullopt;

    std::int64_t index = target;
    if (index < 0) index = static_cast<std::int64_t>(values.size()) + index;
    if (index < 0 || index >= static_cast<std::int64_t>(values.size())) {
      return std::nullopt;
    }
    return values[static_cast<std::size_t>(index)];
  }

  return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> evaluate_expression(
    const bson_t& source_doc, const std::vector<std::uint8_t>& expr_bytes) {
  bson_t expr_holder;
  bson_iter_t expr_iter;
  if (!unwrap_iter(expr_bytes, &expr_holder, &expr_iter)) return std::nullopt;

  std::string field_path;
  if (field_ref_from_iter(expr_iter, &field_path)) {
    bson_iter_t value_iter;
    if (!jungle::query::v1::resolve_path(source_doc, field_path.c_str(), &value_iter)) {
      return std::nullopt;
    }
    return wrap_iter_value(value_iter);
  }
  if (bson_iter_type(&expr_iter) == BSON_TYPE_DOCUMENT) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&expr_iter, &len, &data);
    bson_t expr_doc;
    if (!bson_init_static(&expr_doc, data, len)) return std::nullopt;

    bson_iter_t maybe_op;
    if (bson_iter_init(&maybe_op, &expr_doc) && bson_iter_next(&maybe_op)) {
      const char* first_key = bson_iter_key(&maybe_op);
      if (first_key && first_key[0] == '$') {
        return evaluate_operator_expression(source_doc, expr_doc);
      }
    }

    bson_t built;
    bson_init(&built);
    bson_iter_t child;
    if (bson_iter_init(&child, &expr_doc)) {
      while (bson_iter_next(&child)) {
        const char* key = bson_iter_key(&child);
        if (!key) continue;
        auto child_value = evaluate_expression(source_doc, wrap_iter_value(child));
        if (!child_value) continue;
        append_wrapped_value(&built, key, *child_value);
      }
    }

    bson_t wrap;
    bson_init(&wrap);
    bson_append_document(&wrap, "v", -1, &built);
    auto out = bytes_from_bson(wrap);
    bson_destroy(&wrap);
    bson_destroy(&built);
    return out;
  }
  if (bson_iter_type(&expr_iter) == BSON_TYPE_ARRAY) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&expr_iter, &len, &data);
    bson_t expr_array;
    if (!bson_init_static(&expr_array, data, len)) return std::nullopt;

    bson_t built;
    bson_init(&built);
    bson_t arr;
    bson_append_array_begin(&built, "v", -1, &arr);
    bson_iter_t child;
    std::size_t index = 0;
    if (bson_iter_init(&child, &expr_array)) {
      while (bson_iter_next(&child)) {
        auto child_value = evaluate_expression(source_doc, wrap_iter_value(child));
        if (!child_value) continue;
        append_wrapped_array_item(&arr, index++, *child_value);
      }
    }
    bson_append_array_end(&built, &arr);
    auto out = bytes_from_bson(built);
    bson_destroy(&built);
    return out;
  }
  return expr_bytes;
}

std::optional<std::vector<std::uint8_t>> unwrap_document_bytes(
    const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return std::nullopt;
  if (bson_iter_type(&iter) != BSON_TYPE_DOCUMENT) return std::nullopt;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_document(&iter, &len, &data);
  return std::vector<std::uint8_t>(data, data + len);
}

std::optional<std::vector<std::uint8_t>> unwrap_array_bytes(
    const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return std::nullopt;
  if (bson_iter_type(&iter) != BSON_TYPE_ARRAY) return std::nullopt;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_array(&iter, &len, &data);
  return std::vector<std::uint8_t>(data, data + len);
}

bool append_wrapped_value(bson_t* out, std::string_view key,
                          const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return false;
  return bson_append_iter(out, key.data(), static_cast<int>(key.size()), &iter);
}

bool append_wrapped_array_item(bson_t* out, std::size_t index,
                               const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return false;
  const std::string key = std::to_string(index);
  return bson_append_iter(out, key.c_str(), -1, &iter);
}

bool top_level_only(std::string_view path) {
  return path.find('.') == std::string_view::npos;
}

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

std::vector<std::vector<std::uint8_t>> snapshot_iterator(
    jungle::storage::v1::Iterator& iter) {
  std::vector<std::vector<std::uint8_t>> docs;
  while (iter.has_next()) docs.push_back(owned_bytes(iter.next()));
  return docs;
}

std::vector<std::vector<std::uint8_t>> load_collection_docs(
    storage::IStorageBackend& owner, std::string_view db_name,
    std::string_view coll_name) {
  auto& foreign = owner.collection(db_name, coll_name);
  auto iter = foreign.find(empty_bson(), empty_bson(), 0, 0);
  return snapshot_iterator(*iter);
}

std::vector<std::uint8_t> clone_doc_with_array_field(
    std::span<const std::uint8_t> source_bytes, std::string_view field_name,
    const std::vector<std::vector<std::uint8_t>>& docs) {
  bson_t source;
  bson_init_static(&source, source_bytes.data(), source_bytes.size());

  bson_t out;
  bson_init(&out);
  bson_iter_t it;
  bson_iter_init(&it, &source);
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key || std::string_view(key) == field_name) continue;
    bson_append_iter(&out, key, -1, &it);
  }

  bson_t arr;
  bson_append_array_begin(&out, field_name.data(),
                          static_cast<int>(field_name.size()), &arr);
  for (std::size_t i = 0; i < docs.size(); ++i) {
    bson_t doc;
    if (!bson_init_static(&doc, docs[i].data(), docs[i].size())) continue;
    const std::string key = std::to_string(i);
    bson_append_document(&arr, key.c_str(), -1, &doc);
  }
  bson_append_array_end(&out, &arr);

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::uint8_t> clone_doc_with_replaced_field(
    std::span<const std::uint8_t> source_bytes, std::string_view field_name,
    const std::vector<std::uint8_t>& wrapped_value,
    std::optional<std::pair<std::string, std::int64_t>> array_index = std::nullopt) {
  bson_t source;
  bson_init_static(&source, source_bytes.data(), source_bytes.size());

  bson_t out;
  bson_init(&out);
  bson_iter_t it;
  bson_iter_init(&it, &source);
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key || std::string_view(key) == field_name ||
        (array_index && std::string_view(key) == array_index->first)) {
      continue;
    }
    bson_append_iter(&out, key, -1, &it);
  }

  append_wrapped_value(&out, field_name, wrapped_value);
  if (array_index) {
    bson_append_int64(&out, array_index->first.c_str(), -1, array_index->second);
  }

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::uint8_t> clone_doc_with_projection(
    std::span<const std::uint8_t> source_bytes, std::span<const std::uint8_t> spec_bytes) {
  bson_t spec;
  bson_t source;
  bson_init_static(&spec, spec_bytes.data(), spec_bytes.size());
  bson_init_static(&source, source_bytes.data(), source_bytes.size());

  bool computed = false;
  bson_iter_t sit;
  bson_iter_init(&sit, &spec);
  while (bson_iter_next(&sit)) {
    const char* key = bson_iter_key(&sit);
    if (!key || std::string_view(key) == "_id") continue;
    const auto type = bson_iter_type(&sit);
    const bool simple_bool = type == BSON_TYPE_BOOL || type == BSON_TYPE_INT32 ||
                             type == BSON_TYPE_INT64 || type == BSON_TYPE_DOUBLE;
    if (simple_bool) continue;
    computed = true;
    break;
  }
  if (!computed) {
    return jungle::query::v1::project(
        bson::BsonView(source_bytes), spec_bytes);
  }

  bool include_id = true;
  bool saw_exclusion = false;
  bson_iter_init(&sit, &spec);
  while (bson_iter_next(&sit)) {
    const char* key = bson_iter_key(&sit);
    if (!key) continue;
    if (std::string_view(key) == "_id") {
      include_id = is_truthy_projection_value(sit);
      continue;
    }
    const auto type = bson_iter_type(&sit);
    const bool numeric_bool_projection =
        type == BSON_TYPE_BOOL || type == BSON_TYPE_INT32 ||
        type == BSON_TYPE_INT64 || type == BSON_TYPE_DOUBLE;
    if (numeric_bool_projection && !is_truthy_projection_value(sit)) {
      saw_exclusion = true;
    }
  }
  if (saw_exclusion) {
    throw std::runtime_error("computed $project cannot mix inclusion and exclusion");
  }

  bson_t out;
  bson_init(&out);
  if (include_id) {
    bson_iter_t id_it;
    if (bson_iter_init_find(&id_it, &source, "_id")) {
      bson_append_iter(&out, "_id", -1, &id_it);
    }
  }

  bson_iter_init(&sit, &spec);
  while (bson_iter_next(&sit)) {
    const char* key = bson_iter_key(&sit);
    if (!key || std::string_view(key) == "_id") continue;
    if (!top_level_only(key)) {
      throw std::runtime_error("aggregate output dotted paths not implemented");
    }
    if (is_truthy_projection_value(sit)) {
      bson_iter_t value_it;
      if (jungle::query::v1::resolve_path(source, key, &value_it)) {
        bson_append_iter(&out, key, -1, &value_it);
      }
      continue;
    }
    auto evaluated = evaluate_expression(source, wrap_iter_value(sit));
    if (!evaluated) continue;
    append_wrapped_value(&out, key, *evaluated);
  }

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::uint8_t> clone_doc_with_add_fields(
    std::span<const std::uint8_t> source_bytes,
    std::span<const std::uint8_t> spec_bytes) {
  bson_t source;
  bson_t spec;
  bson_init_static(&source, source_bytes.data(), source_bytes.size());
  bson_init_static(&spec, spec_bytes.data(), spec_bytes.size());

  bson_t out;
  bson_init(&out);
  bson_iter_t it;
  bson_iter_init(&it, &source);
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key) continue;
    bson_append_iter(&out, key, -1, &it);
  }

  bson_iter_t sit;
  bson_iter_init(&sit, &spec);
  while (bson_iter_next(&sit)) {
    const char* key = bson_iter_key(&sit);
    if (!key) continue;
    if (!top_level_only(key)) {
      throw std::runtime_error("aggregate output dotted paths not implemented");
    }
    auto value = evaluate_expression(source, wrap_iter_value(sit));
    if (!value) continue;
    append_wrapped_value(&out, key, *value);
  }

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::uint8_t> clone_doc_with_unset_fields(
    std::span<const std::uint8_t> source_bytes,
    const std::unordered_set<std::string>& fields) {
  bson_t source;
  bson_init_static(&source, source_bytes.data(), source_bytes.size());

  bson_t out;
  bson_init(&out);
  bson_iter_t it;
  bson_iter_init(&it, &source);
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key || fields.contains(std::string(key))) continue;
    bson_append_iter(&out, key, -1, &it);
  }

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::vector<std::uint8_t>> apply_match_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes) {
  std::vector<std::vector<std::uint8_t>> out;
  for (const auto& doc : docs) {
    if (jungle::query::v1::matches(
            bson::BsonView(std::span<const std::uint8_t>{doc.data(), doc.size()}),
            spec_bytes)) {
      out.push_back(doc);
    }
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_sort_stage(
    std::vector<std::vector<std::uint8_t>> docs,
    std::span<const std::uint8_t> spec_bytes) {
  std::stable_sort(docs.begin(), docs.end(),
                   [&](const auto& left, const auto& right) {
                     return jungle::query::v1::sort_less(
                         bson::BsonView(std::span<const std::uint8_t>{left.data(), left.size()}),
                         bson::BsonView(std::span<const std::uint8_t>{right.data(), right.size()}),
                         spec_bytes);
                   });
  return docs;
}

std::vector<std::vector<std::uint8_t>> apply_skip_stage(
    std::vector<std::vector<std::uint8_t>> docs, std::size_t skip) {
  if (skip >= docs.size()) return {};
  docs.erase(docs.begin(), docs.begin() + static_cast<std::ptrdiff_t>(skip));
  return docs;
}

std::vector<std::vector<std::uint8_t>> apply_limit_stage(
    std::vector<std::vector<std::uint8_t>> docs, std::size_t limit) {
  if (limit == 0 || docs.size() <= limit) return docs;
  docs.resize(limit);
  return docs;
}

std::vector<std::vector<std::uint8_t>> apply_project_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes) {
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(docs.size());
  for (const auto& doc : docs) {
    out.push_back(clone_doc_with_projection(doc, spec_bytes));
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_add_fields_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes) {
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(docs.size());
  for (const auto& doc : docs) {
    out.push_back(clone_doc_with_add_fields(doc, spec_bytes));
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_unset_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    const std::unordered_set<std::string>& fields) {
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(docs.size());
  for (const auto& doc : docs) {
    out.push_back(clone_doc_with_unset_fields(doc, fields));
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_replace_root_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    const std::vector<std::uint8_t>& expr_bytes) {
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(docs.size());
  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());
    auto evaluated = evaluate_expression(source, expr_bytes);
    if (!evaluated) {
      throw std::runtime_error("$replaceRoot expression resolved to missing");
    }
    auto replacement = unwrap_document_bytes(*evaluated);
    if (!replacement) {
      throw std::runtime_error("$replaceRoot requires a document result");
    }
    out.push_back(*replacement);
  }
  return out;
}

struct GroupAccumulatorSpec {
  enum class Kind { Sum, First, Last, Push, Min, Max, Avg };
  std::string name;
  Kind kind{Kind::Sum};
  std::vector<std::uint8_t> expr_bytes;
};

struct GroupState {
  std::vector<std::uint8_t> id_value;
  std::vector<double> sums;
  std::vector<std::size_t> counts;
  std::vector<bool> first_seen;
  std::vector<bool> last_seen;
  std::vector<bool> value_seen;
  std::vector<std::vector<std::uint8_t>> first_values;
  std::vector<std::vector<std::uint8_t>> last_values;
  std::vector<std::vector<std::uint8_t>> values;
  std::vector<std::vector<std::vector<std::uint8_t>>> pushes;
};

int compare_wrapped_values(const std::vector<std::uint8_t>& left,
                           const std::vector<std::uint8_t>& right) {
  bson_t left_holder;
  bson_t right_holder;
  bson_iter_t left_it;
  bson_iter_t right_it;
  if (!unwrap_iter(left, &left_holder, &left_it) ||
      !unwrap_iter(right, &right_holder, &right_it)) {
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
  }
  if (jungle::query::v1::value_equal(left_it, right_it)) return 0;
  auto cmp = jungle::query::v1::value_compare(left_it, right_it);
  if (cmp) return *cmp;
  if (left < right) return -1;
  if (left > right) return 1;
  return 0;
}

std::vector<std::vector<std::uint8_t>> apply_group_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes) {
  bson_t spec;
  if (!init_static_bson(spec_bytes, &spec)) {
    throw std::runtime_error("$group requires a document");
  }

  bson_iter_t id_iter;
  if (!bson_iter_init_find(&id_iter, &spec, "_id")) {
    throw std::runtime_error("$group requires _id");
  }
  const auto id_expr = wrap_iter_value(id_iter);

  std::vector<GroupAccumulatorSpec> accumulators;
  bson_iter_t it;
  bson_iter_init(&it, &spec);
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key || std::string_view(key) == "_id") continue;
    if (bson_iter_type(&it) != BSON_TYPE_DOCUMENT) {
      throw std::runtime_error("$group accumulator must be a document");
    }
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&it, &len, &data);
    bson_t acc_doc;
    bson_init_static(&acc_doc, data, len);
    bson_iter_t acc_it;
    if (!bson_iter_init(&acc_it, &acc_doc) || !bson_iter_next(&acc_it)) {
      throw std::runtime_error("$group accumulator must contain one operator");
    }
    GroupAccumulatorSpec spec_item;
    spec_item.name = key;
    spec_item.expr_bytes = wrap_iter_value(acc_it);
    const std::string_view op = bson_iter_key(&acc_it);
    if (op == "$sum") spec_item.kind = GroupAccumulatorSpec::Kind::Sum;
    else if (op == "$first") spec_item.kind = GroupAccumulatorSpec::Kind::First;
    else if (op == "$last") spec_item.kind = GroupAccumulatorSpec::Kind::Last;
    else if (op == "$push") spec_item.kind = GroupAccumulatorSpec::Kind::Push;
    else if (op == "$min") spec_item.kind = GroupAccumulatorSpec::Kind::Min;
    else if (op == "$max") spec_item.kind = GroupAccumulatorSpec::Kind::Max;
    else if (op == "$avg") spec_item.kind = GroupAccumulatorSpec::Kind::Avg;
    else throw std::runtime_error("group accumulator not implemented");
    accumulators.push_back(std::move(spec_item));
  }

  std::unordered_map<std::string, GroupState> groups;
  std::vector<std::string> order;
  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());
    auto evaluated_id = evaluate_expression(source, id_expr).value_or(std::vector<std::uint8_t>{});
    bson_t id_holder;
    bson_iter_t id_value;
    if (!unwrap_iter(evaluated_id, &id_holder, &id_value)) continue;
    bson_t id_doc;
    bson_init(&id_doc);
    bson_append_iter(&id_doc, "_id", -1, &id_value);
    const auto group_key = bytes_from_bson(id_doc);
    bson_destroy(&id_doc);
    const std::string key(group_key.begin(), group_key.end());

    auto [state_it, inserted] = groups.try_emplace(key);
    if (inserted) {
      order.push_back(key);
      state_it->second.id_value = std::move(evaluated_id);
      state_it->second.sums.assign(accumulators.size(), 0.0);
      state_it->second.counts.assign(accumulators.size(), 0);
      state_it->second.first_seen.assign(accumulators.size(), false);
      state_it->second.last_seen.assign(accumulators.size(), false);
      state_it->second.value_seen.assign(accumulators.size(), false);
      state_it->second.first_values.resize(accumulators.size());
      state_it->second.last_values.resize(accumulators.size());
      state_it->second.values.resize(accumulators.size());
      state_it->second.pushes.resize(accumulators.size());
    }
    auto& state = state_it->second;

    for (std::size_t i = 0; i < accumulators.size(); ++i) {
      const auto& acc = accumulators[i];
      auto evaluated = evaluate_expression(source, acc.expr_bytes);
      if (acc.kind == GroupAccumulatorSpec::Kind::Sum) {
        if (!evaluated) continue;
        bson_t value_holder;
        bson_iter_t value_it;
        if (!unwrap_iter(*evaluated, &value_holder, &value_it)) continue;
        if (jungle::query::v1::is_numeric(bson_iter_type(&value_it))) {
          state.sums[i] += bson_iter_as_double(&value_it);
        }
        continue;
      }
      if (acc.kind == GroupAccumulatorSpec::Kind::Avg) {
        if (!evaluated) continue;
        bson_t value_holder;
        bson_iter_t value_it;
        if (!unwrap_iter(*evaluated, &value_holder, &value_it)) continue;
        if (jungle::query::v1::is_numeric(bson_iter_type(&value_it))) {
          state.sums[i] += bson_iter_as_double(&value_it);
          state.counts[i] += 1;
        }
        continue;
      }
      if (acc.kind == GroupAccumulatorSpec::Kind::First) {
        if (!state.first_seen[i] && evaluated) {
          state.first_seen[i] = true;
          state.first_values[i] = *evaluated;
        }
        continue;
      }
      if (acc.kind == GroupAccumulatorSpec::Kind::Last) {
        if (evaluated) {
          state.last_seen[i] = true;
          state.last_values[i] = *evaluated;
        }
        continue;
      }
      if (acc.kind == GroupAccumulatorSpec::Kind::Push && evaluated) {
        state.pushes[i].push_back(*evaluated);
        continue;
      }
      if ((acc.kind == GroupAccumulatorSpec::Kind::Min ||
           acc.kind == GroupAccumulatorSpec::Kind::Max) && evaluated) {
        if (!state.value_seen[i]) {
          state.value_seen[i] = true;
          state.values[i] = *evaluated;
          continue;
        }
        const int cmp = compare_wrapped_values(*evaluated, state.values[i]);
        if ((acc.kind == GroupAccumulatorSpec::Kind::Min && cmp < 0) ||
            (acc.kind == GroupAccumulatorSpec::Kind::Max && cmp > 0)) {
          state.values[i] = *evaluated;
        }
      }
    }
  }

  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(order.size());
  for (const auto& key : order) {
    const auto& state = groups.at(key);
    bson_t group_doc;
    bson_init(&group_doc);
    append_wrapped_value(&group_doc, "_id", state.id_value);
    for (std::size_t i = 0; i < accumulators.size(); ++i) {
      const auto& acc = accumulators[i];
      if (acc.kind == GroupAccumulatorSpec::Kind::Sum) {
        bson_append_double(&group_doc, acc.name.c_str(), -1, state.sums[i]);
      } else if (acc.kind == GroupAccumulatorSpec::Kind::Avg) {
        if (state.counts[i] == 0) {
          bson_append_null(&group_doc, acc.name.c_str(), -1);
        } else {
          bson_append_double(&group_doc, acc.name.c_str(), -1,
                             state.sums[i] / static_cast<double>(state.counts[i]));
        }
      } else if (acc.kind == GroupAccumulatorSpec::Kind::First) {
        if (state.first_seen[i]) {
          append_wrapped_value(&group_doc, acc.name, state.first_values[i]);
        } else {
          bson_append_null(&group_doc, acc.name.c_str(), -1);
        }
      } else if (acc.kind == GroupAccumulatorSpec::Kind::Last) {
        if (state.last_seen[i]) {
          append_wrapped_value(&group_doc, acc.name, state.last_values[i]);
        } else {
          bson_append_null(&group_doc, acc.name.c_str(), -1);
        }
      } else if (acc.kind == GroupAccumulatorSpec::Kind::Push) {
        bson_t arr;
        bson_append_array_begin(&group_doc, acc.name.c_str(), -1, &arr);
        for (std::size_t item = 0; item < state.pushes[i].size(); ++item) {
          append_wrapped_array_item(&arr, item, state.pushes[i][item]);
        }
        bson_append_array_end(&group_doc, &arr);
      } else if (acc.kind == GroupAccumulatorSpec::Kind::Min ||
                 acc.kind == GroupAccumulatorSpec::Kind::Max) {
        if (state.value_seen[i]) {
          append_wrapped_value(&group_doc, acc.name, state.values[i]);
        } else {
          bson_append_null(&group_doc, acc.name.c_str(), -1);
        }
      }
    }
    out.push_back(bytes_from_bson(group_doc));
    bson_destroy(&group_doc);
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_count_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::string_view field_name) {
  if (field_name.empty() || field_name[0] == '$' || !top_level_only(field_name)) {
    throw std::runtime_error("$count requires a top-level field name");
  }
  if (docs.empty()) return {};

  bson_t out;
  bson_init(&out);
  bson_append_int64(&out, field_name.data(), static_cast<int>(field_name.size()),
                    static_cast<std::int64_t>(docs.size()));
  std::vector<std::vector<std::uint8_t>> result;
  result.push_back(bytes_from_bson(out));
  bson_destroy(&out);
  return result;
}

std::vector<std::vector<std::uint8_t>> apply_sort_by_count_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    const std::vector<std::uint8_t>& expr_bytes) {
  struct CountState {
    std::vector<std::uint8_t> value;
    std::size_t count{0};
  };

  std::unordered_map<std::string, CountState> groups;
  std::vector<std::string> order;
  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());
    auto evaluated = evaluate_expression(source, expr_bytes);
    if (!evaluated) continue;
    std::string key(evaluated->begin(), evaluated->end());
    auto [it, inserted] = groups.try_emplace(key);
    if (inserted) {
      it->second.value = *evaluated;
      order.push_back(key);
    }
    it->second.count += 1;
  }

  std::vector<CountState> ordered;
  ordered.reserve(order.size());
  for (const auto& key : order) ordered.push_back(groups.at(key));
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const CountState& left, const CountState& right) {
                     return left.count > right.count;
                   });

  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(ordered.size());
  for (const auto& item : ordered) {
    bson_t doc;
    bson_init(&doc);
    append_wrapped_value(&doc, "_id", item.value);
    bson_append_int64(&doc, "count", -1, static_cast<std::int64_t>(item.count));
    out.push_back(bytes_from_bson(doc));
    bson_destroy(&doc);
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_lookup_stage(
    const std::vector<std::vector<std::uint8_t>>& docs, const bson_t& spec,
    storage::IStorageBackend& owner, std::string_view db_name) {
  bson_iter_t it;
  std::string from;
  std::string local_field;
  std::string foreign_field;
  std::string as;
  if (bson_iter_init_find(&it, &spec, "from")) {
    std::uint32_t len = 0;
    const char* s = bson_iter_utf8(&it, &len);
    from.assign(s, len);
  }
  if (bson_iter_init_find(&it, &spec, "localField")) {
    std::uint32_t len = 0;
    const char* s = bson_iter_utf8(&it, &len);
    local_field.assign(s, len);
  }
  if (bson_iter_init_find(&it, &spec, "foreignField")) {
    std::uint32_t len = 0;
    const char* s = bson_iter_utf8(&it, &len);
    foreign_field.assign(s, len);
  }
  if (bson_iter_init_find(&it, &spec, "as")) {
    std::uint32_t len = 0;
    const char* s = bson_iter_utf8(&it, &len);
    as.assign(s, len);
  }
  if (from.empty() || local_field.empty() || foreign_field.empty() || as.empty()) {
    throw std::runtime_error("$lookup currently requires from/localField/foreignField/as");
  }
  if (!top_level_only(as)) {
    throw std::runtime_error("$lookup dotted as paths not implemented");
  }

  const auto foreign_docs = load_collection_docs(owner, db_name, from);
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(docs.size());
  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());
    bson_iter_t local_it;
    const bool has_local =
        jungle::query::v1::resolve_path(source, local_field.c_str(), &local_it);

    std::vector<std::vector<std::uint8_t>> matches;
    for (const auto& foreign : foreign_docs) {
      bson_t foreign_doc;
      bson_init_static(&foreign_doc, foreign.data(), foreign.size());
      bson_iter_t foreign_it;
      if (!jungle::query::v1::resolve_path(
              foreign_doc, foreign_field.c_str(), &foreign_it)) {
        continue;
      }
      bool matched = false;
      if (has_local && bson_iter_type(&local_it) == BSON_TYPE_ARRAY) {
        bson_iter_t array_it;
        std::uint32_t len = 0;
        const std::uint8_t* data = nullptr;
        bson_iter_array(&local_it, &len, &data);
        bson_t array_doc;
        if (bson_init_static(&array_doc, data, len) &&
            bson_iter_init(&array_it, &array_doc)) {
          while (bson_iter_next(&array_it)) {
            if (jungle::query::v1::value_equal(array_it, foreign_it)) {
              matched = true;
              break;
            }
          }
        }
      } else if (has_local) {
        matched = jungle::query::v1::value_equal(local_it, foreign_it);
      }
      if (matched) matches.push_back(foreign);
    }
    out.push_back(clone_doc_with_array_field(doc, as, matches));
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> apply_unwind_stage(
    const std::vector<std::vector<std::uint8_t>>& docs, const bson_t& spec,
    bool spec_is_string) {
  std::string path;
  bool preserve_null_empty = false;
  std::optional<std::string> include_array_index;

  if (spec_is_string) {
    bson_iter_t it;
    bson_iter_init(&it, &spec);
    bson_iter_next(&it);
    std::uint32_t len = 0;
    const char* text = bson_iter_utf8(&it, &len);
    path.assign(text, len);
  } else {
    bson_iter_t it;
    if (bson_iter_init_find(&it, &spec, "path")) {
      std::uint32_t len = 0;
      const char* text = bson_iter_utf8(&it, &len);
      path.assign(text, len);
    }
    if (bson_iter_init_find(&it, &spec, "preserveNullAndEmptyArrays")) {
      preserve_null_empty = bson_iter_as_bool(&it);
    }
    if (bson_iter_init_find(&it, &spec, "includeArrayIndex")) {
      std::uint32_t len = 0;
      const char* text = bson_iter_utf8(&it, &len);
      include_array_index = std::string(text, len);
    }
  }
  if (!path.empty() && path[0] == '$') path.erase(path.begin());
  if (path.empty()) throw std::runtime_error("$unwind requires a path");
  if (!top_level_only(path)) {
    throw std::runtime_error("$unwind dotted paths not implemented");
  }

  std::vector<std::vector<std::uint8_t>> out;
  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());
    bson_iter_t value_it;
    if (!jungle::query::v1::resolve_path(source, path.c_str(), &value_it)) {
      if (preserve_null_empty) out.push_back(doc);
      continue;
    }

    if (bson_iter_type(&value_it) != BSON_TYPE_ARRAY) {
      if (include_array_index) {
        out.push_back(clone_doc_with_replaced_field(
            doc, path, wrap_iter_value(value_it),
            std::make_optional(std::make_pair(*include_array_index, static_cast<std::int64_t>(0)))));
      } else {
        out.push_back(doc);
      }
      continue;
    }

    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&value_it, &len, &data);
    bson_t array_doc;
    if (!bson_init_static(&array_doc, data, len)) continue;
    bson_iter_t array_it;
    if (!bson_iter_init(&array_it, &array_doc)) continue;
    std::size_t index = 0;
    bool emitted = false;
    while (bson_iter_next(&array_it)) {
      emitted = true;
      auto replacement = wrap_iter_value(array_it);
      std::optional<std::pair<std::string, std::int64_t>> idx =
          include_array_index
              ? std::make_optional(std::make_pair(*include_array_index,
                                                  static_cast<std::int64_t>(index)))
              : std::nullopt;
      out.push_back(clone_doc_with_replaced_field(doc, path, replacement, idx));
      ++index;
    }
    if (!emitted && preserve_null_empty) {
      std::optional<std::pair<std::string, std::int64_t>> idx =
          include_array_index
              ? std::make_optional(std::make_pair(*include_array_index, static_cast<std::int64_t>(-1)))
              : std::nullopt;
      out.push_back(clone_doc_with_replaced_field(
          doc, path, wrap_iter_value(value_it), idx));
    }
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> run_pipeline(
    std::vector<std::vector<std::uint8_t>> docs,
    std::span<const std::uint8_t> pipeline_bytes, storage::IStorageBackend& owner,
    std::string_view db_name) {
  bson_t wrapper;
  if (!init_static_bson(pipeline_bytes, &wrapper)) {
    throw std::runtime_error("aggregate requires a pipeline wrapper document");
  }
  bson_iter_t pipeline_it;
  if (!bson_iter_init_find(&pipeline_it, &wrapper, "pipeline") ||
      bson_iter_type(&pipeline_it) != BSON_TYPE_ARRAY) {
    throw std::runtime_error("aggregate requires a pipeline array");
  }

  std::uint32_t array_len = 0;
  const std::uint8_t* array_data = nullptr;
  bson_iter_array(&pipeline_it, &array_len, &array_data);
  bson_t pipeline;
  if (!bson_init_static(&pipeline, array_data, array_len)) {
    throw std::runtime_error("invalid aggregate pipeline");
  }

  bson_iter_t stage_array_it;
  if (!bson_iter_init(&stage_array_it, &pipeline)) {
    throw std::runtime_error("invalid aggregate pipeline array");
  }
  while (bson_iter_next(&stage_array_it)) {
    if (bson_iter_type(&stage_array_it) != BSON_TYPE_DOCUMENT) {
      throw std::runtime_error("each aggregation stage must be a document");
    }
    std::uint32_t stage_len = 0;
    const std::uint8_t* stage_data = nullptr;
    bson_iter_document(&stage_array_it, &stage_len, &stage_data);
    bson_t stage;
    if (!bson_init_static(&stage, stage_data, stage_len)) {
      throw std::runtime_error("invalid aggregation stage");
    }
    bson_iter_t op_it;
    if (!bson_iter_init(&op_it, &stage) || !bson_iter_next(&op_it)) {
      throw std::runtime_error("each aggregation stage must contain one operator");
    }
    const char* op = bson_iter_key(&op_it);
    if (!op) throw std::runtime_error("aggregation stage missing operator name");
    if (bson_iter_next(&op_it)) {
      throw std::runtime_error("each aggregation stage must contain exactly one operator");
    }

    bson_iter_init(&op_it, &stage);
    bson_iter_next(&op_it);
    if (std::string_view(op) == "$match") {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      docs = apply_match_stage(docs, {data, len});
    } else if (std::string_view(op) == "$sort") {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      docs = apply_sort_stage(std::move(docs), {data, len});
    } else if (std::string_view(op) == "$skip") {
      docs = apply_skip_stage(std::move(docs), static_cast<std::size_t>(std::max<std::int64_t>(0, bson_iter_as_int64(&op_it))));
    } else if (std::string_view(op) == "$limit") {
      docs = apply_limit_stage(std::move(docs), static_cast<std::size_t>(std::max<std::int64_t>(0, bson_iter_as_int64(&op_it))));
    } else if (std::string_view(op) == "$project") {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      docs = apply_project_stage(docs, {data, len});
    } else if (std::string_view(op) == "$set" ||
               std::string_view(op) == "$addFields") {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      docs = apply_add_fields_stage(docs, {data, len});
    } else if (std::string_view(op) == "$unset") {
      std::unordered_set<std::string> fields;
      if (bson_iter_type(&op_it) == BSON_TYPE_UTF8) {
        std::uint32_t len = 0;
        const char* text = bson_iter_utf8(&op_it, &len);
        fields.insert(std::string(text, len));
      } else if (bson_iter_type(&op_it) == BSON_TYPE_ARRAY) {
        std::uint32_t len = 0;
        const std::uint8_t* data = nullptr;
        bson_iter_array(&op_it, &len, &data);
        bson_t arr;
        bson_init_static(&arr, data, len);
        bson_iter_t arr_it;
        if (!bson_iter_init(&arr_it, &arr)) {
          throw std::runtime_error("$unset array is invalid");
        }
        while (bson_iter_next(&arr_it)) {
          if (bson_iter_type(&arr_it) != BSON_TYPE_UTF8) {
            throw std::runtime_error("$unset array must contain field names");
          }
          std::uint32_t item_len = 0;
          const char* text = bson_iter_utf8(&arr_it, &item_len);
          fields.insert(std::string(text, item_len));
        }
      } else {
        throw std::runtime_error("$unset requires a field name or array of field names");
      }
      for (const auto& field : fields) {
        if (!top_level_only(field)) {
          throw std::runtime_error("$unset dotted fields not implemented");
        }
      }
      docs = apply_unset_stage(docs, fields);
    } else if (std::string_view(op) == "$group") {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      docs = apply_group_stage(docs, {data, len});
    } else if (std::string_view(op) == "$count") {
      if (bson_iter_type(&op_it) != BSON_TYPE_UTF8) {
        throw std::runtime_error("$count requires a field name");
      }
      std::uint32_t len = 0;
      const char* text = bson_iter_utf8(&op_it, &len);
      docs = apply_count_stage(docs, std::string_view(text, len));
    } else if (std::string_view(op) == "$sortByCount") {
      docs = apply_sort_by_count_stage(docs, wrap_iter_value(op_it));
    } else if (std::string_view(op) == "$lookup") {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      bson_t spec;
      bson_init_static(&spec, data, len);
      docs = apply_lookup_stage(docs, spec, owner, db_name);
    } else if (std::string_view(op) == "$replaceRoot") {
      if (bson_iter_type(&op_it) != BSON_TYPE_DOCUMENT) {
        throw std::runtime_error("$replaceRoot requires a document");
      }
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      bson_t spec;
      bson_init_static(&spec, data, len);
      bson_iter_t spec_it;
      if (!bson_iter_init_find(&spec_it, &spec, "newRoot")) {
        throw std::runtime_error("$replaceRoot requires newRoot");
      }
      docs = apply_replace_root_stage(docs, wrap_iter_value(spec_it));
    } else if (std::string_view(op) == "$replaceWith") {
      docs = apply_replace_root_stage(docs, wrap_iter_value(op_it));
    } else if (std::string_view(op) == "$unwind") {
      if (bson_iter_type(&op_it) == BSON_TYPE_UTF8) {
        bson_t spec;
        bson_init(&spec);
        bson_append_iter(&spec, "path", -1, &op_it);
        docs = apply_unwind_stage(docs, spec, true);
        bson_destroy(&spec);
      } else {
        std::uint32_t len = 0;
        const std::uint8_t* data = nullptr;
        bson_iter_document(&op_it, &len, &data);
        bson_t spec;
        bson_init_static(&spec, data, len);
        docs = apply_unwind_stage(docs, spec, false);
      }
    } else {
      throw std::runtime_error(std::string("aggregation stage ") + op + " not implemented");
    }
  }
  return docs;
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

std::unique_ptr<jungle::storage::v1::Iterator> MemoryCollection::aggregate(
    std::span<const std::uint8_t> pipeline_bytes) {
  auto docs = snapshot_live_docs(slots_);
  docs = run_pipeline(std::move(docs), pipeline_bytes, owner_, db_name_);
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
