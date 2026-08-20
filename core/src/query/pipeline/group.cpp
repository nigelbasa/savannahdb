#include "internal.h"

#include "savannah/query/expression.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// $group + $sortByCount.
//
// Both stages bucket the input by an evaluated key, accumulate per-bucket
// state, then emit one output doc per bucket in first-seen order. They
// share the wrapped-value envelope discipline from query/expression.h —
// keys and accumulator inputs round-trip through `{v: <value>}` so we
// can compare arbitrary BSON values without dragging bson_value_t around.

namespace savannah::jungle::query::v1 {

namespace {

struct GroupAccumulatorSpec {
  enum class Kind { Sum, First, Last, Push, Min, Max, Avg };
  std::string name;
  Kind kind{Kind::Sum};
  std::vector<std::uint8_t> expr_bytes;
  // Pre-extracted `'$field.path'` for the fast accumulator path. Empty means
  // the expression is anything more complex (operator doc, literal, nested
  // expression) and we have to call the full evaluator per doc. The fast
  // path skips the wrap_iter_value heap alloc + memcpy per doc per
  // accumulator, which dominates the $group hot loop at scale.
  std::string fast_field;
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

// Compare two wrapped values via the canonical filter-side comparator.
// Falls back to byte-wise compare on envelope failure so $min/$max stay
// deterministic even on malformed inputs (which the smoke shouldn't hit).
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

}  // namespace

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

    // Pre-extract a simple `'$field.path'` so the hot loop can skip the
    // wrap_iter_value allocation per doc. Anything more complex (operator
    // doc, literal, nested expr) keeps fast_field empty and falls through
    // to the full evaluator path.
    {
      bson_t holder;
      bson_iter_t expr_it;
      if (unwrap_iter(spec_item.expr_bytes, &holder, &expr_it)) {
        std::string maybe_field;
        if (field_ref_from_iter(expr_it, &maybe_field)) {
          spec_item.fast_field = std::move(maybe_field);
        }
      }
    }

    accumulators.push_back(std::move(spec_item));
  }

  // Detect a constant `_id` (literals only — not field refs, not operator
  // docs). When the id is the same for every doc, we can skip the per-doc
  // id evaluation, id_doc construction, key allocation, and map lookup — and
  // accumulate into a single GroupState pointer.
  bool constant_id = false;
  std::vector<std::uint8_t> constant_id_value;
  std::string constant_id_key;
  {
    bson_t holder;
    bson_iter_t expr_it;
    if (unwrap_iter(id_expr, &holder, &expr_it)) {
      const auto type = bson_iter_type(&expr_it);
      bool is_op_doc = false;
      if (type == BSON_TYPE_DOCUMENT) {
        std::uint32_t len = 0;
        const std::uint8_t* data = nullptr;
        bson_iter_document(&expr_it, &len, &data);
        bson_t inner;
        bson_iter_t first;
        if (bson_init_static(&inner, data, len) &&
            bson_iter_init(&first, &inner) && bson_iter_next(&first)) {
          const char* k = bson_iter_key(&first);
          if (k && k[0] == '$') is_op_doc = true;
        }
      }
      std::string maybe_field;
      const bool is_field_ref = field_ref_from_iter(expr_it, &maybe_field);
      if (!is_field_ref && !is_op_doc) {
        bson_t empty_doc;
        bson_init(&empty_doc);
        auto v = evaluate_expression(empty_doc, id_expr).value_or(wrap_null());
        bson_destroy(&empty_doc);
        bson_t id_holder;
        bson_iter_t id_value_iter;
        if (unwrap_iter(v, &id_holder, &id_value_iter)) {
          bson_t id_doc;
          bson_init(&id_doc);
          bson_append_iter(&id_doc, "_id", -1, &id_value_iter);
          constant_id_key.assign(
              reinterpret_cast<const char*>(bson_get_data(&id_doc)),
              id_doc.len);
          bson_destroy(&id_doc);
          constant_id_value = std::move(v);
          constant_id = true;
        }
      }
    }
  }

  auto init_state = [&](GroupState& state, std::vector<std::uint8_t> id_value) {
    state.id_value = std::move(id_value);
    state.sums.assign(accumulators.size(), 0.0);
    state.counts.assign(accumulators.size(), 0);
    state.first_seen.assign(accumulators.size(), false);
    state.last_seen.assign(accumulators.size(), false);
    state.value_seen.assign(accumulators.size(), false);
    state.first_values.resize(accumulators.size());
    state.last_values.resize(accumulators.size());
    state.values.resize(accumulators.size());
    state.pushes.resize(accumulators.size());
  };

  std::unordered_map<std::string, GroupState> groups;
  std::vector<std::string> order;
  GroupState* single_state = nullptr;

  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());

    GroupState* state_ptr;
    if (constant_id) {
      if (!single_state) {
        auto [it, _] = groups.try_emplace(constant_id_key);
        order.push_back(constant_id_key);
        init_state(it->second, constant_id_value);
        single_state = &it->second;
      }
      state_ptr = single_state;
    } else {
      auto evaluated_id =
          evaluate_expression(source, id_expr).value_or(wrap_null());
      bson_t id_holder;
      bson_iter_t id_value;
      if (!unwrap_iter(evaluated_id, &id_holder, &id_value)) continue;
      bson_t id_doc;
      bson_init(&id_doc);
      bson_append_iter(&id_doc, "_id", -1, &id_value);
      const auto group_key = bytes_from_bson(id_doc);
      bson_destroy(&id_doc);
      std::string key(group_key.begin(), group_key.end());

      auto [state_it, inserted] = groups.try_emplace(key);
      if (inserted) {
        order.push_back(key);
        init_state(state_it->second, std::move(evaluated_id));
      }
      state_ptr = &state_it->second;
    }
    GroupState& state = *state_ptr;

    for (std::size_t i = 0; i < accumulators.size(); ++i) {
      const auto& acc = accumulators[i];

      // FAST PATH: simple field-path accumulator on numeric Sum/Avg. Skips
      // evaluate_expression entirely — no wrap_iter_value heap alloc, no
      // optional<vector> return, no second unwrap. Just resolve_path +
      // bson_iter_as_double + add. This is the inner loop of every
      // `{ $sum: '$x' }` / `{ $avg: '$x' }` pipeline.
      if (!acc.fast_field.empty() &&
          (acc.kind == GroupAccumulatorSpec::Kind::Sum ||
           acc.kind == GroupAccumulatorSpec::Kind::Avg)) {
        bson_iter_t fit;
        if (resolve_path(source, acc.fast_field.c_str(), &fit) &&
            jungle::query::v1::is_numeric(bson_iter_type(&fit))) {
          state.sums[i] += bson_iter_as_double(&fit);
          if (acc.kind == GroupAccumulatorSpec::Kind::Avg) state.counts[i] += 1;
        }
        continue;
      }

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
        if (!state.first_seen[i]) {
          state.first_seen[i] = true;
          state.first_values[i] = evaluated.value_or(wrap_null());
        }
        continue;
      }
      if (acc.kind == GroupAccumulatorSpec::Kind::Last) {
        state.last_seen[i] = true;
        state.last_values[i] = evaluated.value_or(wrap_null());
        continue;
      }
      if (acc.kind == GroupAccumulatorSpec::Kind::Push) {
        if (evaluated) state.pushes[i].push_back(*evaluated);
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
        bson_array_builder_t* arr = nullptr;
        bson_append_array_builder_begin(&group_doc, acc.name.c_str(), -1, &arr);
        for (std::size_t item = 0; item < state.pushes[i].size(); ++item) {
          append_wrapped_array_item(arr, state.pushes[i][item]);
        }
        bson_append_array_builder_end(&group_doc, arr);
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
    auto evaluated = evaluate_expression(source, expr_bytes).value_or(wrap_null());
    std::string key(evaluated.begin(), evaluated.end());
    auto [it, inserted] = groups.try_emplace(key);
    if (inserted) {
      it->second.value = evaluated;
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

}  // namespace savannah::jungle::query::v1
