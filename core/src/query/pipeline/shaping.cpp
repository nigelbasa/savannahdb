#include "internal.h"

#include "savannah/bson/document.h"
#include "savannah/query/expression.h"
#include "savannah/query/sort.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

// $sort, $skip, $limit, $count — uniform shape: take a doc set, return a
// reshaped doc set without changing per-doc structure (except $count, which
// collapses to a single tally doc).

namespace savannah::jungle::query::v1 {

std::vector<std::vector<std::uint8_t>> apply_sort_stage(
    std::vector<std::vector<std::uint8_t>> docs,
    std::span<const std::uint8_t> spec_bytes) {
  std::stable_sort(docs.begin(), docs.end(),
                   [&](const auto& left, const auto& right) {
                     return jungle::query::v1::sort_less(
                         bson::BsonView(std::span<const std::uint8_t>{left.data(), left.size()}),
                         bson::BsonView(std::span<const std::uint8_t>{right.data(), right.size()}),
                         spec_bytes);
                   });
  return docs;
}

std::vector<std::vector<std::uint8_t>> apply_skip_stage(
    std::vector<std::vector<std::uint8_t>> docs, std::size_t skip) {
  if (skip >= docs.size()) return {};
  docs.erase(docs.begin(), docs.begin() + static_cast<std::ptrdiff_t>(skip));
  return docs;
}

std::vector<std::vector<std::uint8_t>> apply_limit_stage(
    std::vector<std::vector<std::uint8_t>> docs, std::size_t limit) {
  if (limit == 0 || docs.size() <= limit) return docs;
  docs.resize(limit);
  return docs;
}

std::vector<std::vector<std::uint8_t>> apply_count_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::string_view field_name) {
  if (field_name.empty() || field_name[0] == '$' || !top_level_only(field_name)) {
    throw std::runtime_error("$count requires a top-level field name");
  }
  if (docs.empty()) return {};

  bson_t out;
  bson_init(&out);
  bson_append_int64(&out, field_name.data(), static_cast<int>(field_name.size()),
                    static_cast<std::int64_t>(docs.size()));
  std::vector<std::vector<std::uint8_t>> result;
  result.push_back(bytes_from_bson(out));
  bson_destroy(&out);
  return result;
}

}  // namespace savannah::jungle::query::v1
