#include "internal.h"

#include "savannah/bson/document.h"
#include "savannah/query/filter.h"

#include <cstdint>
#include <span>
#include <vector>

namespace savannah::jungle::query::v1 {

std::vector<std::vector<std::uint8_t>> apply_match_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes) {
  std::vector<std::vector<std::uint8_t>> out;
  for (const auto& doc : docs) {
    if (jungle::query::v1::matches(
            bson::BsonView(std::span<const std::uint8_t>{doc.data(), doc.size()}),
            spec_bytes)) {
      out.push_back(doc);
    }
  }
  return out;
}

}  // namespace savannah::jungle::query::v1
