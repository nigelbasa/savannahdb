#include "internal.h"

#include "savannah/bson/document.h"
#include "savannah/query/expression.h"
#include "savannah/query/filter.h"
#include "savannah/query/pipeline.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Pipeline dispatcher.
//
// run_pipeline parses the wire-format pipeline array, then for each stage
// hands the doc set to an apply_*_stage function. Simple stages live in
// their own translation units (match.cpp, shaping.cpp, project.cpp) and
// reach this file via internal.h forward declarations.
//
// Pass A of the pipeline split kept the gnarlier stages — $group,
// $sortByCount, $lookup, $unwind — co-located with the dispatcher because
// they share private helpers (clone_doc_with_array_field,
// clone_doc_with_replaced_field, compare_wrapped_values) that aren't worth
// promoting into internal.h until those stages move out together. Pass B
// will lift them into pipeline/{group,lookup,unwind}.cpp.

namespace savannah::jungle::query::v1 {

bool top_level_only(std::string_view path) {
  return path.find('.') == std::string_view::npos;
}

namespace {

std::vector<std::vector<std::uint8_t>> snapshot_iterator(
    jungle::storage::v1::Iterator& iter) {
  std::vector<std::vector<std::uint8_t>> docs;
  while (iter.has_next()) docs.push_back(owned_bytes(iter.next()));
  return docs;
}

std::vector<std::vector<std::uint8_t>> load_collection_docs(
    ::savannah::storage::IStorageBackend& owner, std::string_view db_name,
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
    ::savannah::storage::IStorageBackend& owner, std::string_view db_name) {
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

}  // namespace

std::vector<std::vector<std::uint8_t>> run_pipeline(
    std::vector<std::vector<std::uint8_t>> docs,
    std::span<const std::uint8_t> pipeline_bytes, ::savannah::storage::IStorageBackend& owner,
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

}  // namespace savannah::jungle::query::v1
