#pragma once

// Jungle query API, version 1: MQL filter evaluation.
// C1b scope: top-level field equality only. Operators ($eq/$gt/$in/$or/...)
// land in C2-C4. Nested-field dot paths are Phase 0.3.

#include "savannah/bson/document.h"

#include <cstdint>
#include <span>

namespace savannah::jungle::query::v1 {

// Returns true if `doc` matches every top-level field in `filter`.
// An empty filter (5-byte empty BSON doc) matches everything.
// Filter keys starting with '$' are ignored in C1b — operators come later.
bool matches(bson::BsonView doc, std::span<const std::uint8_t> filter);

}  // namespace savannah::jungle::query::v1
