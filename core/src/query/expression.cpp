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

  // Conditional --------------------------------------------------------------
  //
  // Drivers (and most real aggregations) lean on $cond and $switch to
  // shape per-doc output. Truthiness uses is_truthy_for_expr — null/0/
  // false → false, everything else → true.

  if (std::string_view(op) == "$cond") {
    // {if, then, else} object form OR [if, then, else] array form.
    std::vector<std::vector<std::uint8_t>> branches;
    if (bson_iter_type(&op_it) == BSON_TYPE_DOCUMENT) {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      bson_t obj;
      if (!bson_init_static(&obj, data, len)) return std::nullopt;
      bson_iter_t f;
      std::vector<std::uint8_t> if_e, then_e, else_e;
      if (bson_iter_init_find(&f, &obj, "if")) if_e = wrap_iter_value(f);
      if (bson_iter_init_find(&f, &obj, "then")) then_e = wrap_iter_value(f);
      if (bson_iter_init_find(&f, &obj, "else")) else_e = wrap_iter_value(f);
      if (if_e.empty() || then_e.empty() || else_e.empty()) return std::nullopt;
      auto cond = evaluate_expression(source_doc, if_e);
      if (!cond) return std::nullopt;
      return evaluate_expression(source_doc,
                                 is_truthy_for_expr(*cond) ? then_e : else_e);
    }
    if (bson_iter_type(&op_it) == BSON_TYPE_ARRAY) {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_array(&op_it, &len, &data);
      bson_t arr;
      if (!bson_init_static(&arr, data, len)) return std::nullopt;
      bson_iter_t it;
      if (!bson_iter_init(&it, &arr)) return std::nullopt;
      std::vector<std::vector<std::uint8_t>> raw;
      while (bson_iter_next(&it)) raw.push_back(wrap_iter_value(it));
      if (raw.size() != 3) return std::nullopt;
      auto cond = evaluate_expression(source_doc, raw[0]);
      if (!cond) return std::nullopt;
      return evaluate_expression(source_doc,
                                 is_truthy_for_expr(*cond) ? raw[1] : raw[2]);
    }
    return std::nullopt;
  }

  if (std::string_view(op) == "$switch") {
    // {branches: [{case, then}, ...], default?}
    if (bson_iter_type(&op_it) != BSON_TYPE_DOCUMENT) return std::nullopt;
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&op_it, &len, &data);
    bson_t obj;
    if (!bson_init_static(&obj, data, len)) return std::nullopt;
    bson_iter_t branches_it;
    if (!bson_iter_init_find(&branches_it, &obj, "branches") ||
        bson_iter_type(&branches_it) != BSON_TYPE_ARRAY) return std::nullopt;
    std::uint32_t blen = 0;
    const std::uint8_t* bdata = nullptr;
    bson_iter_array(&branches_it, &blen, &bdata);
    bson_t barr;
    if (!bson_init_static(&barr, bdata, blen)) return std::nullopt;
    bson_iter_t bit;
    if (!bson_iter_init(&bit, &barr)) return std::nullopt;
    while (bson_iter_next(&bit)) {
      if (bson_iter_type(&bit) != BSON_TYPE_DOCUMENT) continue;
      std::uint32_t blen2 = 0;
      const std::uint8_t* bdata2 = nullptr;
      bson_iter_document(&bit, &blen2, &bdata2);
      bson_t branch;
      if (!bson_init_static(&branch, bdata2, blen2)) continue;
      bson_iter_t case_it, then_it;
      if (!bson_iter_init_find(&case_it, &branch, "case") ||
          !bson_iter_init_find(&then_it, &branch, "then")) continue;
      auto case_v = evaluate_expression(source_doc, wrap_iter_value(case_it));
      if (case_v && is_truthy_for_expr(*case_v)) {
        return evaluate_expression(source_doc, wrap_iter_value(then_it));
      }
    }
    bson_iter_t def;
    if (bson_iter_init_find(&def, &obj, "default")) {
      return evaluate_expression(source_doc, wrap_iter_value(def));
    }
    return std::nullopt;  // no match, no default → Mongo throws; we surface missing
  }

  // Boolean (expression form, returns bool) ----------------------------------

  if (std::string_view(op) == "$and" || std::string_view(op) == "$or") {
    if (bson_iter_type(&op_it) != BSON_TYPE_ARRAY) return wrap_bool(false);
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&op_it, &alen, &adata);
    bson_t arr;
    if (!bson_init_static(&arr, adata, alen)) return wrap_bool(false);
    bson_iter_t it;
    if (!bson_iter_init(&it, &arr)) return wrap_bool(false);
    const bool is_and = std::string_view(op) == "$and";
    bool any = false;
    while (bson_iter_next(&it)) {
      any = true;
      auto v = evaluate_expression(source_doc, wrap_iter_value(it));
      const bool t = v && is_truthy_for_expr(*v);
      if (is_and && !t) return wrap_bool(false);
      if (!is_and && t) return wrap_bool(true);
    }
    // $and on empty array → true; $or on empty array → false (per Mongo).
    return wrap_bool(is_and ? true : false);
  }

  if (std::string_view(op) == "$not") {
    // {$not: expr} or {$not: [expr]}; both forms common in the wild.
    std::vector<std::uint8_t> arg;
    if (bson_iter_type(&op_it) == BSON_TYPE_ARRAY) {
      const auto args = evaluate_array_elements(source_doc, op_it);
      if (args.size() != 1) return wrap_bool(false);
      arg = args[0];
    } else {
      auto v = evaluate_expression(source_doc, wrap_iter_value(op_it));
      if (!v) return wrap_bool(true);  // missing → !falsy → true
      arg = *v;
    }
    return wrap_bool(!is_truthy_for_expr(arg));
  }

  // String -------------------------------------------------------------------
  //
  // String semantics here are byte-oriented (UTF-8 byte length, not code
  // points). Mongo distinguishes $substrCP/$substrBytes and
  // $strLenCP/$strLenBytes; we collapse the CP variants to byte semantics
  // until a driver actually exercises a multi-byte character. Smoke
  // checks the byte-orientation explicitly so the assumption is visible.

  auto eval_string_arg = [&](bson_iter_t it) -> std::optional<std::string> {
    auto v = evaluate_expression(source_doc, wrap_iter_value(it));
    if (!v || nullish_wrapped(*v)) return std::nullopt;
    bson_t holder;
    bson_iter_t value_it;
    if (!unwrap_iter(*v, &holder, &value_it)) return std::nullopt;
    if (bson_iter_type(&value_it) != BSON_TYPE_UTF8) return std::nullopt;
    std::uint32_t len = 0;
    const char* text = bson_iter_utf8(&value_it, &len);
    return std::string(text, len);
  };

  if (std::string_view(op) == "$toLower" || std::string_view(op) == "$toUpper") {
    auto s = eval_string_arg(op_it);
    if (!s) return wrap_utf8("");  // Mongo: null/missing → "" for case ops
    const bool to_lower = std::string_view(op) == "$toLower";
    for (auto& c : *s) {
      // ASCII-only fold; full Unicode case mapping is out of scope. Drivers
      // that need locale-aware folding can do it client-side.
      if (to_lower && c >= 'A' && c <= 'Z') c = c + 32;
      if (!to_lower && c >= 'a' && c <= 'z') c = c - 32;
    }
    return wrap_utf8(*s);
  }

  if (std::string_view(op) == "$trim") {
    // {$trim: {input, chars?}} — chars defaults to whitespace. Bare
    // {$trim: "$x"} also works as a string shorthand.
    std::string input;
    std::string chars = " \t\n\r\f\v";
    if (bson_iter_type(&op_it) == BSON_TYPE_DOCUMENT) {
      std::uint32_t len = 0;
      const std::uint8_t* data = nullptr;
      bson_iter_document(&op_it, &len, &data);
      bson_t obj;
      if (!bson_init_static(&obj, data, len)) return wrap_null();
      bson_iter_t f;
      if (!bson_iter_init_find(&f, &obj, "input")) return wrap_null();
      auto v = eval_string_arg(f);
      if (!v) return wrap_null();
      input = *v;
      if (bson_iter_init_find(&f, &obj, "chars")) {
        auto cv = eval_string_arg(f);
        if (cv) chars = *cv;
      }
    } else {
      auto v = eval_string_arg(op_it);
      if (!v) return wrap_null();
      input = *v;
    }
    const auto first = input.find_first_not_of(chars);
    if (first == std::string::npos) return wrap_utf8("");
    const auto last = input.find_last_not_of(chars);
    return wrap_utf8(input.substr(first, last - first + 1));
  }

  if (std::string_view(op) == "$substr" || std::string_view(op) == "$substrBytes" ||
      std::string_view(op) == "$substrCP") {
    // [string, start, length]; length=-1 means "to end of string".
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 3 || args[0].empty()) return wrap_utf8("");
    bson_t hs, hb, hl;
    bson_iter_t is, ib, il;
    if (!unwrap_iter(args[0], &hs, &is) ||
        !unwrap_iter(args[1], &hb, &ib) ||
        !unwrap_iter(args[2], &hl, &il)) return wrap_utf8("");
    if (bson_iter_type(&is) != BSON_TYPE_UTF8) return wrap_utf8("");
    if (!is_numeric(bson_iter_type(&ib)) || !is_numeric(bson_iter_type(&il))) {
      return wrap_utf8("");
    }
    std::uint32_t slen = 0;
    const char* text = bson_iter_utf8(&is, &slen);
    const std::int64_t start = bson_iter_as_int64(&ib);
    const std::int64_t length = bson_iter_as_int64(&il);
    if (start < 0 || start >= static_cast<std::int64_t>(slen)) return wrap_utf8("");
    const std::int64_t avail = static_cast<std::int64_t>(slen) - start;
    const std::int64_t take = (length < 0 || length > avail) ? avail : length;
    return wrap_utf8(std::string_view(text + start, static_cast<std::size_t>(take)));
  }

  if (std::string_view(op) == "$split") {
    // [string, delimiter] — returns an array of substrings.
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 2) return wrap_null();
    bson_t hs, hd;
    bson_iter_t is, id;
    if (!unwrap_iter(args[0], &hs, &is) || !unwrap_iter(args[1], &hd, &id)) {
      return wrap_null();
    }
    if (bson_iter_type(&is) != BSON_TYPE_UTF8 ||
        bson_iter_type(&id) != BSON_TYPE_UTF8) return wrap_null();
    std::uint32_t slen = 0, dlen = 0;
    const char* str = bson_iter_utf8(&is, &slen);
    const char* delim = bson_iter_utf8(&id, &dlen);
    const std::string_view input(str, slen);
    const std::string_view sep(delim, dlen);
    if (sep.empty()) return wrap_null();

    bson_t built;
    bson_init(&built);
    bson_t arr;
    bson_append_array_begin(&built, "v", -1, &arr);
    std::size_t pos = 0;
    std::size_t idx = 0;
    while (true) {
      const auto next = input.find(sep, pos);
      const auto piece = input.substr(pos,
          next == std::string_view::npos ? std::string_view::npos : next - pos);
      const std::string key = std::to_string(idx++);
      bson_append_utf8(&arr, key.c_str(), -1, piece.data(),
                       static_cast<int>(piece.size()));
      if (next == std::string_view::npos) break;
      pos = next + sep.size();
    }
    bson_append_array_end(&built, &arr);
    auto out = bytes_from_bson(built);
    bson_destroy(&built);
    return out;
  }

  if (std::string_view(op) == "$strLenCP" || std::string_view(op) == "$strLenBytes") {
    auto s = eval_string_arg(op_it);
    if (!s) return std::nullopt;
    return wrap_int64(static_cast<std::int64_t>(s->size()));
  }

  // Type conversion + introspection -----------------------------------------
  //
  // $toInt/$toLong/$toDouble parse strings (with strtoll/strtod), pass
  // numerics through (with truncation for $toInt/$toLong on doubles),
  // bool → 0/1. Missing/null returns null. Mongo's $convert with onError
  // is not yet plumbed; failed conversions return null instead of
  // throwing. $type returns Mongo's BSON type name string.

  if (std::string_view(op) == "$toInt" || std::string_view(op) == "$toLong" ||
      std::string_view(op) == "$toDouble" || std::string_view(op) == "$toBool") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value || nullish_wrapped(*value)) return wrap_null();
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*value, &holder, &it)) return wrap_null();
    const auto t = bson_iter_type(&it);
    const std::string_view target = op;

    if (target == "$toBool") {
      switch (t) {
        case BSON_TYPE_BOOL:   return wrap_bool(bson_iter_bool(&it));
        case BSON_TYPE_INT32:  return wrap_bool(bson_iter_int32(&it) != 0);
        case BSON_TYPE_INT64:  return wrap_bool(bson_iter_int64(&it) != 0);
        case BSON_TYPE_DOUBLE: return wrap_bool(bson_iter_double(&it) != 0.0);
        case BSON_TYPE_UTF8: {
          std::uint32_t len = 0;
          bson_iter_utf8(&it, &len);
          return wrap_bool(len > 0);  // any non-empty string → true
        }
        default: return wrap_bool(true);  // any present non-null value
      }
    }

    // Numeric targets share a common parse step.
    double d = 0.0;
    bool got = false;
    switch (t) {
      case BSON_TYPE_INT32:  d = bson_iter_int32(&it); got = true; break;
      case BSON_TYPE_INT64:  d = static_cast<double>(bson_iter_int64(&it)); got = true; break;
      case BSON_TYPE_DOUBLE: d = bson_iter_double(&it); got = true; break;
      case BSON_TYPE_BOOL:   d = bson_iter_bool(&it) ? 1.0 : 0.0; got = true; break;
      case BSON_TYPE_UTF8: {
        std::uint32_t len = 0;
        const char* text = bson_iter_utf8(&it, &len);
        const std::string s(text, len);
        try {
          if (target == "$toDouble") d = std::stod(s);
          else                       d = static_cast<double>(std::stoll(s));
          got = true;
        } catch (...) { return wrap_null(); }
        break;
      }
      default: return wrap_null();
    }
    if (!got) return wrap_null();
    if (target == "$toDouble") return wrap_double(d);
    return wrap_int64(static_cast<std::int64_t>(d));  // $toInt and $toLong
  }

  if (std::string_view(op) == "$type") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value) return wrap_utf8("missing");
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*value, &holder, &it)) return wrap_utf8("missing");
    switch (bson_iter_type(&it)) {
      case BSON_TYPE_DOUBLE:    return wrap_utf8("double");
      case BSON_TYPE_UTF8:      return wrap_utf8("string");
      case BSON_TYPE_DOCUMENT:  return wrap_utf8("object");
      case BSON_TYPE_ARRAY:     return wrap_utf8("array");
      case BSON_TYPE_BINARY:    return wrap_utf8("binData");
      case BSON_TYPE_OID:       return wrap_utf8("objectId");
      case BSON_TYPE_BOOL:      return wrap_utf8("bool");
      case BSON_TYPE_DATE_TIME: return wrap_utf8("date");
      case BSON_TYPE_NULL:      return wrap_utf8("null");
      case BSON_TYPE_INT32:     return wrap_utf8("int");
      case BSON_TYPE_TIMESTAMP: return wrap_utf8("timestamp");
      case BSON_TYPE_INT64:     return wrap_utf8("long");
      case BSON_TYPE_DECIMAL128: return wrap_utf8("decimal");
      default:                  return wrap_utf8("missing");
    }
  }

  if (std::string_view(op) == "$isNumber") {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value) return wrap_bool(false);
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*value, &holder, &it)) return wrap_bool(false);
    return wrap_bool(is_numeric(bson_iter_type(&it)));
  }

  // Aggregator-as-expression + math -----------------------------------------
  //
  // $sum/$avg/$min/$max also exist as $group accumulators (in group.cpp).
  // The expression form takes a single arg that resolves to either a
  // numeric or an array of values, and folds across that array.
  // [<e1>, <e2>, ...] also works because evaluate_expression on an array
  // literal returns an evaluated array.

  auto fold_array = [&](auto on_each) -> bool {
    auto value = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!value) return false;
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*value, &holder, &it)) return false;
    if (bson_iter_type(&it) == BSON_TYPE_ARRAY) {
      std::uint32_t alen = 0;
      const std::uint8_t* adata = nullptr;
      bson_iter_array(&it, &alen, &adata);
      bson_t arr;
      if (!bson_init_static(&arr, adata, alen)) return false;
      bson_iter_t ait;
      if (!bson_iter_init(&ait, &arr)) return false;
      while (bson_iter_next(&ait)) on_each(ait);
    } else {
      on_each(it);
    }
    return true;
  };

  if (std::string_view(op) == "$sum") {
    bool any_double = false;
    double acc_d = 0.0;
    std::int64_t acc_i = 0;
    if (!fold_array([&](bson_iter_t it) {
      if (!is_numeric(bson_iter_type(&it))) return;  // skip non-numeric
      if (bson_iter_type(&it) == BSON_TYPE_DOUBLE) any_double = true;
      acc_d += bson_iter_as_double(&it);
      acc_i += bson_iter_as_int64(&it);
    })) return wrap_int64(0);
    return any_double ? wrap_double(acc_d) : wrap_int64(acc_i);
  }

  if (std::string_view(op) == "$avg") {
    double sum = 0.0;
    std::int64_t count = 0;
    if (!fold_array([&](bson_iter_t it) {
      if (!is_numeric(bson_iter_type(&it))) return;
      sum += bson_iter_as_double(&it);
      ++count;
    })) return wrap_null();
    if (count == 0) return wrap_null();
    return wrap_double(sum / static_cast<double>(count));
  }

  if (std::string_view(op) == "$min" || std::string_view(op) == "$max") {
    const bool want_min = std::string_view(op) == "$min";
    std::vector<std::uint8_t> best;
    bool seen = false;
    if (!fold_array([&](bson_iter_t it) {
      if (bson_iter_type(&it) == BSON_TYPE_NULL) return;
      auto wrapped = wrap_iter_value(it);
      if (!seen) { best = std::move(wrapped); seen = true; return; }
      bson_t hb, hc;
      bson_iter_t ib, ic;
      if (!unwrap_iter(best, &hb, &ib) || !unwrap_iter(wrapped, &hc, &ic)) return;
      auto cmp = value_compare(ic, ib);
      if (!cmp) return;
      if (( want_min && *cmp < 0) || (!want_min && *cmp > 0)) best = std::move(wrapped);
    })) return wrap_null();
    if (!seen) return wrap_null();
    return best;
  }

  if (std::string_view(op) == "$pow") {
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 2) return wrap_null();
    bson_t hb, he;
    bson_iter_t ib, ie;
    if (!unwrap_iter(args[0], &hb, &ib) || !unwrap_iter(args[1], &he, &ie)) return wrap_null();
    if (!is_numeric(bson_iter_type(&ib)) || !is_numeric(bson_iter_type(&ie))) return wrap_null();
    return wrap_double(std::pow(bson_iter_as_double(&ib), bson_iter_as_double(&ie)));
  }

  if (std::string_view(op) == "$sqrt" || std::string_view(op) == "$exp" ||
      std::string_view(op) == "$ln" || std::string_view(op) == "$log10") {
    auto v = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!v || nullish_wrapped(*v)) return wrap_null();
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*v, &holder, &it)) return wrap_null();
    if (!is_numeric(bson_iter_type(&it))) return wrap_null();
    const double d = bson_iter_as_double(&it);
    if (std::string_view(op) == "$sqrt")  return wrap_double(std::sqrt(d));
    if (std::string_view(op) == "$exp")   return wrap_double(std::exp(d));
    if (std::string_view(op) == "$ln")    return wrap_double(std::log(d));
    return wrap_double(std::log10(d));
  }

  if (std::string_view(op) == "$log") {
    // [number, base] — log base `base` of number.
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 2) return wrap_null();
    bson_t hn, hb;
    bson_iter_t in, ib;
    if (!unwrap_iter(args[0], &hn, &in) || !unwrap_iter(args[1], &hb, &ib)) return wrap_null();
    if (!is_numeric(bson_iter_type(&in)) || !is_numeric(bson_iter_type(&ib))) return wrap_null();
    return wrap_double(std::log(bson_iter_as_double(&in)) /
                       std::log(bson_iter_as_double(&ib)));
  }

  // Array helpers ------------------------------------------------------------

  if (std::string_view(op) == "$concatArrays") {
    // Args is an array of arrays; concatenate in order. Any non-array → null.
    if (bson_iter_type(&op_it) != BSON_TYPE_ARRAY) return wrap_null();
    const auto args = evaluate_array_elements(source_doc, op_it);
    bson_t built;
    bson_init(&built);
    bson_t arr;
    bson_append_array_begin(&built, "v", -1, &arr);
    std::size_t out_idx = 0;
    for (const auto& w : args) {
      if (w.empty() || nullish_wrapped(w)) {
        bson_append_array_end(&built, &arr);
        bson_destroy(&built);
        return wrap_null();
      }
      bson_t holder;
      bson_iter_t it;
      if (!unwrap_iter(w, &holder, &it) || bson_iter_type(&it) != BSON_TYPE_ARRAY) {
        bson_append_array_end(&built, &arr);
        bson_destroy(&built);
        return wrap_null();
      }
      std::uint32_t alen = 0;
      const std::uint8_t* adata = nullptr;
      bson_iter_array(&it, &alen, &adata);
      bson_t inner;
      if (!bson_init_static(&inner, adata, alen)) continue;
      bson_iter_t iit;
      if (!bson_iter_init(&iit, &inner)) continue;
      while (bson_iter_next(&iit)) {
        const std::string key = std::to_string(out_idx++);
        bson_append_iter(&arr, key.c_str(), -1, &iit);
      }
    }
    bson_append_array_end(&built, &arr);
    auto out = bytes_from_bson(built);
    bson_destroy(&built);
    return out;
  }

  if (std::string_view(op) == "$slice") {
    // [array, n] OR [array, position, n]. Negative n in 2-arg form means
    // last |n| elements; in 3-arg form n is always non-negative count.
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() < 2 || args.size() > 3 || args[0].empty()) return wrap_null();
    bson_t harr;
    bson_iter_t iarr;
    if (!unwrap_iter(args[0], &harr, &iarr) || bson_iter_type(&iarr) != BSON_TYPE_ARRAY) {
      return wrap_null();
    }
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&iarr, &alen, &adata);
    bson_t inner;
    if (!bson_init_static(&inner, adata, alen)) return wrap_null();
    std::vector<std::vector<std::uint8_t>> elems;
    bson_iter_t it;
    if (bson_iter_init(&it, &inner)) {
      while (bson_iter_next(&it)) elems.push_back(wrap_iter_value(it));
    }
    const std::int64_t total = static_cast<std::int64_t>(elems.size());

    std::int64_t start = 0;
    std::int64_t count = 0;
    if (args.size() == 2) {
      bson_t hn;
      bson_iter_t in;
      if (!unwrap_iter(args[1], &hn, &in) || !is_numeric(bson_iter_type(&in))) return wrap_null();
      const std::int64_t n = bson_iter_as_int64(&in);
      if (n >= 0) { start = 0; count = std::min(n, total); }
      else        { start = std::max<std::int64_t>(0, total + n); count = total - start; }
    } else {
      bson_t hp, hn;
      bson_iter_t ip, in;
      if (!unwrap_iter(args[1], &hp, &ip) || !unwrap_iter(args[2], &hn, &in)) return wrap_null();
      if (!is_numeric(bson_iter_type(&ip)) || !is_numeric(bson_iter_type(&in))) return wrap_null();
      const std::int64_t pos = bson_iter_as_int64(&ip);
      start = pos < 0 ? std::max<std::int64_t>(0, total + pos) : std::min(pos, total);
      count = std::min(bson_iter_as_int64(&in), total - start);
      if (count < 0) count = 0;
    }

    bson_t built;
    bson_init(&built);
    bson_t out_arr;
    bson_append_array_begin(&built, "v", -1, &out_arr);
    for (std::int64_t i = 0; i < count; ++i) {
      append_wrapped_array_item(&out_arr, static_cast<std::size_t>(i),
                                elems[static_cast<std::size_t>(start + i)]);
    }
    bson_append_array_end(&built, &out_arr);
    auto bytes = bytes_from_bson(built);
    bson_destroy(&built);
    return bytes;
  }

  if (std::string_view(op) == "$range") {
    // [start, end, step?] — step defaults to 1. step must be non-zero.
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() < 2 || args.size() > 3) return wrap_null();
    auto get_int = [&](const std::vector<std::uint8_t>& w, std::int64_t* out) {
      bson_t h;
      bson_iter_t it;
      if (!unwrap_iter(w, &h, &it) || !is_numeric(bson_iter_type(&it))) return false;
      *out = bson_iter_as_int64(&it);
      return true;
    };
    std::int64_t start = 0, end = 0, step = 1;
    if (!get_int(args[0], &start) || !get_int(args[1], &end)) return wrap_null();
    if (args.size() == 3 && !get_int(args[2], &step)) return wrap_null();
    if (step == 0) return wrap_null();

    bson_t built;
    bson_init(&built);
    bson_t arr;
    bson_append_array_begin(&built, "v", -1, &arr);
    std::size_t idx = 0;
    if (step > 0) {
      for (std::int64_t v = start; v < end; v += step) {
        const std::string key = std::to_string(idx++);
        bson_append_int64(&arr, key.c_str(), -1, v);
      }
    } else {
      for (std::int64_t v = start; v > end; v += step) {
        const std::string key = std::to_string(idx++);
        bson_append_int64(&arr, key.c_str(), -1, v);
      }
    }
    bson_append_array_end(&built, &arr);
    auto bytes = bytes_from_bson(built);
    bson_destroy(&built);
    return bytes;
  }

  if (std::string_view(op) == "$in") {
    // [value, array] → bool
    const auto args = evaluate_array_elements(source_doc, op_it);
    if (args.size() != 2 || args[1].empty()) return wrap_bool(false);
    bson_t harr;
    bson_iter_t iarr;
    if (!unwrap_iter(args[1], &harr, &iarr) || bson_iter_type(&iarr) != BSON_TYPE_ARRAY) {
      return wrap_bool(false);
    }
    if (args[0].empty()) return wrap_bool(false);
    bson_t hv;
    bson_iter_t iv;
    if (!unwrap_iter(args[0], &hv, &iv)) return wrap_bool(false);
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&iarr, &alen, &adata);
    bson_t inner;
    if (!bson_init_static(&inner, adata, alen)) return wrap_bool(false);
    bson_iter_t it;
    if (!bson_iter_init(&it, &inner)) return wrap_bool(false);
    while (bson_iter_next(&it)) {
      // Re-init iv each iteration because value_equal may consume state
      // implicitly (libbson iterators are stateful but copy-by-value here).
      bson_iter_t iv_copy = iv;
      if (value_equal(iv_copy, it)) return wrap_bool(true);
    }
    return wrap_bool(false);
  }

  if (std::string_view(op) == "$isArray") {
    auto v = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!v) return wrap_bool(false);
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*v, &holder, &it)) return wrap_bool(false);
    return wrap_bool(bson_iter_type(&it) == BSON_TYPE_ARRAY);
  }

  if (std::string_view(op) == "$first" || std::string_view(op) == "$last") {
    auto v = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!v || nullish_wrapped(*v)) return wrap_null();
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*v, &holder, &it)) return wrap_null();
    if (bson_iter_type(&it) != BSON_TYPE_ARRAY) return wrap_null();
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&it, &alen, &adata);
    bson_t inner;
    if (!bson_init_static(&inner, adata, alen)) return wrap_null();
    std::vector<std::vector<std::uint8_t>> items;
    bson_iter_t iit;
    if (bson_iter_init(&iit, &inner)) {
      while (bson_iter_next(&iit)) items.push_back(wrap_iter_value(iit));
    }
    if (items.empty()) return wrap_null();
    return std::string_view(op) == "$first" ? items.front() : items.back();
  }

  if (std::string_view(op) == "$reverseArray") {
    auto v = evaluate_expression(source_doc, wrap_iter_value(op_it));
    if (!v || nullish_wrapped(*v)) return wrap_null();
    bson_t holder;
    bson_iter_t it;
    if (!unwrap_iter(*v, &holder, &it)) return wrap_null();
    if (bson_iter_type(&it) != BSON_TYPE_ARRAY) return wrap_null();
    std::uint32_t alen = 0;
    const std::uint8_t* adata = nullptr;
    bson_iter_array(&it, &alen, &adata);
    bson_t inner;
    if (!bson_init_static(&inner, adata, alen)) return wrap_null();
    std::vector<std::vector<std::uint8_t>> items;
    bson_iter_t iit;
    if (bson_iter_init(&iit, &inner)) {
      while (bson_iter_next(&iit)) items.push_back(wrap_iter_value(iit));
    }
    bson_t built;
    bson_init(&built);
    bson_t out_arr;
    bson_append_array_begin(&built, "v", -1, &out_arr);
    for (std::size_t i = 0; i < items.size(); ++i) {
      append_wrapped_array_item(&out_arr, i, items[items.size() - 1 - i]);
    }
    bson_append_array_end(&built, &out_arr);
    auto bytes = bytes_from_bson(built);
    bson_destroy(&built);
    return bytes;
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
