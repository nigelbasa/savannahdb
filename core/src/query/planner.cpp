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

  // Pass 1 — collect every top-level equality clause (literal value, not
  // an operator subdoc). Used to try matching a compound index covering
  // multiple equalities at once.
  struct EqClause {
    std::string path;
    index::IndexedValue value;
  };
  std::vector<EqClause> equalities;
  std::size_t total_clauses = 0;
  {
    bson_iter_t it;
    if (!bson_iter_init(&it, &filter)) return std::nullopt;
    while (bson_iter_next(&it)) {
      const char* key = bson_iter_key(&it);
      if (!key) continue;
      if (key[0] == '$') continue;
      ++total_clauses;
      if (bson_iter_type(&it) == BSON_TYPE_ARRAY) continue;
      if (is_operator_subdoc(it)) continue;
      EqClause c{key, index::IndexedValue::from_iter(it)};
      equalities.push_back(std::move(c));
    }
  }

  // Compound match: if every top-level clause is an equality AND a
  // compound index covers a non-trivial prefix, use it. Building a key
  // that matches fewer fields than the index declares is still valid —
  // the planner just falls back to single-field below if that prefix is
  // 1 (since single-field is already covered there).
  if (!equalities.empty() && equalities.size() == total_clauses) {
    std::vector<std::string> eq_paths;
    eq_paths.reserve(equalities.size());
    for (const auto& e : equalities) eq_paths.push_back(e.path);
    const auto matched_prefix = indexes.match_compound_index(eq_paths);
    if (matched_prefix.size() >= 2) {
      LookupPlan plan;
      plan.compound_field_paths = matched_prefix;
      plan.compound_exact_key.reserve(matched_prefix.size());
      for (const auto& declared : matched_prefix) {
        for (const auto& e : equalities) {
          if (e.path == declared) {
            plan.compound_exact_key.push_back(e.value);
            break;
          }
        }
      }
      // Sanity: compound key must match the prefix length.
      if (plan.compound_exact_key.size() == matched_prefix.size()) {
        return plan;
      }
    }
  }

  // Single-field path — preserves the existing 1-clause invariant.
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
  if (!key || !indexes.has_path(key) || !indexes.supports_ordered_sort(key)) {
    return std::nullopt;
  }

  bool ascending = true;
  if (is_numeric(bson_iter_type(&it))) {
    ascending = bson_iter_as_int64(&it) >= 0;
  }

  // F7 only supports single-key index-backed sorts.
  if (bson_iter_next(&it)) return std::nullopt;
  return SortPlan{std::string(key), ascending};
}

std::vector<::savannah::storage::RecordId> snapshot_from_lookup_plan(
    const index::IndexManager& indexes, const LookupPlan& plan) {
  if (!plan.compound_field_paths.empty()) {
    const auto* record_ids = indexes.lookup_exact_compound(
        plan.compound_field_paths, plan.compound_exact_key);
    return record_ids ? *record_ids
                      : std::vector<::savannah::storage::RecordId>{};
  }
  if (plan.exact_key) {
    const auto* record_ids =
        indexes.lookup_exact(plan.field_path, *plan.exact_key);
    return record_ids ? *record_ids
                      : std::vector<::savannah::storage::RecordId>{};
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
