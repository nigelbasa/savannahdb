#pragma once

// Shared BSON value comparison primitives. Used by filter eval, sort,
// and (Phase 0.3 F4) the index comparator. Single source of truth
// prevents index-vs-scan result-set drift.
//
// Note: this header pulls in libbson; it is engine-internal and not
// part of the binding-facing API surface.

#include <bson/bson.h>

#include <cstdint>
#include <optional>

namespace savannah::jungle::query::v1 {

bool is_numeric(bson_type_t t);

// Equality with MongoDB's number semantics: int32/int64/double form one
// "Number" type for equality. Other types must match exactly. Subdocs and
// arrays compare byte-for-byte (order-sensitive — matches Mongo).
bool value_equal(bson_iter_t a, bson_iter_t b);

// Same-type ordered compare with numeric canonicalization. Returns nullopt
// for cross-type pairs so callers can decide behavior (filter: no match,
// sort: defer to next key, index: not applicable since indexes are typed).
std::optional<int> value_compare(bson_iter_t a, bson_iter_t b);

// MongoDB's BSON-type precedence for sort operations. Missing fields are
// handled at the caller site as equivalent to `null`; this helper ranks
// only concrete BSON values.
int sort_type_rank(bson_type_t t);

// Resolve a top-level or dotted field path against `doc`. *out is valid
// only on true. Single-segment keys go through bson_iter_find; dotted keys
// use libbson's descendant walker (handles array indices like "tags.0").
bool resolve_path(const bson_t& doc, const char* path, bson_iter_t* out);

}  // namespace savannah::jungle::query::v1
