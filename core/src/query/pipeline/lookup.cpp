#include "internal.h"

#include "savannah/query/expression.h"
#include "savannah/query/value.h"
#include "savannah/storage/backend.h"

#include <bson/bson.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// $lookup — equality join against another collection.
//
// Loads the foreign collection into memory once per stage invocation
// (snapshot via the backend), then for each input doc walks the cached
// foreign set comparing the local field against the foreign field. The
// matched foreign docs are inlined as an array under `as`. Array-valued
// local fields fan out: any array element matching the foreign field
// counts as a match (one-shot, no dedup).

namespace savannah::jungle::query::v1 {

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

}  // namespace

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

}  // namespace savannah::jungle::query::v1
