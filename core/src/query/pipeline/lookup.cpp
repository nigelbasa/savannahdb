#include "internal.h"

#include "savannah/query/expression.h"
#include "savannah/query/pipeline.h"
#include "savannah/query/value.h"
#include "savannah/storage/backend.h"

#include <bson/bson.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// $lookup - equality joins plus the pipeline form.
//
// The simple localField/foreignField path still snapshots the foreign
// collection once and compares in memory. For the pipeline form, we keep
// that same snapshot behavior but build a per-local-doc pipeline by
// substituting `$$let` variables into the foreign subpipeline, then reuse
// the normal pipeline runner on the foreign doc set.

namespace savannah::jungle::query::v1 {

namespace {

using LookupBindings = std::unordered_map<std::string, std::vector<std::uint8_t>>;

bool iter_is_explicit_null(bson_iter_t iter) {
  return bson_iter_type(&iter) == BSON_TYPE_NULL;
}

bool local_matches_foreign(bool has_local, bson_iter_t local_it,
                           bool has_foreign, bson_iter_t foreign_it) {
  if (!has_local) {
    return !has_foreign ||
           (has_foreign && iter_is_explicit_null(foreign_it));
  }

  if (bson_iter_type(&local_it) == BSON_TYPE_ARRAY) {
    bson_iter_t array_it;
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&local_it, &len, &data);
    bson_t array_doc;
    if (!(bson_init_static(&array_doc, data, len) &&
          bson_iter_init(&array_it, &array_doc))) {
      return false;
    }
    while (bson_iter_next(&array_it)) {
      if (!has_foreign) {
        if (iter_is_explicit_null(array_it)) return true;
        continue;
      }
      if (jungle::query::v1::value_equal(array_it, foreign_it)) return true;
    }
    return false;
  }

  if (!has_foreign) return iter_is_explicit_null(local_it);
  return jungle::query::v1::value_equal(local_it, foreign_it);
}

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

std::vector<std::vector<std::uint8_t>> filter_foreign_docs_for_local(
    const std::vector<std::vector<std::uint8_t>>& foreign_docs,
    bool has_local, bson_iter_t local_it, std::string_view foreign_field) {
  std::vector<std::vector<std::uint8_t>> matches;
  for (const auto& foreign : foreign_docs) {
    bson_t foreign_doc;
    bson_init_static(&foreign_doc, foreign.data(), foreign.size());
    bson_iter_t foreign_it{};
    const bool has_foreign = jungle::query::v1::resolve_path(
        foreign_doc, foreign_field.data(), &foreign_it);
    if (local_matches_foreign(has_local, local_it, has_foreign, foreign_it)) {
      matches.push_back(foreign);
    }
  }
  return matches;
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

  bson_array_builder_t* arr = nullptr;
  bson_append_array_builder_begin(&out, field_name.data(),
                                  static_cast<int>(field_name.size()), &arr);
  for (std::size_t i = 0; i < docs.size(); ++i) {
    bson_t doc;
    if (!bson_init_static(&doc, docs[i].data(), docs[i].size())) continue;
    bson_array_builder_append_document(arr, &doc);
  }
  bson_append_array_builder_end(&out, arr);

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::string read_utf8_field(const bson_t& spec, std::string_view key) {
  bson_iter_t it;
  if (!bson_iter_init_find(&it, &spec, key.data()) ||
      bson_iter_type(&it) != BSON_TYPE_UTF8) {
    return {};
  }
  std::uint32_t len = 0;
  const char* text = bson_iter_utf8(&it, &len);
  return std::string(text, len);
}

LookupBindings build_lookup_bindings(const bson_t& source_doc,
                                     const bson_t* let_doc) {
  LookupBindings bindings;
  if (!let_doc) return bindings;

  bson_iter_t it;
  if (!bson_iter_init(&it, let_doc)) return bindings;
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key) continue;
    bindings.emplace(
        key,
        evaluate_expression(source_doc, wrap_iter_value(it)).value_or(wrap_null()));
  }
  return bindings;
}

void append_substituted_value(bson_t* out, std::string_view key,
                              bson_iter_t source_iter,
                              const LookupBindings& bindings) {
  const auto append_nested_doc = [&](bool as_array) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    if (as_array) {
      bson_iter_array(&source_iter, &len, &data);
    } else {
      bson_iter_document(&source_iter, &len, &data);
    }
    bson_t source_doc;
    if (!bson_init_static(&source_doc, data, len)) {
      bson_append_iter(out, key.data(), static_cast<int>(key.size()), &source_iter);
      return;
    }

    if (as_array) {
      // Build into a temporary bson_t and attach as array — lets the
      // recursive callee keep its bson_t* signature without dragging the
      // array-builder API through every nesting level.
      bson_t temp;
      bson_init(&temp);
      bson_iter_t child;
      if (bson_iter_init(&child, &source_doc)) {
        while (bson_iter_next(&child)) {
          const char* child_key = bson_iter_key(&child);
          if (!child_key) continue;
          append_substituted_value(&temp, child_key, child, bindings);
        }
      }
      bson_append_array(out, key.data(), static_cast<int>(key.size()), &temp);
      bson_destroy(&temp);
      return;
    }

    bson_t nested;
    bson_append_document_begin(out, key.data(), static_cast<int>(key.size()), &nested);
    bson_iter_t child;
    if (bson_iter_init(&child, &source_doc)) {
      while (bson_iter_next(&child)) {
        const char* child_key = bson_iter_key(&child);
        if (!child_key) continue;
        append_substituted_value(&nested, child_key, child, bindings);
      }
    }
    bson_append_document_end(out, &nested);
  };

  if (bson_iter_type(&source_iter) == BSON_TYPE_UTF8) {
    std::uint32_t len = 0;
    const char* text = bson_iter_utf8(&source_iter, &len);
    if (text && len >= 2 && text[0] == '$' && text[1] == '$') {
      const std::string_view var_name(text + 2, len - 2);
      const auto bound = bindings.find(std::string(var_name));
      if (bound == bindings.end()) {
        throw std::runtime_error(
            std::string("$lookup referenced unknown let variable ") +
            std::string(var_name));
      }
      append_wrapped_value(out, key, bound->second);
      return;
    }
    bson_append_utf8(out, key.data(), static_cast<int>(key.size()), text, static_cast<int>(len));
    return;
  }

  if (bson_iter_type(&source_iter) == BSON_TYPE_DOCUMENT) {
    append_nested_doc(false);
    return;
  }
  if (bson_iter_type(&source_iter) == BSON_TYPE_ARRAY) {
    append_nested_doc(true);
    return;
  }

  bson_append_iter(out, key.data(), static_cast<int>(key.size()), &source_iter);
}

std::vector<std::uint8_t> build_lookup_pipeline_wrapper(
    bson_iter_t pipeline_iter, const LookupBindings& bindings) {
  bson_t built;
  bson_init(&built);

  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_array(&pipeline_iter, &len, &data);
  bson_t source_pipeline;
  // Stages are themselves documents, not scalars. We emit them as auto-keyed
  // entries by writing into a temporary bson_t and then attaching it as the
  // "pipeline" array — keeps append_substituted_value's bson_t* signature.
  bson_t pipeline_arr;
  bson_init(&pipeline_arr);
  if (bson_init_static(&source_pipeline, data, len)) {
    bson_iter_t stage_it;
    std::size_t idx = 0;
    if (bson_iter_init(&stage_it, &source_pipeline)) {
      while (bson_iter_next(&stage_it)) {
        const std::string key = std::to_string(idx++);
        append_substituted_value(&pipeline_arr, key, stage_it, bindings);
      }
    }
  }
  bson_append_array(&built, "pipeline", -1, &pipeline_arr);
  bson_destroy(&pipeline_arr);

  auto bytes = bytes_from_bson(built);
  bson_destroy(&built);
  return bytes;
}

}  // namespace

std::vector<std::vector<std::uint8_t>> apply_lookup_stage(
    const std::vector<std::vector<std::uint8_t>>& docs, const bson_t& spec,
    ::savannah::storage::IStorageBackend& owner, std::string_view db_name) {
  const std::string from = read_utf8_field(spec, "from");
  const std::string local_field = read_utf8_field(spec, "localField");
  const std::string foreign_field = read_utf8_field(spec, "foreignField");
  const std::string as = read_utf8_field(spec, "as");

  bson_iter_t pipeline_it{};
  const bool has_pipeline =
      bson_iter_init_find(&pipeline_it, &spec, "pipeline");
  if (has_pipeline && bson_iter_type(&pipeline_it) != BSON_TYPE_ARRAY) {
    throw std::runtime_error("$lookup pipeline must be an array");
  }

  bson_t let_doc;
  bson_iter_t let_it{};
  const bool has_let =
      bson_iter_init_find(&let_it, &spec, "let") &&
      bson_iter_type(&let_it) == BSON_TYPE_DOCUMENT;
  if (has_let) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&let_it, &len, &data);
    bson_init_static(&let_doc, data, len);
  }

  if (from.empty() || as.empty()) {
    throw std::runtime_error("$lookup requires from and as");
  }
  if (!top_level_only(as)) {
    throw std::runtime_error("$lookup dotted as paths not implemented");
  }

  const bool has_equality_join = !local_field.empty() || !foreign_field.empty();
  if (!has_pipeline && !has_equality_join) {
    throw std::runtime_error(
        "$lookup currently requires either localField/foreignField or pipeline");
  }
  if (has_equality_join && (local_field.empty() || foreign_field.empty())) {
    throw std::runtime_error(
        "$lookup requires both localField and foreignField when either is provided");
  }

  const auto foreign_docs = load_collection_docs(owner, db_name, from);
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(docs.size());
  for (const auto& doc : docs) {
    bson_t source;
    bson_init_static(&source, doc.data(), doc.size());

    bson_iter_t local_it{};
    const bool has_local = has_equality_join &&
        jungle::query::v1::resolve_path(source, local_field.c_str(), &local_it);

    std::vector<std::vector<std::uint8_t>> matches =
        has_equality_join
            ? filter_foreign_docs_for_local(foreign_docs, has_local, local_it,
                                            foreign_field)
            : foreign_docs;

    if (has_pipeline) {
      const auto bindings = build_lookup_bindings(source, has_let ? &let_doc : nullptr);
      const auto pipeline_bytes = build_lookup_pipeline_wrapper(pipeline_it, bindings);
      matches = run_pipeline(std::move(matches), pipeline_bytes, owner, db_name);
    }

    out.push_back(clone_doc_with_array_field(doc, as, matches));
  }
  return out;
}

}  // namespace savannah::jungle::query::v1
