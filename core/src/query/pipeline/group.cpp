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

}  // namespace savannah::jungle::query::v1
