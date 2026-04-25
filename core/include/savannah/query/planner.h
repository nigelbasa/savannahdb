#pragma once

// Index-aware query planning. Produces pure plan values (no iterators) that
// the storage backend turns into concrete index lookups. Keeping plans
// separate from iterator construction is what lets a single planner serve
// the in-memory backend today and the LMDB backend in Phase 0.5.
//
// Scope (Phase 0.3, F5-F7):
//   - One indexable equality clause OR one indexable range clause.
//   - Single-key index-backed sort.
//   - Multikey deferred (paths whose value is BSON_TYPE_ARRAY are skipped).

#include "savannah/index/manager.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savannah::jungle::query::v1 {

struct RangeBound {
  index::IndexedValue key;
  bool inclusive{false};
};

struct LookupPlan {
  std::string field_path;
  std::optional<index::IndexedValue> exact_key;     // set for equality
  std::optional<RangeBound> lower_bound;            // set for $gt/$gte
  std::optional<RangeBound> upper_bound;            // set for $lt/$lte
};

struct SortPlan {
  std::string field_path;
  bool ascending{true};
};

// True iff `sort_bytes` decodes to a non-empty BSON doc (i.e. the caller
// asked for at least one sort key). Empty doc → no sort, streaming preserved.
bool has_sort_spec(std::span<const std::uint8_t> sort_bytes);

// Returns a plan iff exactly one top-level filter clause is indexable.
// Falling back to a scan when 0 or >1 clauses are indexable preserves
// scan-vs-index byte parity (see F5 advisor note).
std::optional<LookupPlan> plan_index_lookup(
    const index::IndexManager& indexes,
    std::span<const std::uint8_t> filter_bytes);

// Returns a plan iff the sort spec has exactly one key and that field
// has an index. F7 only handles single-key index-backed sorts.
std::optional<SortPlan> plan_index_sort(
    const index::IndexManager& indexes,
    std::span<const std::uint8_t> sort_bytes);

// Materialize a plan into the slot id list the index recorded.
std::vector<std::size_t> snapshot_from_lookup_plan(
    const index::IndexManager& indexes, const LookupPlan& plan);

}  // namespace savannah::jungle::query::v1
