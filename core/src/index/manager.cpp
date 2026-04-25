#include "savannah/index/manager.h"

#include "savannah/query/value.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace savannah::index {

namespace {

int value_type_class(bson_iter_t it) {
  const bson_type_t type = bson_iter_type(&it);
  if (jungle::query::v1::is_numeric(type)) return 1;
  return static_cast<int>(type);
}

}  // namespace

// ---------------------------------------------------------------------------
// IndexedValue
// ---------------------------------------------------------------------------

IndexedValue IndexedValue::from_iter(bson_iter_t it) {
  IndexedValue out;
  bson_t wrap;
  bson_init(&wrap);
  // Append the value under a fixed key so the comparator can find it.
  bson_append_iter(&wrap, "v", -1, &it);
  const std::uint8_t* data = bson_get_data(&wrap);
  out.bytes_.assign(data, data + wrap.len);
  bson_destroy(&wrap);
  return out;
}

bool IndexedValue::get_iter(bson_iter_t* out) const {
  if (bytes_.empty()) return false;
  bson_t b;
  if (!bson_init_static(&b, bytes_.data(), bytes_.size())) return false;
  if (!bson_iter_init(out, &b)) return false;
  return bson_iter_next(out) && std::string_view(bson_iter_key(out)) == "v";
}

bool IndexedValueLess::operator()(const IndexedValue& a,
                                  const IndexedValue& b) const {
  bson_iter_t ai, bi;
  const bool ga = a.get_iter(&ai);
  const bool gb = b.get_iter(&bi);
  if (!ga || !gb) {
    // Defensive: degenerate keys sort by raw bytes for stable ordering.
    return std::lexicographical_compare(a.bytes().begin(), a.bytes().end(),
                                         b.bytes().begin(), b.bytes().end());
  }
  const int class_a = value_type_class(ai);
  const int class_b = value_type_class(bi);
  if (class_a != class_b) return class_a < class_b;
  auto cmp = jungle::query::v1::value_compare(ai, bi);
  if (cmp) return *cmp < 0;
  // Same logical class but no richer ordering (e.g. documents/arrays):
  // fall back to raw bytes for a stable strict weak ordering.
  return std::lexicographical_compare(a.bytes().begin(), a.bytes().end(),
                                       b.bytes().begin(), b.bytes().end());
}

// ---------------------------------------------------------------------------
// IndexManager
// ---------------------------------------------------------------------------

bool IndexManager::create(std::string name, std::string field_path) {
  if (indexes_.contains(name)) return false;
  auto entry = std::make_unique<Entry>();
  entry->field_path = std::move(field_path);
  indexes_.emplace(std::move(name), std::move(entry));
  return true;
}

IndexManager::Entry* IndexManager::find_by_path(std::string_view field_path) {
  for (auto& [_, entry] : indexes_) {
    if (entry->field_path == field_path) return entry.get();
  }
  return nullptr;
}

const IndexManager::Entry* IndexManager::find_by_path(
    std::string_view field_path) const {
  for (const auto& [_, entry] : indexes_) {
    if (entry->field_path == field_path) return entry.get();
  }
  return nullptr;
}

bool IndexManager::drop(std::string_view name) {
  auto it = indexes_.find(std::string(name));
  if (it == indexes_.end()) return false;
  indexes_.erase(it);
  return true;
}

bool IndexManager::has_path(std::string_view field_path) const {
  return find_by_path(field_path) != nullptr;
}

const std::vector<std::size_t>* IndexManager::lookup_exact(
    std::string_view field_path, const IndexedValue& key) const {
  const Entry* entry = find_by_path(field_path);
  if (!entry) return nullptr;
  auto it = entry->by_value.find(key);
  if (it == entry->by_value.end()) return nullptr;
  return &it->second;
}

std::vector<std::size_t> IndexManager::lookup_range(
    std::string_view field_path,
    const IndexedValue* lower_bound, bool lower_inclusive,
    const IndexedValue* upper_bound, bool upper_inclusive,
    bool descending) const {
  std::vector<std::size_t> out;
  const Entry* entry = find_by_path(field_path);
  if (!entry) return out;

  auto begin = entry->by_value.begin();
  auto end = entry->by_value.end();

  if (lower_bound) {
    begin = lower_inclusive ? entry->by_value.lower_bound(*lower_bound)
                            : entry->by_value.upper_bound(*lower_bound);
  }
  if (upper_bound) {
    end = upper_inclusive ? entry->by_value.upper_bound(*upper_bound)
                          : entry->by_value.lower_bound(*upper_bound);
  }

  if (!descending) {
    for (auto it = begin; it != end; ++it) {
      out.insert(out.end(), it->second.begin(), it->second.end());
    }
    return out;
  }

  using ConstReverseIt =
      std::map<IndexedValue, std::vector<std::size_t>, IndexedValueLess>::const_reverse_iterator;
  ConstReverseIt rbegin(end);
  ConstReverseIt rend(begin);
  for (auto it = rbegin; it != rend; ++it) {
    out.insert(out.end(), it->second.begin(), it->second.end());
  }
  return out;
}

std::vector<IndexInfo> IndexManager::list() const {
  std::vector<IndexInfo> out;
  out.reserve(indexes_.size());
  for (const auto& [name, entry] : indexes_) {
    std::size_t entries = 0;
    for (const auto& [_, slots] : entry->by_value) entries += slots.size();
    out.push_back(IndexInfo{name, entry->field_path, entries});
  }
  return out;
}

namespace {

// Look up the indexed value for a doc; returns false if missing or if the
// value is array-typed (multikey deferred — index just skips the slot).
bool extract_indexed_value(bson::BsonView doc, std::string_view field_path,
                           IndexedValue* out) {
  bson_t d;
  if (!bson_init_static(&d, doc.data(), doc.size())) return false;
  bson_iter_t it;
  if (!jungle::query::v1::resolve_path(d, std::string(field_path).c_str(), &it)) {
    return false;
  }
  if (bson_iter_type(&it) == BSON_TYPE_ARRAY) return false;  // multikey: skip.
  *out = IndexedValue::from_iter(it);
  return true;
}

}  // namespace

void IndexManager::on_insert(std::size_t slot_idx, bson::BsonView doc) {
  for (auto& [name, entry] : indexes_) {
    IndexedValue key;
    if (!extract_indexed_value(doc, entry->field_path, &key)) continue;
    entry->by_value[std::move(key)].push_back(slot_idx);
  }
}

void IndexManager::on_erase(std::size_t slot_idx, bson::BsonView old_doc) {
  for (auto& [name, entry] : indexes_) {
    IndexedValue key;
    if (!extract_indexed_value(old_doc, entry->field_path, &key)) continue;
    auto it = entry->by_value.find(key);
    if (it == entry->by_value.end()) continue;
    auto& slots = it->second;
    auto pos = std::find(slots.begin(), slots.end(), slot_idx);
    if (pos != slots.end()) slots.erase(pos);
    if (slots.empty()) entry->by_value.erase(it);
  }
}

}  // namespace savannah::index
