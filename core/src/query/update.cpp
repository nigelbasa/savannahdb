#include "savannah/query/update.h"

#include <bson/bson.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace savannah::jungle::query::v1 {

namespace {

bool is_numeric(bson_type_t t) {
  return t == BSON_TYPE_INT32 || t == BSON_TYPE_INT64 ||
         t == BSON_TYPE_DOUBLE;
}

UpdateOutcome err(int code, std::string name, std::string msg) {
  UpdateOutcome o;
  o.err_code = code;
  o.err_name = std::move(name);
  o.err_message = std::move(msg);
  return o;
}

std::vector<std::uint8_t> bson_to_bytes(const bson_t& b) {
  const std::uint8_t* data = bson_get_data(&b);
  return std::vector<std::uint8_t>(data, data + b.len);
}

bool starts_with_dollar(const char* k) { return k && k[0] == '$'; }

// Append a value from `src_iter` to `dst` under `key`, preserving its type.
// libbson's iter-based append handles every BSON type uniformly.
bool copy_value(bson_t* dst, const char* key, bson_iter_t* src_iter) {
  return bson_append_iter(dst, key, -1, src_iter);
}

// Add two numeric BSON values, preserving type when feasible. Doubles
// promote; otherwise we keep int64 to avoid silent overflow on large sums.
bool append_inc_sum(bson_t* dst, const char* key, bson_iter_t orig,
                    bson_iter_t delta) {
  const bson_type_t to = bson_iter_type(&orig);
  const bson_type_t td = bson_iter_type(&delta);
  if (to == BSON_TYPE_DOUBLE || td == BSON_TYPE_DOUBLE) {
    return bson_append_double(dst, key, -1,
                              bson_iter_as_double(&orig) +
                                  bson_iter_as_double(&delta));
  }
  return bson_append_int64(dst, key, -1,
                           bson_iter_as_int64(&orig) +
                               bson_iter_as_int64(&delta));
}

// Append the increment as a fresh field (when the original lacked it).
// Mongo creates the field with `0 + delta` typed per the delta side.
bool append_inc_fresh(bson_t* dst, const char* key, bson_iter_t delta) {
  if (bson_iter_type(&delta) == BSON_TYPE_DOUBLE) {
    return bson_append_double(dst, key, -1, bson_iter_as_double(&delta));
  }
  return bson_append_int64(dst, key, -1, bson_iter_as_int64(&delta));
}

// ---------------------------------------------------------------------------
// Spec parsing
// ---------------------------------------------------------------------------

enum class SpecKind { Operator, Replacement, Empty, Mixed };

SpecKind classify(const bson_t& spec) {
  bson_iter_t it;
  if (!bson_iter_init(&it, &spec)) return SpecKind::Empty;
  bool any = false;
  bool any_dollar = false;
  bool any_plain = false;
  while (bson_iter_next(&it)) {
    any = true;
    if (starts_with_dollar(bson_iter_key(&it))) any_dollar = true;
    else any_plain = true;
  }
  if (!any) return SpecKind::Empty;
  if (any_dollar && any_plain) return SpecKind::Mixed;
  return any_dollar ? SpecKind::Operator : SpecKind::Replacement;
}

// ---------------------------------------------------------------------------
// Operator-style application
// ---------------------------------------------------------------------------

struct OpMaps {
  // Field name → iter pointing at the value inside the spec subdoc.
  // bson_iter_t is a snapshot; copying it preserves position.
  std::unordered_map<std::string, bson_iter_t> set_ops;
  std::unordered_set<std::string> unset_ops;
  std::unordered_map<std::string, bson_iter_t> inc_ops;
};

// Walk a `{ field: value, field2: value2, ... }` subdoc, calling sink(key, iter).
template <typename F>
bool for_each_subdoc_field(bson_iter_t op_iter, F&& sink) {
  if (bson_iter_type(&op_iter) != BSON_TYPE_DOCUMENT) return false;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_document(&op_iter, &len, &data);
  bson_t sub;
  if (!bson_init_static(&sub, data, len)) return false;
  bson_iter_t it;
  if (!bson_iter_init(&it, &sub)) return false;
  while (bson_iter_next(&it)) {
    sink(bson_iter_key(&it), it);
  }
  return true;
}

UpdateOutcome apply_operator_spec(bson::BsonView original, const bson_t& spec) {
  OpMaps ops;
  // Collect ops. Reject anything we don't implement so we don't silently drop.
  bson_iter_t spec_it;
  if (!bson_iter_init(&spec_it, &spec)) {
    return err(2, "BadValue", "malformed update spec");
  }
  while (bson_iter_next(&spec_it)) {
    const char* op = bson_iter_key(&spec_it);
    if (!op) continue;
    const std::string_view name(op);
    if (name == "$set") {
      for_each_subdoc_field(spec_it, [&](const char* k, bson_iter_t v) {
        if (k) ops.set_ops[k] = v;
      });
    } else if (name == "$unset") {
      for_each_subdoc_field(spec_it, [&](const char* k, bson_iter_t /*v*/) {
        if (k) ops.unset_ops.emplace(k);
      });
    } else if (name == "$inc") {
      for_each_subdoc_field(spec_it, [&](const char* k, bson_iter_t v) {
        if (k) ops.inc_ops[k] = v;
      });
    } else {
      return err(9, "FailedToParse",
                 std::string("unsupported update operator: ") + std::string(name));
    }
  }

  bson_t orig;
  if (!bson_init_static(&orig, original.data(), original.size())) {
    return err(2, "BadValue", "malformed original document");
  }
  bson_t out;
  bson_init(&out);

  bson_iter_t oit;
  if (!bson_iter_init(&oit, &orig)) {
    bson_destroy(&out);
    return err(2, "BadValue", "malformed original document");
  }

  std::unordered_set<std::string> consumed_set;
  std::unordered_set<std::string> consumed_inc;

  while (bson_iter_next(&oit)) {
    const char* k = bson_iter_key(&oit);
    if (!k) continue;
    const std::string key(k);

    if (ops.unset_ops.contains(key)) continue;  // tombstone

    if (auto sit = ops.set_ops.find(key); sit != ops.set_ops.end()) {
      bson_iter_t val = sit->second;
      copy_value(&out, k, &val);
      consumed_set.insert(key);
      continue;
    }

    if (auto iit = ops.inc_ops.find(key); iit != ops.inc_ops.end()) {
      if (!is_numeric(bson_iter_type(&oit))) {
        bson_destroy(&out);
        return err(14, "TypeMismatch",
                   "Cannot apply $inc to a value of non-numeric type");
      }
      bson_iter_t delta = iit->second;
      append_inc_sum(&out, k, oit, delta);
      consumed_inc.insert(key);
      continue;
    }

    copy_value(&out, k, &oit);
  }

  // $set fields not present in original — append at end.
  for (auto& [k, v] : ops.set_ops) {
    if (!consumed_set.contains(k)) {
      bson_iter_t val = v;
      copy_value(&out, k.c_str(), &val);
    }
  }
  // $inc fresh fields — value = delta.
  for (auto& [k, v] : ops.inc_ops) {
    if (!consumed_inc.contains(k)) {
      bson_iter_t delta = v;
      append_inc_fresh(&out, k.c_str(), delta);
    }
  }

  UpdateOutcome o;
  o.bytes = bson_to_bytes(out);
  o.changed = o.bytes.size() != original.size() ||
              std::memcmp(o.bytes.data(), original.data(), o.bytes.size()) != 0;
  bson_destroy(&out);
  return o;
}

// ---------------------------------------------------------------------------
// Replacement
// ---------------------------------------------------------------------------

// Returns true if both docs have an `_id` field and they're byte-equal.
// Sets `original_has_id` to true if original includes _id.
bool ids_compatible(const bson_t& original, const bson_t& replacement,
                    bool& original_has_id, bool& replacement_has_id) {
  bson_iter_t oit;
  bson_iter_t rit;
  original_has_id = bson_iter_init(&oit, &original) && bson_iter_find(&oit, "_id");
  replacement_has_id =
      bson_iter_init(&rit, &replacement) && bson_iter_find(&rit, "_id");
  if (!original_has_id || !replacement_has_id) return true;

  const bson_value_t* ov = bson_iter_value(&oit);
  const bson_value_t* rv = bson_iter_value(&rit);
  if (!ov || !rv || ov->value_type != rv->value_type) return false;

  // Type-specific equality for the common _id types.
  switch (ov->value_type) {
    case BSON_TYPE_OID:
      return bson_oid_equal(&ov->value.v_oid, &rv->value.v_oid);
    case BSON_TYPE_INT32:
      return ov->value.v_int32 == rv->value.v_int32;
    case BSON_TYPE_INT64:
      return ov->value.v_int64 == rv->value.v_int64;
    case BSON_TYPE_UTF8:
      return ov->value.v_utf8.len == rv->value.v_utf8.len &&
             std::memcmp(ov->value.v_utf8.str, rv->value.v_utf8.str,
                         ov->value.v_utf8.len) == 0;
    default:
      // Conservative for exotic _id types — treat as mismatch.
      return false;
  }
}

UpdateOutcome apply_replacement(bson::BsonView original, const bson_t& spec) {
  bson_t orig;
  if (!bson_init_static(&orig, original.data(), original.size())) {
    return err(2, "BadValue", "malformed original document");
  }

  bool orig_has_id = false, repl_has_id = false;
  if (!ids_compatible(orig, spec, orig_has_id, repl_has_id)) {
    return err(66, "ImmutableField",
               "Performing an update on the path '_id' would modify the immutable field '_id'");
  }

  bson_t out;
  bson_init(&out);

  // If the replacement has no _id but the original did, prepend the original _id.
  if (!repl_has_id && orig_has_id) {
    bson_iter_t oid_it;
    bson_iter_init(&oid_it, &orig);
    if (bson_iter_find(&oid_it, "_id")) {
      copy_value(&out, "_id", &oid_it);
    }
  }
  // Copy every replacement field.
  bson_iter_t rit;
  bson_iter_init(&rit, &spec);
  while (bson_iter_next(&rit)) {
    const char* k = bson_iter_key(&rit);
    if (!k) continue;
    copy_value(&out, k, &rit);
  }

  UpdateOutcome o;
  o.bytes = bson_to_bytes(out);
  o.changed = o.bytes.size() != original.size() ||
              std::memcmp(o.bytes.data(), original.data(), o.bytes.size()) != 0;
  bson_destroy(&out);
  return o;
}

}  // namespace

UpdateOutcome apply_update(bson::BsonView original,
                           std::span<const std::uint8_t> spec_bytes) {
  bson_t spec;
  if (spec_bytes.size() < 5 ||
      !bson_init_static(&spec, spec_bytes.data(), spec_bytes.size())) {
    return err(2, "BadValue", "malformed update spec");
  }

  switch (classify(spec)) {
    case SpecKind::Operator:
      return apply_operator_spec(original, spec);
    case SpecKind::Replacement:
      return apply_replacement(original, spec);
    case SpecKind::Empty: {
      // Mongo treats {} as a full replacement (clears all but _id).
      return apply_replacement(original, spec);
    }
    case SpecKind::Mixed:
      return err(9, "FailedToParse",
                 "update spec mixes operators and plain fields");
  }
  return err(2, "BadValue", "unknown spec shape");
}

UpdateOutcome seed_upsert(std::span<const std::uint8_t> filter_bytes,
                          std::span<const std::uint8_t> spec_bytes) {
  bson_t out;
  bson_init(&out);

  // Ensure _id comes first in the seeded doc (matches Mongo's convention).
  // We'll write it after collecting all fields so we know whether either
  // side supplied one.
  bool has_id = false;

  // Walk filter, copying only literal (non-$-operator-subdoc) fields.
  bson_t filter;
  if (filter_bytes.size() >= 5 &&
      bson_init_static(&filter, filter_bytes.data(), filter_bytes.size())) {
    bson_iter_t it;
    if (bson_iter_init(&it, &filter)) {
      while (bson_iter_next(&it)) {
        const char* k = bson_iter_key(&it);
        if (!k || k[0] == '$') continue;  // top-level $and/$or → skip
        // Operator subdoc value? Skip — never seed `{x: {$gt:5}}` literally.
        if (bson_iter_type(&it) == BSON_TYPE_DOCUMENT) {
          std::uint32_t len = 0;
          const std::uint8_t* data = nullptr;
          bson_iter_document(&it, &len, &data);
          bson_t sub;
          if (bson_init_static(&sub, data, len)) {
            bson_iter_t sit;
            if (bson_iter_init(&sit, &sub) && bson_iter_next(&sit)) {
              const char* sk = bson_iter_key(&sit);
              if (sk && sk[0] == '$') continue;
            }
          }
        }
        if (std::string_view(k) == "_id") has_id = true;
        copy_value(&out, k, &it);
      }
    }
  }

  // Generate _id if neither filter nor (eventually) spec supplied one. We
  // check spec for _id below before generating; for replacement specs the
  // _id may be embedded.
  bson_t spec;
  bool spec_ok = spec_bytes.size() >= 5 &&
                 bson_init_static(&spec, spec_bytes.data(), spec_bytes.size());
  if (spec_ok) {
    bson_iter_t sit;
    if (bson_iter_init(&sit, &spec)) {
      while (bson_iter_next(&sit)) {
        const char* k = bson_iter_key(&sit);
        if (k && std::string_view(k) == "_id") has_id = true;
      }
    }
  }
  if (!has_id) {
    bson_oid_t oid;
    bson_oid_init(&oid, nullptr);
    // Prepend by rebuilding: simpler since out has no _id yet, just append.
    // (Order: _id first is conventional but not required for correctness.)
    // We build a new doc with _id first, then copy existing fields.
    bson_t reordered;
    bson_init(&reordered);
    bson_append_oid(&reordered, "_id", -1, &oid);
    bson_iter_t it;
    if (bson_iter_init(&it, &out)) {
      while (bson_iter_next(&it)) {
        copy_value(&reordered, bson_iter_key(&it), &it);
      }
    }
    bson_destroy(&out);
    out = reordered;
  }

  // Now apply spec on top of the seeded doc.
  std::vector<std::uint8_t> seed_bytes = bson_to_bytes(out);
  bson_destroy(&out);

  bson::BsonView seed_view(
      std::span<const std::uint8_t>{seed_bytes.data(), seed_bytes.size()});
  return apply_update(seed_view, spec_bytes);
}

}  // namespace savannah::jungle::query::v1
