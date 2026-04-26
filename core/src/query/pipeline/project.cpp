#include "internal.h"

#include "savannah/bson/document.h"
#include "savannah/query/expression.h"
#include "savannah/query/filter.h"
#include "savannah/query/project.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Document-shaping stages that rewrite each input doc:
//   $project / $set / $addFields / $unset / $replaceRoot / $replaceWith.
//
// All five share the same per-doc loop; the differences live in the
// clone_doc_with_* helpers, kept private to this TU.

namespace savannah::jungle::query::v1 {

namespace {

std::vector<std::uint8_t> clone_doc_with_projection(
    std::span<const std::uint8_t> source_bytes, std::span<const std::uint8_t> spec_bytes) {
  bson_t spec;
  bson_t source;
  bson_init_static(&spec, spec_bytes.data(), spec_bytes.size());
  bson_init_static(&source, source_bytes.data(), source_bytes.size());

  // Fast path: pure inclusion/exclusion projection — delegate to the
  // existing project module. Computed projections (any non-bool/non-numeric
  // value) need expression evaluation, handled below.
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

}  // namespace

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

}  // namespace savannah::jungle::query::v1
