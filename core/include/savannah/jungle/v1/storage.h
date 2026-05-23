#pragma once

// Jungle storage API, version 1.
// "Jungle" is SavannahDB's internal abstraction layer — keeping it versioned
// lets the wire/query layers evolve without breaking downstream consumers.

#include "savannah/bson/document.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <string>
#include <vector>

namespace savannah::index { class IndexManager; }

namespace savannah::jungle::storage::v1 {

struct InsertResult {
  std::size_t inserted_count{0};
  int err_code{0};
  std::string err_name;
  std::string err_message;
};

struct UpdateBatchResult {
  std::size_t matched{0};
  std::size_t modified{0};
  // BSON-encoded `_id` value bytes (the value, not a wrapping doc) of any
  // upsert that landed during this call. Drivers report these per-statement.
  std::vector<std::vector<std::uint8_t>> upserted_ids;
  // Mongo-style error surface so the wire layer can build {ok:0, code, errmsg}.
  int err_code{0};
  std::string err_name;
  std::string err_message;
};

struct EraseResult {
  std::size_t deleted{0};
  int err_code{0};
  std::string err_name;
  std::string err_message;
};

struct IndexMutationResult {
  bool changed{false};
  int err_code{0};
  std::string err_name;
  std::string err_message;
};

struct CreateIndexOptions {
  bool unique{false};
};

class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool has_next() = 0;
  virtual bson::BsonView next() = 0;
};

class Collection {
 public:
  virtual ~Collection() = default;

  virtual InsertResult insert(std::span<const bson::BsonView> docs) = 0;

  virtual std::unique_ptr<Iterator> find(
      std::span<const std::uint8_t> filter_bytes,
      std::span<const std::uint8_t> sort_bytes,
      std::size_t skip, std::size_t limit) = 0;

  // Aggregation pipeline entry point. Returns an iterator over owned BSON
  // result docs so getMore can stream transformed batches the same way it
  // streams find() results.
  virtual std::unique_ptr<Iterator> aggregate(
      std::span<const std::uint8_t> pipeline_bytes) = 0;

  // Apply `spec` (operator-style or replacement) to docs matching `filter`.
  // multi=false stops at the first match. upsert=true seeds + inserts a new
  // doc if no docs match (using literal filter clauses + spec).
  virtual UpdateBatchResult update(
      std::span<const std::uint8_t> filter_bytes,
      std::span<const std::uint8_t> spec_bytes,
      bool multi, bool upsert) = 0;

  // single=true deletes only the first match (Mongo's limit:1).
  virtual EraseResult erase(
      std::span<const std::uint8_t> filter_bytes, bool single) = 0;

  // Index registry — every backend exposes one, even if (initially) empty.
  // The binding uses this to handle createIndex/dropIndex/listIndexes
  // without downcasting to a concrete backend.
  //
  // For compound indexes, pass multiple field paths in declaration order;
  // single-field indexes pass a one-element list. Backends that don't
  // implement compound natively can validate field_paths.size() == 1 and
  // return an error.
  virtual IndexMutationResult create_index(
      std::string_view name,
      std::span<const std::string> field_paths,
      CreateIndexOptions options = {}) = 0;
  // Single-field convenience overload — wraps the path and delegates.
  IndexMutationResult create_index(
      std::string_view name, std::string_view field_path,
      CreateIndexOptions options = {}) {
    std::string owned(field_path);
    std::array<std::string, 1> paths{std::move(owned)};
    return create_index(name, std::span<const std::string>(paths), options);
  }
  virtual IndexMutationResult drop_index(std::string_view name) = 0;
  virtual savannah::index::IndexManager& indexes() = 0;

  // Backend-owned backfill keeps the binding decoupled from slot/layout
  // details. Memory walks tombstoned slots; LMDB will iterate its store.
  virtual bool backfill_index(std::string_view name) = 0;
};

}  // namespace savannah::jungle::storage::v1
