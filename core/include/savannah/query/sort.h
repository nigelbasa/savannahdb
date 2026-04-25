#pragma once

// MQL sort spec evaluation. Slice F2 supports:
//   - Single or multi-key sort: {field: 1, other: -1}
//   - Dotted field paths (shared with filter eval)
//   - Stable: ties preserve insertion order via std::stable_sort
//
// Cross-type compares return "equal" so the next sort key (or insertion
// order, on the last key) breaks the tie. MongoDB's full type-bracketing
// order is Phase 0.4+ territory.

#include "savannah/bson/document.h"

#include <cstdint>
#include <span>

namespace savannah::jungle::query::v1 {

// Returns true if `a` should sort before `b` under the given spec.
// Missing values sort *before* present ones in ascending order (Mongo's
// "null comes first" rule), inverted for descending keys.
bool sort_less(bson::BsonView a, bson::BsonView b,
               std::span<const std::uint8_t> sort_spec);

}  // namespace savannah::jungle::query::v1
