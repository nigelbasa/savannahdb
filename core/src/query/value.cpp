#include "savannah/query/value.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace savannah::jungle::query::v1 {

bool is_numeric(bson_type_t t) {
  return t == BSON_TYPE_INT32 || t == BSON_TYPE_INT64 ||
         t == BSON_TYPE_DOUBLE;
}

namespace {

bool numeric_equal(bson_iter_t a, bson_iter_t b) {
  if (bson_iter_type(&a) == BSON_TYPE_DOUBLE ||
      bson_iter_type(&b) == BSON_TYPE_DOUBLE) {
    return bson_iter_as_double(&a) == bson_iter_as_double(&b);
  }
  return bson_iter_as_int64(&a) == bson_iter_as_int64(&b);
}

int numeric_compare(bson_iter_t a, bson_iter_t b) {
  if (bson_iter_type(&a) == BSON_TYPE_DOUBLE ||
      bson_iter_type(&b) == BSON_TYPE_DOUBLE) {
    const double da = bson_iter_as_double(&a);
    const double db = bson_iter_as_double(&b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
  }
  const std::int64_t ia = bson_iter_as_int64(&a);
  const std::int64_t ib = bson_iter_as_int64(&b);
  if (ia < ib) return -1;
  if (ia > ib) return 1;
  return 0;
}

}  // namespace

int sort_type_rank(bson_type_t t) {
  switch (t) {
    case BSON_TYPE_MINKEY:
      return 1;
    case BSON_TYPE_NULL:
    case BSON_TYPE_UNDEFINED:
      return 2;
    case BSON_TYPE_INT32:
    case BSON_TYPE_INT64:
    case BSON_TYPE_DOUBLE:
      return 3;
    case BSON_TYPE_SYMBOL:
    case BSON_TYPE_UTF8:
      return 4;
    case BSON_TYPE_DOCUMENT:
      return 5;
    case BSON_TYPE_ARRAY:
      return 6;
    case BSON_TYPE_BINARY:
      return 7;
    case BSON_TYPE_OID:
      return 8;
    case BSON_TYPE_BOOL:
      return 9;
    case BSON_TYPE_DATE_TIME:
      return 10;
    case BSON_TYPE_TIMESTAMP:
      return 11;
    case BSON_TYPE_REGEX:
      return 12;
    case BSON_TYPE_CODE:
      return 13;
    case BSON_TYPE_CODEWSCOPE:
      return 14;
    case BSON_TYPE_MAXKEY:
      return 15;
    default:
      return 100;
  }
}

bool value_equal(bson_iter_t a, bson_iter_t b) {
  const bson_type_t ta = bson_iter_type(&a);
  const bson_type_t tb = bson_iter_type(&b);

  if (is_numeric(ta) && is_numeric(tb)) return numeric_equal(a, b);
  if (ta != tb) return false;

  switch (ta) {
    case BSON_TYPE_UTF8: {
      std::uint32_t la = 0, lb = 0;
      const char* sa = bson_iter_utf8(&a, &la);
      const char* sb = bson_iter_utf8(&b, &lb);
      return la == lb && std::memcmp(sa, sb, la) == 0;
    }
    case BSON_TYPE_OID:
      return bson_oid_equal(bson_iter_oid(&a), bson_iter_oid(&b));
    case BSON_TYPE_BOOL:
      return bson_iter_bool(&a) == bson_iter_bool(&b);
    case BSON_TYPE_NULL:
    case BSON_TYPE_UNDEFINED:
      return true;
    case BSON_TYPE_DATE_TIME:
      return bson_iter_date_time(&a) == bson_iter_date_time(&b);
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY: {
      const std::uint8_t *ab = nullptr, *bb = nullptr;
      std::uint32_t alen = 0, blen = 0;
      if (ta == BSON_TYPE_DOCUMENT) {
        bson_iter_document(&a, &alen, &ab);
        bson_iter_document(&b, &blen, &bb);
      } else {
        bson_iter_array(&a, &alen, &ab);
        bson_iter_array(&b, &blen, &bb);
      }
      return alen == blen && std::memcmp(ab, bb, alen) == 0;
    }
    default:
      return false;
  }
}

bool resolve_path(const bson_t& doc, const char* path, bson_iter_t* out) {
  bson_iter_t root;
  if (!bson_iter_init(&root, &doc)) return false;
  if (std::string_view(path).find('.') == std::string_view::npos) {
    if (!bson_iter_find(&root, path)) return false;
    *out = root;
    return true;
  }
  return bson_iter_find_descendant(&root, path, out);
}

std::optional<int> value_compare(bson_iter_t a, bson_iter_t b) {
  const bson_type_t ta = bson_iter_type(&a);
  const bson_type_t tb = bson_iter_type(&b);

  if (is_numeric(ta) && is_numeric(tb)) return numeric_compare(a, b);
  if (ta != tb) return std::nullopt;

  switch (ta) {
    case BSON_TYPE_UTF8: {
      std::uint32_t la = 0, lb = 0;
      const char* sa = bson_iter_utf8(&a, &la);
      const char* sb = bson_iter_utf8(&b, &lb);
      const std::uint32_t n = la < lb ? la : lb;
      const int c = std::memcmp(sa, sb, n);
      if (c != 0) return c < 0 ? -1 : 1;
      if (la < lb) return -1;
      if (la > lb) return 1;
      return 0;
    }
    case BSON_TYPE_OID: {
      const int c = std::memcmp(bson_iter_oid(&a), bson_iter_oid(&b), 12);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case BSON_TYPE_BOOL:
      return (bson_iter_bool(&a) ? 1 : 0) - (bson_iter_bool(&b) ? 1 : 0);
    case BSON_TYPE_DATE_TIME: {
      const std::int64_t va = bson_iter_date_time(&a);
      const std::int64_t vb = bson_iter_date_time(&b);
      if (va < vb) return -1;
      if (va > vb) return 1;
      return 0;
    }
    default:
      return std::nullopt;
  }
}

}  // namespace savannah::jungle::query::v1
