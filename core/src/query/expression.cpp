#include "savannah/query/expression.h"

#include "savannah/query/value.h"

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

namespace savannah::jungle::query::v1 {

namespace {

constexpr std::uint8_t kEmptyBsonBytes[] = {5, 0, 0, 0, 0};

// make_wrapped_value(append) builds a {v: ...} envelope by handing the
// caller a fresh bson_t to append into. Templated so we can pass any
// libbson append_* lambda without runtime indirection.
template <typename Fn>
std::vector<std::uint8_t> make_wrapped_value(Fn&& append) {
  bson_t wrap;
  bson_init(&wrap);
  append(&wrap);
  auto out = bytes_from_bson(wrap);
  bson_destroy(&wrap);
  return out;
}

// Forward decls — evaluator and its helpers reference each other.
std::vector<std::vector<std::uint8_t>> evaluate_array_elements(
    const bson_t& source_doc, bson_iter_t array_iter);

std::optional<std::vector<std::uint8_t>> evaluate_operator_expression(
    const bson_t& source_doc, const bson_t& expr_doc);

}  // namespace

// Generic BSON helpers ------------------------------------------------------

bool init_static_bson(std::span<const std::uint8_t> bytes, bson_t* out) {
  return bytes.size() >= 5 && bson_init_static(out, bytes.data(), bytes.size());
}

std::span<const std::uint8_t> empty_bson() {
  return {kEmptyBsonBytes, sizeof(kEmptyBsonBytes)};
}

std::vector<std::uint8_t> copy_bytes(std::span<const std::uint8_t> bytes) {
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> owned_bytes(bson::BsonView view) {
  return copy_bytes(view.span());
}

std::vector<std::uint8_t> bytes_from_bson(const bson_t& doc) {
  const auto* data = bson_get_data(&doc);
  return std::vector<std::uint8_t>(data, data + doc.len);
}

// Wrapped-value helpers -----------------------------------------------------

std::vector<std::uint8_t> wrap_iter_value(bson_iter_t iter) {
  bson_t wrap;
  bson_init(&wrap);
  bson_append_iter(&wrap, "v", -1, &iter);
  auto out = bytes_from_bson(wrap);
  bson_destroy(&wrap);
  return out;
}

std::vector<std::uint8_t> wrap_null() {
  return make_wrapped_value([](bson_t* wrap) { bson_append_null(wrap, "v", -1); });
}

std::vector<std::uint8_t> wrap_utf8(std::string_view value) {
  return make_wrapped_value([&](bson_t* wrap) {
    bson_append_utf8(wrap, "v", -1, value.data(), static_cast<int>(value.size()));
  });
}

std::vector<std::uint8_t> wrap_int64(std::int64_t value) {
  return make_wrapped_value([&](bson_t* wrap) {
    bson_append_int64(wrap, "v", -1, value);
  });
}

namespace {

std::vector<std::uint8_t> wrap_double(double value) {
  return make_wrapped_value([&](bson_t* wrap) {
    bson_append_double(wrap, "v", -1, value);
  });
}

std::vector<std::uint8_t> wrap_bool(bool value) {
  return make_wrapped_value([&](bson_t* wrap) {
    bson_append_bool(wrap, "v", -1, value);
  });
}

// MongoDB expression-context truthiness: null/missing/false/0 → false,
// everything else (including empty string) → true. Distinct from filter
// truthiness because $cond/$and/$or evaluate exprs, not field selectors.
bool is_truthy_for_expr(const std::vector<std::uint8_t>& wrapped) {
  if (wrapped.empty()) return false;
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return false;
  switch (bson_iter_type(&iter)) {
    case BSON_TYPE_NULL:
    case BSON_TYPE_UNDEFINED:
      return false;
    case BSON_TYPE_BOOL:
      return bson_iter_bool(&iter);
    case BSON_TYPE_INT32:
      return bson_iter_int32(&iter) != 0;
    case BSON_TYPE_INT64:
      return bson_iter_int64(&iter) != 0;
    case BSON_TYPE_DOUBLE:
      return bson_iter_double(&iter) != 0.0;
    default:
      return true;
  }
}

}  // namespace

bool unwrap_iter(const std::vector<std::uint8_t>& wrapped, bson_t* holder,
                 bson_iter_t* out) {
  if (!init_static_bson(wrapped, holder)) return false;
  return bson_iter_init_find(out, holder, "v");
}

bool append_wrapped_value(bson_t* out, std::string_view key,
                          const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return false;
  return bson_append_iter(out, key.data(), static_cast<int>(key.size()), &iter);
}

bool append_wrapped_array_item(bson_t* out, std::size_t index,
                               const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return false;
  const std::string key = std::to_string(index);
  return bson_append_iter(out, key.c_str(), -1, &iter);
}

std::optional<std::vector<std::uint8_t>> unwrap_document_bytes(
    const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return std::nullopt;
  if (bson_iter_type(&iter) != BSON_TYPE_DOCUMENT) return std::nullopt;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_document(&iter, &len, &data);
  return std::vector<std::uint8_t>(data, data + len);
}

std::optional<std::vector<std::uint8_t>> unwrap_array_bytes(
    const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return std::nullopt;
  if (bson_iter_type(&iter) != BSON_TYPE_ARRAY) return std::nullopt;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_array(&iter, &len, &data);
  return std::vector<std::uint8_t>(data, data + len);
}

bool nullish_wrapped(const std::vector<std::uint8_t>& wrapped) {
  bson_t holder;
  bson_iter_t iter;
  if (!unwrap_iter(wrapped, &holder, &iter)) return true;
  const auto type = bson_iter_type(&iter);
  return type == BSON_TYPE_NULL || type == BSON_TYPE_UNDEFINED;
}

// Type predicates -----------------------------------------------------------

bool is_truthy_projection_value(bson_iter_t iter) {
  switch (bson_iter_type(&iter)) {
    case BSON_TYPE_BOOL:
      return bson_iter_bool(&iter);
    case BSON_TYPE_INT32:
      return bson_iter_int32(&iter) != 0;
    case BSON_TYPE_INT64:
      return bson_iter_int64(&iter) != 0;
    case BSON_TYPE_DOUBLE:
      return bson_iter_double(&iter) != 0.0;
    default:
      return false;
  }
}

bool field_ref_from_iter(bson_iter_t iter, std::string* out) {
  if (bson_iter_type(&iter) != BSON_TYPE_UTF8) return false;
  std::uint32_t len = 0;
  const char* text = bson_iter_utf8(&iter, &len);
  if (!text || len == 0 || text[0] != '$') return false;
  out->assign(text + 1, len - 1);
  return true;
}

// Evaluator -----------------------------------------------------------------

namespace {

std::vector<std::vector<std::uint8_t>> evaluate_array_elements(
    const bson_t& source_doc, bson_iter_t array_iter) {
  std::vector<std::vector<std::uint8_t>> values;
  if (bson_iter_type(&array_iter) != BSON_TYPE_ARRAY) return values;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_array(&array_iter, &len, &data);
  bson_t arr;
  if (!bson_init_static(&arr, data, len)) return values;
  bson_iter_t it;
  if (!bson_iter_init(&it, &arr)) return values;
  while (bson_iter_next(&it)) {
    auto value = evaluate_expression(source_doc, wrap_iter_value(it));
    if (value) values.push_back(*value);
    else values.push_back({});
  }
  return values;
}

std::optional<std::vector<std::uint8_t>> evaluate_operator_expression(
    const bson_t& source_doc, const bson_t& expr_doc) {
  bson_iter_t op_it;
  if (!bson_iter_init(&op_it, &expr_doc) || !bson_iter_next(&op_it)) return std::nullopt;
  const char* op = bson_iter_key(&op_it);
  if (!op || op[0] != '$') return std::nullopt;

  if (std::string_view(op) == "$literal") {
    return wrap_iter_value(op_it);
  }

  if (std::string_view(op) == "$ifNull") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    for (const auto& arg : args) {
      if (!arg.empty() && !nullish_wrapped(arg)) return arg;
    }
    return wrap_null();
  }

  if (std::string_view(op) == "$concat") {
    const auto parts = evaluate_array_elements(source_doc, op_it);
    std::string out;
    for (const auto& part : parts) {
      if (part.empty() || nullish_wrapped(part)) return wrap_null();
      bson_t holder;
      bson_iter_t iter;
      if (!unwrap_iter(part, &holder, &iter)) return wrap_null();
      if (bson_iter_type(&iter) != BSON_TYPE_UTF8) return wrap_null();
      std::uint32_t len = 0;
      const char* text = bson_iter_utf8(&iter, &len);
      out.append(text, len);
    }
    return wrap_utf8(out);
  }

  if (std::string_view(op) == "$toString") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value || nullish_wrapped(*value)) return wrap_null();
    bson_t holder;
    bson_iter_t iter;
    if (!unwrap_iter(*value, &holder, &iter)) return wrap_null();
    switch (bson_iter_type(&iter)) {
      case BSON_TYPE_UTF8: {
        std::uint32_t len = 0;
        const char* text = bson_iter_utf8(&iter, &len);
        return wrap_utf8(std::string_view(text, len));
      }
      case BSON_TYPE_INT32:
        return wrap_utf8(std::to_string(bson_iter_int32(&iter)));
      case BSON_TYPE_INT64:
        return wrap_utf8(std::to_string(bson_iter_int64(&iter)));
      case BSON_TYPE_DOUBLE:
        return wrap_utf8(std::to_string(bson_iter_double(&iter)));
      case BSON_TYPE_BOOL:
        return wrap_utf8(bson_iter_bool(&iter) ? "true" : "false");
      case BSON_TYPE_OID: {
        char oid[25] = {};
        bson_oid_to_string(bson_iter_oid(&iter), oid);
        return wrap_utf8(oid);
      }
      default:
        return wrap_null();
    }
  }

  if (std::string_view(op) == "$size") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value) return std::nullopt;
    bson_t holder;
    bson_iter_t iter;
    if (!unwrap_iter(*value, &holder, &iter)) return std::nullopt;
    if (bson_iter_type(&iter) != BSON_TYPE_ARRAY) return std::nullopt;
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&iter, &len, &data);
    bson_t arr;
    if (!bson_init_static(&arr, data, len)) return std::nullopt;
    bson_iter_t arr_it;
    std::int64_t count = 0;
    if (bson_iter_init(&arr_it, &arr)) {
      while (bson_iter_next(&arr_it)) ++count;
    }
    return wrap_int64(count);
  }

  // Arithmetic --------------------------------------------------------------
  //
  // Type rules: if every operand is int32/int64, stay in int64. Any double
  // operand promotes the result to double. Division and roots always
  // produce double. Missing/null short-circuits the whole expression to
  // null — matches MongoDB's "non-numeric in arithmetic = null" rule.

  auto eval_numeric_arg = [&](bson_iter_t arg_iter, bool* is_double,
                              double* d_out, std::int64_t* i_out) -> bool {
    auto v = evaluate_expression(source_doc, wrap_iter_value(arg_iter));
    if (!v) return false;
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*v, &holder, &it)) return false;
    if (!is_numeric(bson_iter_type(&it))) return false;
    if (bson_iter_type(&it) == BSON_TYPE_DOUBLE) *is_double = true;
    *d_out = bson_iter_as_double(&it);
    *i_out = bson_iter_as_int64(&it);
    return true;
  };

  auto reduce_arithmetic = [&](double init_d, std::int64_t init_i,
                               auto combine_d, auto combine_i)
      -> std::optional<std::vector<std::uint8_t>> {
    bool any_double = false;
    double acc_d = init_d;
    std::int64_t acc_i = init_i;
    if (bson_iter_type(&op_it) != BSON_TYPE_ARRAY) return wrap_null();
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&op_it, &alen, &adata);
    bson_t arr;
    if (!bson_init_static(&arr, adata, alen)) return wrap_null();
    bson_iter_t it;
    if (!bson_iter_init(&it, &arr)) return wrap_null();
    bool first = true;
    while (bson_iter_next(&it)) {
      double d = 0.0;
      std::int64_t i = 0;
      if (!eval_numeric_arg(it, &any_double, &d, &i)) return wrap_null();
      if (first) { acc_d = d; acc_i = i; first = false; continue; }
      acc_d = combine_d(acc_d, d);
      acc_i = combine_i(acc_i, i);
    }
    if (first) return wrap_null();  // empty array
    return any_double ? wrap_double(acc_d) : wrap_int64(acc_i);
  };

  if (std::string_view(op) == "$add") {
    return reduce_arithmetic(0.0, 0,
        [](double a, double b) { return a + b; },
        [](std::int64_t a, std::int64_t b) { return a + b; });
  }
  if (std::string_view(op) == "$multiply") {
    return reduce_arithmetic(1.0, 1,
        [](double a, double b) { return a * b; },
        [](std::int64_t a, std::int64_t b) { return a * b; });
  }
  if (std::string_view(op) == "$subtract") {
    return reduce_arithmetic(0.0, 0,
        [](double a, double b) { return a - b; },
        [](std::int64_t a, std::int64_t b) { return a - b; });
  }
  if (std::string_view(op) == "$divide" || std::string_view(op) == "$mod") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 2) return wrap_null();
    bson_t ha, hb;
    bson_iter_t ia, ib;
    if (!unwrap_iter(args[0], &ha, &ia) || !unwrap_iter(args[1], &hb, &ib)) {
      return wrap_null();
    }
    if (!is_numeric(bson_iter_type(&ia)) || !is_numeric(bson_iter_type(&ib))) {
      return wrap_null();
    }
    const double a = bson_iter_as_double(&ia);
    const double b = bson_iter_as_double(&ib);
    if (b == 0.0) return wrap_null();  // div-by-zero → null in Mongo
    if (std::string_view(op) == "$divide") return wrap_double(a / b);
    return wrap_double(std::fmod(a, b));
  }

  if (std::string_view(op) == "$abs" || std::string_view(op) == "$ceil" ||
      std::string_view(op) == "$floor" || std::string_view(op) == "$trunc") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value || nullish_wrapped(*value)) return wrap_null();
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*value, &holder, &it)) return wrap_null();
    if (!is_numeric(bson_iter_type(&it))) return wrap_null();
    const double d = bson_iter_as_double(&it);
    if (std::string_view(op) == "$abs") return wrap_double(std::fabs(d));
    if (std::string_view(op) == "$ceil") return wrap_double(std::ceil(d));
    if (std::string_view(op) == "$floor") return wrap_double(std::floor(d));
    return wrap_double(std::trunc(d));
  }

  if (std::string_view(op) == "$round") {
    // {$round: [value, place]} — place defaults to 0. Banker's rounding
    // would match Mongo more precisely; std::round is good enough for the
    // smoke and we can tighten if a driver complains.
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.empty() || args[0].empty() || nullish_wrapped(args[0])) return wrap_null();
    bson_t hv;
    bson_iter_t iv;
    if (!unwrap_iter(args[0], &hv, &iv)) return wrap_null();
    if (!is_numeric(bson_iter_type(&iv))) return wrap_null();
    int place = 0;
    if (args.size() >= 2 && !args[1].empty()) {
      bson_t hp;
      bson_iter_t ip;
      if (unwrap_iter(args[1], &hp, &ip) && is_numeric(bson_iter_type(&ip))) {
        place = static_cast<int>(bson_iter_as_int64(&ip));
      }
    }
    const double d = bson_iter_as_double(&iv);
    const double scale = std::pow(10.0, place);
    return wrap_double(std::round(d * scale) / scale);
  }

  // Comparison ---------------------------------------------------------------
  //
  // Expression form: returns a bool, vs filter form which returns a match
  // verdict. Reuses value_compare/value_equal so the answer matches what
  // $match would say. Cross-type pairs follow Mongo's BSON sort order via
  // value_compare; if it can't decide we fall back to false (Mongo's
  // canonical-type order subtleties beyond this aren't tested by drivers).

  if (std::string_view(op) == "$eq" || std::string_view(op) == "$ne" ||
      std::string_view(op) == "$gt" || std::string_view(op) == "$gte" ||
      std::string_view(op) == "$lt" || std::string_view(op) == "$lte") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 2 || args[0].empty() || args[1].empty()) return wrap_bool(false);
    bson_t ha, hb;
    bson_iter_t ia, ib;
    if (!unwrap_iter(args[0], &ha, &ia) || !unwrap_iter(args[1], &hb, &ib)) {
      return wrap_bool(false);
    }
    const bool eq = value_equal(ia, ib);
    if (std::string_view(op) == "$eq") return wrap_bool(eq);
    if (std::string_view(op) == "$ne") return wrap_bool(!eq);
    auto cmp = value_compare(ia, ib);
    if (!cmp) return wrap_bool(false);
    if (std::string_view(op) == "$gt")  return wrap_bool(*cmp > 0);
    if (std::string_view(op) == "$gte") return wrap_bool(*cmp >= 0);
    if (std::string_view(op) == "$lt")  return wrap_bool(*cmp < 0);
    return wrap_bool(*cmp <= 0);  // $lte
  }

  if (std::string_view(op) == "$arrayElemAt") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() < 2 || args[0].empty() || args[1].empty()) return std::nullopt;

    bson_t array_holder;
    bson_iter_t array_value;
    bson_t index_holder;
    bson_iter_t index_value;
    if (!unwrap_iter(args[0], &array_holder, &array_value) ||
        !unwrap_iter(args[1], &index_holder, &index_value)) {
      return std::nullopt;
    }
    if (bson_iter_type(&array_value) != BSON_TYPE_ARRAY ||
        !is_numeric(bson_iter_type(&index_value))) {
      return std::nullopt;
    }

    const std::int64_t target = bson_iter_as_int64(&index_value);
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&array_value, &len, &data);
    bson_t arr;
    if (!bson_init_static(&arr, data, len)) return std::nullopt;
    bson_iter_t it;
    if (!bson_iter_init(&it, &arr)) return std::nullopt;

    std::vector<std::vector<std::uint8_t>> values;
    while (bson_iter_next(&it)) values.push_back(wrap_iter_value(it));
    if (values.empty()) return std::nullopt;

    std::int64_t index = target;
    if (index < 0) index = static_cast<std::int64_t>(values.size()) + index;
    if (index < 0 || index >= static_cast<std::int64_t>(values.size())) {
      return std::nullopt;
    }
    return values[static_cast<std::size_t>(index)];
  }

  return std::nullopt;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> evaluate_expression(
    const bson_t& source_doc, const std::vector<std::uint8_t>& expr_bytes) {
  bson_t expr_holder;
  bson_iter_t expr_iter;
  if (!unwrap_iter(expr_bytes, &expr_holder, &expr_iter)) return std::nullopt;

  std::string field_path;
  if (field_ref_from_iter(expr_iter, &field_path)) {
    bson_iter_t value_iter;
    if (!resolve_path(source_doc, field_path.c_str(), &value_iter)) {
      return std::nullopt;
    }
    return wrap_iter_value(value_iter);
  }
  if (bson_iter_type(&expr_iter) == BSON_TYPE_DOCUMENT) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&expr_iter, &len, &data);
    bson_t expr_doc;
    if (!bson_init_static(&expr_doc, data, len)) return std::nullopt;

    bson_iter_t maybe_op;
    if (bson_iter_init(&maybe_op, &expr_doc) && bson_iter_next(&maybe_op)) {
      const char* first_key = bson_iter_key(&maybe_op);
      if (first_key && first_key[0] == '$') {
        return evaluate_operator_expression(source_doc, expr_doc);
      }
    }

    bson_t built;
    bson_init(&built);
    bson_iter_t child;
    if (bson_iter_init(&child, &expr_doc)) {
      while (bson_iter_next(&child)) {
        const char* key = bson_iter_key(&child);
        if (!key) continue;
        auto child_value = evaluate_expression(source_doc, wrap_iter_value(child));
        if (!child_value) continue;
        append_wrapped_value(&built, key, *child_value);
      }
    }

    bson_t wrap;
    bson_init(&wrap);
    bson_append_document(&wrap, "v", -1, &built);
    auto out = bytes_from_bson(wrap);
    bson_destroy(&wrap);
    bson_destroy(&built);
    return out;
  }
  if (bson_iter_type(&expr_iter) == BSON_TYPE_ARRAY) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_array(&expr_iter, &len, &data);
    bson_t expr_array;
    if (!bson_init_static(&expr_array, data, len)) return std::nullopt;

    bson_t built;
    bson_init(&built);
    bson_t arr;
    bson_append_array_begin(&built, "v", -1, &arr);
    bson_iter_t child;
    std::size_t index = 0;
    if (bson_iter_init(&child, &expr_array)) {
      while (bson_iter_next(&child)) {
        auto child_value = evaluate_expression(source_doc, wrap_iter_value(child));
        if (!child_value) continue;
        append_wrapped_array_item(&arr, index++, *child_value);
      }
    }
    bson_append_array_end(&built, &arr);
    auto out = bytes_from_bson(built);
    bson_destroy(&built);
    return out;
  }
  return expr_bytes;
}

}  // namespace savannah::jungle::query::v1
