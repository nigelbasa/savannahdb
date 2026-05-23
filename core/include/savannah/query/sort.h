#pragma once

// MQL sort spec evaluation. Slice F2 supports:
//   - Single or multi-key sort: {field: 1, other: -1}
//   - Dotted field paths (shared with filter eval)
//   - Stable: ties preserve insertion order via std::stable_sort
//
// Cross-type compares follow MongoDB's BSON type order, while missing
// fields sort as equivalent to explicit `null`.

#include "savannah/bson/document.h"

#include <cstdint>
#include <span>

namespace savannah::jungle::query::v1 {

// Returns true if `a` should sort before `b` under the given spec.
// Missing values sort as `null`: before numbers/strings/etc. in ascending
// order, after them in descending order, and equal to explicit `null`.
bool sort_less(bson::BsonView a, bson::BsonView b,
               std::span<const std::uint8_t> sort_spec);

}  // namespace savannah::jungle::query::v1
