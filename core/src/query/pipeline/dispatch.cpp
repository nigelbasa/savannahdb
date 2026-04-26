#include "internal.h"

#include "savannah/query/expression.h"
#include "savannah/query/pipeline.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Pipeline dispatcher.
//
// run_pipeline parses the wire-format pipeline array, then for each stage
// hands the doc set to an apply_*_stage function. Each stage lives in its
// own TU under pipeline/ — this file holds only the routing loop, the
// shared `top_level_only` predicate, and the per-operator argument shaping
// needed to invoke stages with the right BSON slice / typed args.

namespace savannah::jungle::query::v1 {

bool top_level_only(std::string_view path) {
  return path.find('.') == std::string_view::npos;
}

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
