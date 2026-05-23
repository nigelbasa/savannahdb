#include "savannah/index/manager.h"

#include "savannah/query/key_order.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace savannah::index {

namespace {

// One component of a compound key, possibly fanning out across array
// elements (multikey). `values` empty means "skip this doc for this index"
// — e.g., the path resolved to BSON we can't sort/compare.
struct ComponentValues {
  std::vector<IndexedValue> values;
  bool is_array{false};
};

bool extract_component(bson::BsonView doc, std::string_view field_path,
                       ComponentValues* out, bool* supports_ordered_sort) {
  bson_t d;
  if (!bson_init_static(&d, doc.data(), doc.size())) return false;
  bson_iter_t it;
  if (!jungle::query::v1::resolve_path(d, std::string(field_path).c_str(), &it)) {
    out->values.push_back(IndexedValue::missing_or_null());
    return true;
  }
  const bson_type_t type = bson_iter_type(&it);
  if (type == BSON_TYPE_ARRAY) {
    // Multikey: emit one indexed entry per array element. Mongo also
    // indexes the array itself as a single key — we skip that for now;
    // {field: arrayValue} exact-equality queries against a literal array
    // are vanishingly rare and require a different lookup shape.
    if (supports_ordered_sort) *supports_ordered_sort = false;
    out->is_array = true;
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&it, &alen, &adata);
    bson_t arr;
    if (!bson_init_static(&arr, adata, alen)) return false;
    bson_iter_t ait;
    if (!bson_iter_init(&ait, &arr)) return false;
    while (bson_iter_next(&ait)) {
      // Skip nested arrays — multikey-of-multikey isn't supported.
      if (bson_iter_type(&ait) == BSON_TYPE_ARRAY) continue;
      out->values.push_back(IndexedValue::from_iter(ait));
    }
    if (out->values.empty()) {
      // Empty array: index as a single nullish entry so the doc remains
      // discoverable via {field: {$exists: false}} style filters.
      out->values.push_back(IndexedValue::missing_or_null());
    }
    return true;
  }
  if (supports_ordered_sort &&
      !jungle::query::v1::is_nullish_type(type) &&
      !jungle::query::v1::supports_total_same_type_order(type)) {
    *supports_ordered_sort = false;
  }
  out->values.push_back(IndexedValue::from_iter(it));
  return true;
}

// Expand a doc into every MultiKey it should be indexed under for a given
// index. For single-field non-array, that's one key. For an indexed array
// field, one key per element. For compound, the cross product — capped to
// one array component (Mongo's restriction; deeper cross-products explode
// index size and are rarely useful).
//
// Returns empty when the doc is unindexable (e.g., compound with two array
// fields, or extraction failed).
std::vector<MultiKey> expand_doc_keys(bson::BsonView doc,
                                      const std::vector<std::string>& paths,
                                      bool* supports_ordered_sort) {
  std::vector<ComponentValues> components;
  components.reserve(paths.size());
  int array_components = 0;
  for (const auto& path : paths) {
    ComponentValues cv;
    if (!extract_component(doc, path, &cv, supports_ordered_sort)) {
      return {};
    }
    if (cv.is_array) ++array_components;
    components.push_back(std::move(cv));
  }
  if (array_components > 1) {
    // Compound multikey across more than one array field would generate
    // |A|*|B| index entries per doc. Mongo rejects this; we do too.
    return {};
  }

  // Cross product: each component contributes some N values; we emit
  // ∏ N keys. With ≤1 array component this stays linear.
  std::vector<MultiKey> out;
  out.push_back({});  // start with one empty key
  for (const auto& cv : components) {
    std::vector<MultiKey> next;
    next.reserve(out.size() * cv.values.size());
    for (const auto& prefix : out) {
      for (const auto& v : cv.values) {
        MultiKey k = prefix;
        k.push_back(v);
        next.push_back(std::move(k));
      }
    }
    out = std::move(next);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// IndexedValue
// ---------------------------------------------------------------------------

IndexedValue IndexedValue::missing_or_null() {
  IndexedValue out;
  out.kind_ = Kind::MissingOrNull;
  return out;
}

IndexedValue IndexedValue::from_iter(bson_iter_t it) {
  if (jungle::query::v1::is_nullish_type(bson_iter_type(&it))) {
    return missing_or_null();
  }

  IndexedValue out;
  out.kind_ = Kind::Present;
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
  if (kind_ == Kind::MissingOrNull) return false;
  if (bytes_.empty()) return false;
  bson_t b;
  if (!bson_init_static(&b, bytes_.data(), bytes_.size())) return false;
  if (!bson_iter_init(out, &b)) return false;
  return bson_iter_next(out) && std::string_view(bson_iter_key(out)) == "v";
}

bool IndexedValueLess::operator()(const IndexedValue& a,
                                  const IndexedValue& b) const {
  if (a.is_missing_or_null() || b.is_missing_or_null()) {
    return !a.is_missing_or_null() && b.is_missing_or_null() ? false
         : a.is_missing_or_null() && !b.is_missing_or_null();
  }

  bson_iter_t ai, bi;
  const bool ga = a.get_iter(&ai);
  const bool gb = b.get_iter(&bi);
  if (!ga || !gb) {
    // Defensive: degenerate keys sort by raw bytes for stable ordering.
    return std::lexicographical_compare(a.bytes().begin(), a.bytes().end(),
                                         b.bytes().begin(), b.bytes().end());
  }
  const int rank_a = jungle::query::v1::sort_type_rank(bson_iter_type(&ai));
  const int rank_b = jungle::query::v1::sort_type_rank(bson_iter_type(&bi));
  if (rank_a != rank_b) return rank_a < rank_b;
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

bool MultiKeyLess::operator()(const MultiKey& a, const MultiKey& b) const {
  const std::size_t n = std::min(a.size(), b.size());
  IndexedValueLess cmp;
  for (std::size_t i = 0; i < n; ++i) {
    if (cmp(a[i], b[i])) return true;
    if (cmp(b[i], a[i])) return false;
  }
  return a.size() < b.size();
}

bool IndexManager::create(std::string name,
                          std::vector<std::string> field_paths,
                          IndexOptions options) {
  if (indexes_.contains(name)) return false;
  if (field_paths.empty()) return false;
  auto entry = std::make_unique<Entry>();
  entry->field_paths = std::move(field_paths);
  entry->unique = options.unique;
  indexes_.emplace(std::move(name), std::move(entry));
  return true;
}

std::string IndexManager::would_violate_unique(
    storage::RecordId record_id_to_skip, bson::BsonView doc) const {
  for (const auto& [name, entry] : indexes_) {
    if (!entry->unique) continue;
    bool sortability = entry->supports_ordered_sort;  // unused for the check
    auto keys = expand_doc_keys(doc, entry->field_paths, &sortability);
    if (keys.empty()) continue;
    for (const auto& key : keys) {
      auto it = entry->by_value.find(key);
      if (it == entry->by_value.end()) continue;
      for (auto rid : it->second) {
        if (rid != record_id_to_skip) return name;
      }
    }
  }
  return {};
}

IndexManager::Entry* IndexManager::find_by_path(std::string_view field_path) {
  // Single-field lookup — only matches indexes whose declared list is one
  // field. Compound indexes don't satisfy single-path planner queries; the
  // planner uses match_compound_index() / find_by_paths() for those.
  for (auto& [_, entry] : indexes_) {
    if (entry->field_paths.size() == 1 && entry->field_paths[0] == field_path) {
      return entry.get();
    }
  }
  return nullptr;
}

const IndexManager::Entry* IndexManager::find_by_path(
    std::string_view field_path) const {
  for (const auto& [_, entry] : indexes_) {
    if (entry->field_paths.size() == 1 && entry->field_paths[0] == field_path) {
      return entry.get();
    }
  }
  return nullptr;
}

const IndexManager::Entry* IndexManager::find_by_paths(
    const std::vector<std::string>& field_paths) const {
  for (const auto& [_, entry] : indexes_) {
    if (entry->field_paths == field_paths) return entry.get();
  }
  return nullptr;
}

std::vector<std::string> IndexManager::match_compound_index(
    const std::vector<std::string>& equality_paths) const {
  // Pick the index with the longest prefix of declared field_paths that
  // is fully covered by equality_paths (order-insensitive). Prefer more-
  // specific matches; ties broken by total declared length (shorter index
  // is cheaper to use). Returns the matched prefix in declaration order.
  std::vector<std::string> best;
  std::size_t best_total = 0;
  for (const auto& [_, entry] : indexes_) {
    std::vector<std::string> matched;
    for (const auto& declared : entry->field_paths) {
      const bool found = std::find(equality_paths.begin(), equality_paths.end(),
                                   declared) != equality_paths.end();
      if (!found) break;
      matched.push_back(declared);
    }
    if (matched.empty()) continue;
    if (matched.size() > best.size() ||
        (matched.size() == best.size() &&
         entry->field_paths.size() < best_total)) {
      best = std::move(matched);
      best_total = entry->field_paths.size();
    }
  }
  return best;
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

bool IndexManager::supports_ordered_sort(std::string_view field_path) const {
  const Entry* entry = find_by_path(field_path);
  return entry && entry->supports_ordered_sort;
}

const std::vector<storage::RecordId>* IndexManager::lookup_exact(
    std::string_view field_path, const IndexedValue& key) const {
  const Entry* entry = find_by_path(field_path);
  if (!entry) return nullptr;
  MultiKey mk;
  mk.push_back(key);
  auto it = entry->by_value.find(mk);
  if (it == entry->by_value.end()) return nullptr;
  return &it->second;
}

const std::vector<storage::RecordId>* IndexManager::lookup_exact_compound(
    const std::vector<std::string>& field_paths, const MultiKey& key) const {
  const Entry* entry = find_by_paths(field_paths);
  if (!entry) return nullptr;
  if (key.size() != entry->field_paths.size()) return nullptr;
  auto it = entry->by_value.find(key);
  if (it == entry->by_value.end()) return nullptr;
  return &it->second;
}

std::vector<storage::RecordId> IndexManager::lookup_range(
    std::string_view field_path,
    const IndexedValue* lower_bound, bool lower_inclusive,
    const IndexedValue* upper_bound, bool upper_inclusive,
    bool descending) const {
  std::vector<storage::RecordId> out;
  const Entry* entry = find_by_path(field_path);
  if (!entry) return out;

  // Lift single-component bounds to MultiKeys for the entry's map.
  std::optional<MultiKey> lower_mk;
  std::optional<MultiKey> upper_mk;
  if (lower_bound) { lower_mk.emplace(); lower_mk->push_back(*lower_bound); }
  if (upper_bound) { upper_mk.emplace(); upper_mk->push_back(*upper_bound); }

  auto begin = entry->by_value.begin();
  auto end = entry->by_value.end();

  if (lower_mk) {
    begin = lower_inclusive ? entry->by_value.lower_bound(*lower_mk)
                            : entry->by_value.upper_bound(*lower_mk);
  }
  if (upper_mk) {
    end = upper_inclusive ? entry->by_value.upper_bound(*upper_mk)
                          : entry->by_value.lower_bound(*upper_mk);
  }

  if (!descending) {
    for (auto it = begin; it != end; ++it) {
      out.insert(out.end(), it->second.begin(), it->second.end());
    }
    return out;
  }

  using ConstReverseIt =
      std::map<MultiKey, std::vector<storage::RecordId>, MultiKeyLess>::
          const_reverse_iterator;
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
    IndexInfo info;
    info.name = name;
    info.field_paths = entry->field_paths;
    info.field_path = entry->field_paths.size() == 1 ? entry->field_paths[0] : "";
    info.unique = entry->unique;
    info.entries = entries;
    out.push_back(std::move(info));
  }
  return out;
}



void IndexManager::on_insert(storage::RecordId record_id, bson::BsonView doc) {
  for (auto& [name, entry] : indexes_) {
    auto keys = expand_doc_keys(doc, entry->field_paths,
                                &entry->supports_ordered_sort);
    if (keys.empty()) continue;
    for (auto& key : keys) {
      auto& ids = entry->by_value[std::move(key)];
      auto pos = std::lower_bound(ids.begin(), ids.end(), record_id);
      // A multikey doc with duplicate array elements would hash to the same
      // bucket twice; dedupe so each (key, record_id) appears at most once.
      if (pos == ids.end() || *pos != record_id) ids.insert(pos, record_id);
    }
  }
}

void IndexManager::on_erase(storage::RecordId record_id, bson::BsonView old_doc) {
  for (auto& [name, entry] : indexes_) {
    auto keys = expand_doc_keys(old_doc, entry->field_paths, nullptr);
    if (keys.empty()) continue;
    for (const auto& key : keys) {
      auto it = entry->by_value.find(key);
      if (it == entry->by_value.end()) continue;
      auto& ids = it->second;
      auto pos = std::lower_bound(ids.begin(), ids.end(), record_id);
      if (pos != ids.end() && *pos == record_id) ids.erase(pos);
      if (ids.empty()) entry->by_value.erase(it);
    }
  }
}

}  // namespace savannah::index
