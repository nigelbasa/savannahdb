#pragma once

// MQL update spec evaluation. Slice E supports:
//   - operator-style with $set, $unset, $inc
//   - replacement docs (no $-prefixed top-level keys)
//   - upsert seeding from literal filter clauses
//
// Out of scope (Phase 0.3+): $push/$pull/$addToSet/$rename/$mul,
// dot-path field paths, positional `$` operator, $currentDate.

#include "savannah/bson/document.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace savannah::jungle::query::v1 {

struct UpdateOutcome {
  std::vector<std::uint8_t> bytes;  // rebuilt doc on success
  bool changed{false};              // true iff bytes != original (drives nModified)
  // Populated on rejection. Wire layer turns these into {ok:0, code, errmsg}.
  int err_code{0};
  std::string err_name;
  std::string err_message;
};

// Apply the spec to `original`. On error, bytes is empty and err_code != 0.
UpdateOutcome apply_update(bson::BsonView original,
                           std::span<const std::uint8_t> spec);

// Build a doc to insert when an upsert finds no matches. Copies literal
// (non-operator) fields from the filter, then applies the spec.
// `_id` is generated if neither side provides one.
UpdateOutcome seed_upsert(std::span<const std::uint8_t> filter,
                          std::span<const std::uint8_t> spec);

}  // namespace savannah::jungle::query::v1
