#include "savannah/query/expression.h"

#include "savannah/query/value.h"

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
