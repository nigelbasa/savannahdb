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
  IndexedValue() = default;
  // Builds from the value `it` currently points at. Wraps in {v: <value>}.
  static IndexedValue from_iter(bson_iter_t it);
  // Initializes an iter at the wrapped value. False if malformed.
  bool get_iter(bson_iter_t* out) const;
  bool empty() const { return bytes_.empty(); }
  // Raw bytes — for debugging / equality on identical encodings.
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
};

struct IndexedValueLess {
  bool operator()(const IndexedValue& a, const IndexedValue& b) const;
};

struct IndexInfo {
  std::string name;
  std::string field_path;
  std::size_t entries{0};  // total indexed slots
};

class IndexManager {
 public:
  // Returns true if the index was created. Returns false (no-op) if an
  // index with that name already exists.
  bool create(std::string name, std::string field_path);

  bool drop(std::string_view name);

  std::vector<IndexInfo> list() const;

  // Walk every index and add the doc's value(s). Caller passes the slot
  // index so the index can record where the doc lives. Array-valued paths
  // are skipped (multikey deferred).
  void on_insert(std::size_t slot_idx, bson::BsonView doc);

  // Remove all entries pointing at slot_idx for the given doc. Used both
  // for erase and as the first half of an update (erase old + insert new).
  void on_erase(std::size_t slot_idx, bson::BsonView old_doc);

  // Backfill a single named index from the live slots. Used after create()
  // to populate from existing data.
  template <typename SlotRange>
  void backfill_one(std::string_view name, const SlotRange& slots);

 private:
  struct Entry {
    std::string field_path;
    std::map<IndexedValue, std::vector<std::size_t>, IndexedValueLess> by_value;
  };

  // Indexes by name. Pointer stability matters because we sometimes pass
  // Entry references around; `unique_ptr` keeps addresses stable across
  // map mutations.
  std::unordered_map<std::string, std::unique_ptr<Entry>> indexes_;
};

template <typename SlotRange>
void IndexManager::backfill_one(std::string_view name, const SlotRange& slots) {
  auto it = indexes_.find(std::string(name));
  if (it == indexes_.end()) return;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.deleted) continue;
    on_insert(i, slot.view);  // hits all indexes; redundant but cheap.
  }
}

}  // namespace savannah::index
