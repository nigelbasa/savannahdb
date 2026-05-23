#pragma once

// Slice F4 IndexManager — per-collection index registry.
//
// Scope:
//   - Single-field indexes on a top-level or dotted path.
//   - Backfill on creation; auto-update on insert/update/erase.
//   - NO query planner yet — indexes exist but aren't queried (F5).
//
// Deferred (write in CLAUDE.md if not already noted):
//   - Multikey (array-valued indexed fields are silently skipped).
//   - Compound indexes (multi-field keys).
//   - Partial / sparse / TTL / unique indexes.
//   - Text / geo indexes (out of scope entirely).

#include "savannah/bson/document.h"
#include "savannah/storage/record_id.h"

#include <bson/bson.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savannah::index {

// Standalone value wrapper — stores `{v: <value>}` so we can re-init a
// libbson iter and reuse the shared value comparator. Avoids dragging in
// libbson's bson_value_t lifecycle (copy/destroy) at the call site.
class IndexedValue {
 public:
  enum class Kind {
    MissingOrNull,
    Present,
  };

  IndexedValue() = default;
  static IndexedValue missing_or_null();
  // Builds from the value `it` currently points at. Wraps in {v: <value>}.
  static IndexedValue from_iter(bson_iter_t it);
  // Initializes an iter at the wrapped value. False if malformed.
  bool get_iter(bson_iter_t* out) const;
  bool is_missing_or_null() const {
    return kind_ == Kind::MissingOrNull;
  }
  bool empty() const { return bytes_.empty(); }
  // Raw bytes — for debugging / equality on identical encodings.
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  Kind kind_{Kind::Present};
  std::vector<std::uint8_t> bytes_;
};

struct IndexedValueLess {
  bool operator()(const IndexedValue& a, const IndexedValue& b) const;
};

struct IndexInfo {
  std::string name;
  // For compound indexes, multiple paths in declaration order. Single-field
  // indexes have exactly one entry. The single `field_path` field below is
  // a convenience for the common case + back-compat with existing callers.
  std::vector<std::string> field_paths;
  std::string field_path;   // == field_paths[0] when single-field; empty if compound.
  bool unique{false};
  std::size_t entries{0};   // total indexed slots
};

// A compound key — one IndexedValue per indexed field, in declaration order.
// Single-field indexes use a 1-element MultiKey. Comparison is lexicographic
// across components, which gives us prefix-friendly range scans for free.
using MultiKey = std::vector<IndexedValue>;

struct MultiKeyLess {
  bool operator()(const MultiKey& a, const MultiKey& b) const;
};

struct IndexOptions {
  // Reject inserts/updates whose indexed key already exists for some other
  // record. Backfill on create_index also fails if the existing data has a
  // duplicate. False (the default) means no uniqueness enforcement.
  bool unique{false};
};

class IndexManager {
 public:
  // Returns true if the index was created. Returns false (no-op) if an
  // index with that name already exists.
  bool create(std::string name, std::vector<std::string> field_paths,
              IndexOptions options = {});
  // Single-field convenience overload — equivalent to the vector form with
  // a one-element list. Kept so existing call sites compile unchanged.
  bool create(std::string name, std::string field_path,
              IndexOptions options = {}) {
    std::vector<std::string> paths;
    paths.push_back(std::move(field_path));
    return create(std::move(name), std::move(paths), options);
  }

  // Check whether inserting (or updating to) `doc` would violate any unique
  // index. `record_id_to_skip` is the doc's own record id when updating in
  // place — skip collisions against itself. Returns the name of the first
  // violating index, or empty on no violation.
  std::string would_violate_unique(storage::RecordId record_id_to_skip,
                                   bson::BsonView doc) const;

  bool drop(std::string_view name);

  std::vector<IndexInfo> list() const;

  // Planner-facing helpers: indexes are named for DDL, but planning routes
  // by field path. has_path matches single-field indexes; pick_compound
  // finds the best compound index whose leading paths match the equality
  // keys in `equality_paths` (in any order).
  bool has_path(std::string_view field_path) const;
  bool supports_ordered_sort(std::string_view field_path) const;
  const std::vector<storage::RecordId>* lookup_exact(
      std::string_view field_path, const IndexedValue& key) const;
  std::vector<storage::RecordId> lookup_range(
      std::string_view field_path,
      const IndexedValue* lower_bound, bool lower_inclusive,
      const IndexedValue* upper_bound, bool upper_inclusive,
      bool descending) const;

  // Compound exact-match lookup. `field_paths` must match a registered
  // compound (or single-field) index exactly, in declaration order.
  // Returns nullptr if no matching index exists or no docs share the key.
  const std::vector<storage::RecordId>* lookup_exact_compound(
      const std::vector<std::string>& field_paths, const MultiKey& key) const;

  // Used by the planner to find an index whose declared field paths match
  // the given filter equality paths. Returns the matched paths in index
  // declaration order, or empty when no full match exists.
  std::vector<std::string> match_compound_index(
      const std::vector<std::string>& equality_paths) const;

  // Walk every index and add the doc's value(s). Caller passes the slot
  // index so the index can record where the doc lives. Array-valued paths
  // are skipped (multikey deferred).
  void on_insert(storage::RecordId record_id, bson::BsonView doc);

  // Remove all entries pointing at slot_idx for the given doc. Used both
  // for erase and as the first half of an update (erase old + insert new).
  void on_erase(storage::RecordId record_id, bson::BsonView old_doc);

  // Backfill a single named index from the live slots. Used after create()
  // to populate from existing data.
  template <typename SlotRange>
  bool backfill_one(std::string_view name, const SlotRange& slots);

 private:
  struct Entry {
    std::vector<std::string> field_paths;
    std::map<MultiKey, std::vector<storage::RecordId>, MultiKeyLess> by_value;
    bool supports_ordered_sort{true};
    bool unique{false};
  };

  Entry* find_by_path(std::string_view field_path);
  const Entry* find_by_path(std::string_view field_path) const;
  const Entry* find_by_paths(const std::vector<std::string>& field_paths) const;

  // Indexes by name. Pointer stability matters because we sometimes pass
  // Entry references around; `unique_ptr` keeps addresses stable across
  // map mutations.
  std::unordered_map<std::string, std::unique_ptr<Entry>> indexes_;
};

template <typename SlotRange>
bool IndexManager::backfill_one(std::string_view name, const SlotRange& slots) {
  auto it = indexes_.find(std::string(name));
  if (it == indexes_.end()) return false;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.deleted) continue;
    on_insert(slot.record_id, slot.view);  // hits all indexes; redundant but cheap.
  }
  return true;
}

}  // namespace savannah::index
