#pragma once

// MQL aggregation pipeline runner.
//
// `run_pipeline(docs, pipeline_bytes, backend, db_name)` consumes an
// already-snapshotted vector of owned BSON documents, applies the stages
// listed in `pipeline_bytes`, and returns the final result vector. The
// backend reference is for cross-collection access ($lookup); db_name is
// the database the source collection lives in.
//
// Stage coverage (Phase 0.4):
//   $match, $sort, $skip, $limit, $project, $set, $addFields, $unset,
//   $group (with $sum/$first/$last/$push/$min/$max/$avg accumulators),
//   $count, $sortByCount, $lookup, $unwind, $replaceRoot, $replaceWith.
//
// Expression coverage (in query/expression.h):
//   field refs ("$foo"), object/array literals (recursive), $literal,
//   $ifNull, $concat, $toString, $size, $arrayElemAt.
//
// The runner owns no storage. Snapshotting from the source collection is
// the backend's job (e.g. MemoryCollection::aggregate calls
// snapshot_live_docs(slots_) before delegating here). This split keeps
// pipeline logic backend-agnostic — Phase 0.5 LMDB will reuse it as-is.

#include "savannah/storage/backend.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace savannah::jungle::query::v1 {

std::vector<std::vector<std::uint8_t>> run_pipeline(
    std::vector<std::vector<std::uint8_t>> docs,
    std::span<const std::uint8_t> pipeline_bytes,
    ::savannah::storage::IStorageBackend& backend,
    std::string_view db_name);

}  // namespace savannah::jungle::query::v1
