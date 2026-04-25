#include "savannah/query/planner.h"

#include "savannah/query/value.h"

#include <bson/bson.h>

#include <string_view>
#include <utility>

namespace savannah::jungle::query::v1 {

namespace {

// Detect the `{path: {$op: value, ...}}` shape — value is a BSON document
// whose first key starts with `$`. Used to decide whether a clause is a
// literal equality or an operator expression to parse for range bounds.
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

// Tighten a lower-bound candidate against the running plan. If the new
// candidate is strictly greater (or equal-but-stricter on inclusivity),
// it replaces the current bound. Returns false on cross-type compares so
// the planner can reject the whole plan rather than guess type ordering.
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
  auto cmp = value_compare(next_it, current_it);
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
  auto cmp = value_compare(next_it, current_it);
  if (!cmp) return false;
  if (*cmp < 0 || (*cmp == 0 && !inclusive && current->inclusive)) {
    current = std::move(next);
  }
  return true;
}

// Parse `{$gt: x, $lte: y, ...}` into the plan's lower/upper bounds.
// Non-range operators ($ne, $exists, ...) are ignored — IndexLookupIterator
// runs the full filter on candidates so they're enforced there.
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

  // Empty range (lower > upper, or equal but at least one exclusive) →
  // reject the plan so we don't open an iterator that yields nothing.
  if (plan->lower_bound && plan->upper_bound) {
    bson_iter_t lower_it, upper_it;
    if (!plan->lower_bound->key.get_iter(&lower_it) ||
        !plan->upper_bound->key.get_iter(&upper_it)) {
      return false;
    }
    auto cmp = value_compare(lower_it, upper_it);
    if (!cmp) return true;  // Cross-type rejected later by matches().
    if (*cmp > 0) return false;
    if (*cmp == 0 &&
        (!plan->lower_bound->inclusive || !plan->upper_bound->inclusive)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool has_sort_spec(std::span<const std::uint8_t> sort_bytes) {
  if (sort_bytes.size() < 5) return false;
  bson_t sort;
  if (!bson_init_static(&sort, sort_bytes.data(), sort_bytes.size())) return false;
  bson_iter_t it;
  return bson_iter_init(&it, &sort) && bson_iter_next(&it);
}

std::optional<LookupPlan> plan_index_lookup(
    const index::IndexManager& indexes,
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

std::optional<SortPlan> plan_index_sort(
    const index::IndexManager& indexes,
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
  if (is_numeric(bson_iter_type(&it))) {
    ascending = bson_iter_as_int64(&it) >= 0;
  }

  // F7 only supports single-key index-backed sorts.
  if (bson_iter_next(&it)) return std::nullopt;
  return SortPlan{std::string(key), ascending};
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

}  // namespace savannah::jungle::query::v1
