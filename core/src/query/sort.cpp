#include "savannah/query/sort.h"

#include "savannah/query/key_order.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace savannah::jungle::query::v1 {

namespace {

bool resolve_view(bson::BsonView doc, const char* path, bson_iter_t* out) {
  bson_t d;
  if (!bson_init_static(&d, doc.data(), doc.size())) return false;
  return resolve_path(d, path, out);
}

// Per-key compare with MongoDB's missing/null and mixed-type sort semantics.
// Missing fields sort as `null`, so missing vs explicit null are equal and
// stable_sort preserves their original order. Present cross-type pairs use the
// BSON type precedence table from query/value.cpp.
int compare_for_key(bson::BsonView a, bson::BsonView b, const char* path,
                    bool ascending) {
  bson_iter_t ai;
  bson_iter_t bi;
  const bool pa = resolve_view(a, path, &ai);
  const bool pb = resolve_view(b, path, &bi);

  return compare_optional_values_for_sort(
      pa ? &ai : nullptr, pb ? &bi : nullptr, ascending);
}

}  // namespace

bool sort_less(bson::BsonView a, bson::BsonView b,
               std::span<const std::uint8_t> sort_spec) {
  bson_t spec;
  if (sort_spec.size() < 5 ||
      !bson_init_static(&spec, sort_spec.data(), sort_spec.size())) {
    return false;
  }
  bson_iter_t it;
  if (!bson_iter_init(&it, &spec)) return false;

  while (bson_iter_next(&it)) {
    const char* path = bson_iter_key(&it);
    if (!path) continue;
    // Direction: 1 ascending, -1 descending. Anything else treated as asc.
    bool ascending = true;
    if (is_numeric(bson_iter_type(&it))) {
      ascending = bson_iter_as_int64(&it) >= 0;
    }
    const int c = compare_for_key(a, b, path, ascending);
    if (c != 0) return c < 0;
  }
  return false;  // equal under all keys → stable_sort preserves order.
}

}  // namespace savannah::jungle::query::v1
