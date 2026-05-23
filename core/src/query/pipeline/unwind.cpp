#include "internal.h"

#include "savannah/query/expression.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// $unwind — explode an array-valued field into one output doc per element.
//
// Both shorthand ($unwind: "$path") and full-spec ($unwind: {path, ...})
// forms are accepted; the dispatcher pre-normalizes the shorthand into a
// `{path: ...}` BSON before calling here, with `spec_is_string` flagging
// that origin so we can route the parse correctly.
//
// preserveNullAndEmptyArrays: emit the source doc unchanged when the path
// is missing or resolves to an empty array.
// includeArrayIndex: write the (zero-based) array position to the named
// top-level field; the index is -1 for the preserved-empty-array emit.

namespace savannah::jungle::query::v1 {

namespace {

using ArrayIndexValue =
    std::optional<std::pair<std::string, std::optional<std::int64_t>>>;

void append_array_index(bson_t* out, const ArrayIndexValue& array_index) {
  if (!array_index) return;
  if (array_index->second) {
    bson_append_int64(out, array_index->first.c_str(), -1,
                      *array_index->second);
    return;
  }
  bson_append_null(out, array_index->first.c_str(), -1);
}

std::vector<std::uint8_t> clone_doc_with_replaced_field(
    std::span<const std::uint8_t> source_bytes, std::string_view field_name,
    const std::vector<std::uint8_t>& wrapped_value,
    ArrayIndexValue array_index = std::nullopt) {
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
  append_array_index(&out, array_index);

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::uint8_t> clone_doc_without_field(
    std::span<const std::uint8_t> source_bytes, std::string_view field_name,
    ArrayIndexValue array_index = std::nullopt) {
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

  append_array_index(&out, array_index);

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

std::vector<std::uint8_t> clone_doc_with_array_index(
    std::span<const std::uint8_t> source_bytes,
    ArrayIndexValue array_index) {
  bson_t source;
  bson_init_static(&source, source_bytes.data(), source_bytes.size());

  bson_t out;
  bson_init(&out);
  bson_iter_t it;
  bson_iter_init(&it, &source);
  while (bson_iter_next(&it)) {
    const char* key = bson_iter_key(&it);
    if (!key || (array_index && std::string_view(key) == array_index->first)) {
      continue;
    }
    bson_append_iter(&out, key, -1, &it);
  }

  append_array_index(&out, array_index);

  auto bytes = bytes_from_bson(out);
  bson_destroy(&out);
  return bytes;
}

}  // namespace

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
      if (preserve_null_empty) {
        ArrayIndexValue idx =
            include_array_index
                ? std::make_optional(std::make_pair(*include_array_index,
                                                    std::optional<std::int64_t>{}))
                : std::nullopt;
        if (idx) out.push_back(clone_doc_with_array_index(doc, idx));
        else out.push_back(doc);
      }
      continue;
    }

    if (bson_iter_type(&value_it) != BSON_TYPE_ARRAY) {
      if (include_array_index) {
        out.push_back(clone_doc_with_replaced_field(
            doc, path, wrap_iter_value(value_it),
            std::make_optional(std::make_pair(*include_array_index,
                                              std::optional<std::int64_t>{}))));
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
      ArrayIndexValue idx =
          include_array_index
              ? std::make_optional(std::make_pair(*include_array_index,
                                                  std::make_optional(
                                                      static_cast<std::int64_t>(index))))
              : std::nullopt;
      out.push_back(clone_doc_with_replaced_field(doc, path, replacement, idx));
      ++index;
    }
    if (!emitted && preserve_null_empty) {
      ArrayIndexValue idx =
          include_array_index
              ? std::make_optional(std::make_pair(*include_array_index,
                                                  std::optional<std::int64_t>{}))
              : std::nullopt;
      out.push_back(clone_doc_without_field(doc, path, idx));
    }
  }
  return out;
}

}  // namespace savannah::jungle::query::v1
