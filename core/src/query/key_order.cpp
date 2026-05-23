#include "savannah/query/key_order.h"

#include "savannah/query/value.h"

namespace savannah::jungle::query::v1 {

bool is_nullish_type(bson_type_t t) {
  return t == BSON_TYPE_NULL || t == BSON_TYPE_UNDEFINED;
}

bool supports_total_same_type_order(bson_type_t t) {
  if (is_numeric(t)) return true;
  return t == BSON_TYPE_NULL || t == BSON_TYPE_UNDEFINED ||
         t == BSON_TYPE_UTF8 || t == BSON_TYPE_OID ||
         t == BSON_TYPE_BOOL || t == BSON_TYPE_DATE_TIME ||
         t == BSON_TYPE_MINKEY || t == BSON_TYPE_MAXKEY;
}

int compare_optional_values_for_sort(
    const bson_iter_t* a, const bson_iter_t* b, bool ascending) {
  if (!a && !b) return 0;
  if (!a) {
    if (b && is_nullish_type(bson_iter_type(b))) return 0;
    return ascending ? -1 : 1;
  }
  if (!b) {
    if (is_nullish_type(bson_iter_type(a))) return 0;
    return ascending ? 1 : -1;
  }

  auto cmp = value_compare(*a, *b);
  if (!cmp) {
    const int left_rank = sort_type_rank(bson_iter_type(a));
    const int right_rank = sort_type_rank(bson_iter_type(b));
    if (left_rank == right_rank) return 0;
    cmp = left_rank < right_rank ? -1 : 1;
  }
  return ascending ? *cmp : -*cmp;
}

}  // namespace savannah::jungle::query::v1
