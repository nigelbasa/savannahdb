#pragma once

#include <bson/bson.h>

namespace savannah::jungle::query::v1 {

// Missing fields sort as equivalent to explicit null / undefined. Keeping
// that rule in one helper lets scan sort, index key ordering, and future
// durable key encoders agree on the same nullish bucket.
bool is_nullish_type(bson_type_t t);

// True only for BSON types whose same-type ordering is fully defined by the
// current query layer. Ordered indexes may use those types directly; other
// types must fall back to scan sort if exact parity matters.
bool supports_total_same_type_order(bson_type_t t);

// Compares optional field values under SavannahDB's current sort semantics.
// `nullptr` means "field missing". Return value follows strcmp-style ordering:
// negative if a < b, positive if a > b, zero if the values are equal under
// the logical sort contract.
int compare_optional_values_for_sort(
    const bson_iter_t* a, const bson_iter_t* b, bool ascending);

}  // namespace savannah::jungle::query::v1
