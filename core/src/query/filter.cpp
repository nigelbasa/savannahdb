#include "savannah/query/filter.h"

#include "savannah/query/expression.h"
#include "savannah/query/value.h"

#include <bson/bson.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savannah::jungle::query::v1 {

namespace {

// Forward declarations - operator evaluators recurse into match_doc for
// $and/$or, which in turn dispatches back through here.
bool match_doc(bson::BsonView doc, std::span<const std::uint8_t> filter);

// Cap on $and/$or nesting. Matches the rationale on the expression
// evaluator's depth cap — protects the C++ stack against pathologically
// nested filters sent via the REST API. Counter is thread-local; the RAII
// guard decrements on every exit path including thrown exceptions.
constexpr int kMaxFilterDepth = 100;
thread_local int g_filter_depth = 0;

struct FilterDepthGuard {
  bool exceeded;
  FilterDepthGuard() { exceeded = ++g_filter_depth > kMaxFilterDepth; }
  ~FilterDepthGuard() { --g_filter_depth; }
};

// is_numeric, value_equal, value_compare live in query/value.cpp - single
// source of truth shared with sort and the index comparator.

// ---------------------------------------------------------------------------
// Per-field operator evaluation
// ---------------------------------------------------------------------------

bool is_operator_doc(bson_iter_t f) {
  if (bson_iter_type(&f) != BSON_TYPE_DOCUMENT) return false;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_document(&f, &len, &data);
  bson_t sub;
  if (!bson_init_static(&sub, data, len)) return false;
  bson_iter_t it;
  if (!bson_iter_init(&it, &sub)) return false;
  if (!bson_iter_next(&it)) return false;
  const char* k = bson_iter_key(&it);
  return k && k[0] == '$';
}

// Iterate `arr_iter`'s array contents and call `fn(elem_iter)` for each.
// libbson models arrays as docs with stringified-int keys, so we re-init
// the array payload as a bson_t and walk it.
template <typename F>
bool for_each_array_elem(bson_iter_t arr_iter, F&& fn) {
  if (bson_iter_type(&arr_iter) != BSON_TYPE_ARRAY) return false;
  std::uint32_t len = 0;
  const std::uint8_t* data = nullptr;
  bson_iter_array(&arr_iter, &len, &data);
  bson_t sub;
  if (!bson_init_static(&sub, data, len)) return false;
  bson_iter_t it;
  if (!bson_iter_init(&it, &sub)) return false;
  while (bson_iter_next(&it)) {
    fn(it);
  }
  return true;
}

// Forward decl — defined below, after `equals_or_contains`. We need it here
// so $in/$nin can fall back to element-of-array semantics when the doc field
// is itself an array.
bool equals_or_contains(bson_iter_t filter_val, bson_iter_t doc_val);

bool eval_in(bson_iter_t op_val, bool present, bson_iter_t doc_val) {
  if (!present) return false;
  bool any = false;
  for_each_array_elem(op_val, [&](bson_iter_t elem) {
    if (!any && equals_or_contains(elem, doc_val)) any = true;
  });
  return any;
}

bool eval_nin(bson_iter_t op_val, bool present, bson_iter_t doc_val) {
  if (!present) return true;  // Like $ne - missing field passes.
  bool any = false;
  for_each_array_elem(op_val, [&](bson_iter_t elem) {
    if (!any && equals_or_contains(elem, doc_val)) any = true;
  });
  return !any;
}

// MongoDB regex options to std::regex flags. We use the ECMAScript flavor,
// which is close enough to PCRE for the patterns drivers commonly send.
// Notes:
//   - 's' (dotall): not native to std::regex ECMAScript mode, but we
//     emulate it by rewriting unescaped `.` outside char classes to
//     `[\s\S]` (matches any char including newlines).
//   - 'x' (extended/verbose): not supported. Silently ignored.
std::regex_constants::syntax_option_type regex_flags(std::string_view options) {
  auto flags = std::regex::ECMAScript;
  for (const char c : options) {
    if (c == 'i') flags |= std::regex::icase;
    // 'm' (multiline): not supported natively by std::regex. Silently ignored.
    // 's' handled via pattern rewrite (see rewrite_dotall_pattern); 'x' ignored.
  }
  return flags;
}

// If `s` (dotall) is set, rewrite each unescaped `.` outside a character
// class to `[\s\S]`. We track escape state and char-class depth so we don't
// touch `\.` or `.` inside `[...]`. Returns the original pattern when 's'
// isn't requested — avoids the allocation in the hot path.
std::string rewrite_dotall_pattern(std::string_view pattern,
                                   std::string_view options) {
  bool dotall = false;
  for (const char c : options) {
    if (c == 's') { dotall = true; break; }
  }
  if (!dotall) return std::string(pattern);

  std::string out;
  out.reserve(pattern.size() + 8);
  bool in_class = false;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    if (c == '\\' && i + 1 < pattern.size()) {
      out.push_back(c);
      out.push_back(pattern[i + 1]);
      ++i;
      continue;
    }
    if (!in_class && c == '[') { in_class = true; out.push_back(c); continue; }
    if (in_class && c == ']')  { in_class = false; out.push_back(c); continue; }
    if (!in_class && c == '.') { out += "[\\s\\S]"; continue; }
    out.push_back(c);
  }
  return out;
}

bool regex_match_doc_string(std::string_view pattern, std::string_view options,
                            bson_iter_t doc_val) {
  if (bson_iter_type(&doc_val) != BSON_TYPE_UTF8) return false;
  std::uint32_t dl = 0;
  const char* ds = bson_iter_utf8(&doc_val, &dl);
  try {
    const std::string effective = rewrite_dotall_pattern(pattern, options);
    std::regex re(effective.data(), effective.size(), regex_flags(options));
    // MQL regex is partial-match (regex_search), not anchored full-match.
    return std::regex_search(ds, ds + dl, re);
  } catch (const std::regex_error&) {
    return false;
  }
}

// $regex value can be either a UTF8 string or a BSON_TYPE_REGEX. In the
// REGEX case the options are embedded; otherwise a sibling $options string
// (extracted in advance by eval_field) supplies them.
bool eval_regex_op(bson_iter_t op_val, std::string_view sibling_options,
                   bool present, bson_iter_t doc_val) {
  if (!present) return false;
  if (bson_iter_type(&op_val) == BSON_TYPE_UTF8) {
    std::uint32_t pl = 0;
    const char* pat = bson_iter_utf8(&op_val, &pl);
    return regex_match_doc_string({pat, pl}, sibling_options, doc_val);
  }
  if (bson_iter_type(&op_val) == BSON_TYPE_REGEX) {
    const char* opts = nullptr;
    const char* pat = bson_iter_regex(&op_val, &opts);
    return regex_match_doc_string(pat ? std::string_view(pat) : "",
                                  opts ? std::string_view(opts) : "", doc_val);
  }
  return false;
}

bool eval_exists(bson_iter_t op_val, bool present) {
  // Mongo accepts any truthy/falsy: bool, numeric, etc. Bool is the common
  // case from drivers; treat numerics via as_bool for safety.
  bool want = false;
  const bson_type_t t = bson_iter_type(&op_val);
  if (t == BSON_TYPE_BOOL) {
    want = bson_iter_bool(&op_val);
  } else if (is_numeric(t)) {
    want = bson_iter_as_int64(&op_val) != 0;
  }
  return want ? present : !present;
}

// Mongo equality on arrays is element-aware: `{tags: 'a'}` matches both
// `{tags: 'a'}` and `{tags: ['a','b']}`. Compare directly first (covers
// whole-array equality if the filter is also an array), then walk array
// elements for scalar-in-array matching.
bool equals_or_contains(bson_iter_t filter_val, bson_iter_t doc_val) {
  if (value_equal(filter_val, doc_val)) return true;
  if (bson_iter_type(&doc_val) != BSON_TYPE_ARRAY) return false;
  std::uint32_t alen = 0;
  const std::uint8_t* adata = nullptr;
  bson_iter_array(&doc_val, &alen, &adata);
  bson_t arr;
  if (!bson_init_static(&arr, adata, alen)) return false;
  bson_iter_t elem;
  if (!bson_iter_init(&elem, &arr)) return false;
  while (bson_iter_next(&elem)) {
    if (value_equal(filter_val, elem)) return true;
  }
  return false;
}

bool eval_operator(std::string_view op, bson_iter_t op_val,
                   std::string_view sibling_options, bool present,
                   bson_iter_t doc_val) {
  if (op == "$eq") return present && equals_or_contains(op_val, doc_val);
  if (op == "$ne") return !present || !equals_or_contains(op_val, doc_val);
  if (op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
    if (!present) return false;
    auto cmp = value_compare(doc_val, op_val);
    if (!cmp) return false;
    if (op == "$gt") return *cmp > 0;
    if (op == "$gte") return *cmp >= 0;
    if (op == "$lt") return *cmp < 0;
    return *cmp <= 0;
  }
  if (op == "$in") return eval_in(op_val, present, doc_val);
  if (op == "$nin") return eval_nin(op_val, present, doc_val);
  if (op == "$exists") return eval_exists(op_val, present);
  if (op == "$regex") {
    return eval_regex_op(op_val, sibling_options, present, doc_val);
  }
  // $options is consumed by $regex; alone it's a no-op (Mongo treats it so).
  if (op == "$options") return true;
  // Unknown ops conservatively reject.
  return false;
}

bool eval_field(bson_iter_t field_filter, bson::BsonView doc) {
  const char* key = bson_iter_key(&field_filter);
  if (!key) return false;

  bson_t d;
  if (!bson_init_static(&d, doc.data(), doc.size())) return false;
  bson_iter_t dit;
  bool present = resolve_path(d, key, &dit);

  if (is_operator_doc(field_filter)) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&field_filter, &len, &data);
    bson_t sub;
    if (!bson_init_static(&sub, data, len)) return false;

    // Pre-pass: extract $options so $regex (which may appear before or after
    // $options in the subdoc) sees them. $options without $regex is a no-op.
    std::string options;
    {
      bson_iter_t scan;
      if (bson_iter_init(&scan, &sub)) {
        while (bson_iter_next(&scan)) {
          const char* k = bson_iter_key(&scan);
          if (k && std::string_view(k) == "$options" &&
              bson_iter_type(&scan) == BSON_TYPE_UTF8) {
            std::uint32_t ol = 0;
            const char* os = bson_iter_utf8(&scan, &ol);
            options.assign(os, ol);
          }
        }
      }
    }

    bson_iter_t op;
    if (!bson_iter_init(&op, &sub)) return false;
    while (bson_iter_next(&op)) {
      const char* opname = bson_iter_key(&op);
      if (!opname || opname[0] != '$') return false;
      if (!eval_operator(opname, op, options, present, dit)) return false;
    }
    return true;
  }

  // Literal BSON regex value `{field: /pat/i}` - sugar for $regex.
  if (bson_iter_type(&field_filter) == BSON_TYPE_REGEX) {
    if (!present) return false;
    const char* opts = nullptr;
    const char* pat = bson_iter_regex(&field_filter, &opts);
    return regex_match_doc_string(pat ? std::string_view(pat) : "",
                                  opts ? std::string_view(opts) : "", dit);
  }

  if (!present) return false;
  return equals_or_contains(field_filter, dit);
}

// ---------------------------------------------------------------------------
// Top-level operators ($and / $or / $expr)
// ---------------------------------------------------------------------------

bool eval_logical(std::string_view op, bson_iter_t op_val,
                  bson::BsonView doc) {
  if (bson_iter_type(&op_val) != BSON_TYPE_ARRAY) return false;
  bool seen = false;
  bool result = (op == "$and");  // AND identity true; OR starts false.
  for_each_array_elem(op_val, [&](bson_iter_t elem) {
    if (bson_iter_type(&elem) != BSON_TYPE_DOCUMENT) {
      result = false;
      seen = true;
      return;
    }
    seen = true;
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    bson_iter_document(&elem, &len, &data);
    const bool m =
        match_doc(doc, std::span<const std::uint8_t>{data, len});
    if (op == "$and") {
      result = result && m;
    } else {
      result = result || m;
    }
  });
  // Mongo treats empty $and/$or arrays as errors; we just no-match.
  return seen && result;
}

bool is_truthy_expr_value(const std::vector<std::uint8_t>& wrapped) {
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

bool eval_top_operator(std::string_view op, bson_iter_t op_val,
                       bson::BsonView doc) {
  if (op == "$and" || op == "$or") return eval_logical(op, op_val, doc);
  if (op == "$expr") {
    bson_t source_doc;
    if (!bson_init_static(&source_doc, doc.data(), doc.size())) return false;
    auto evaluated = evaluate_expression(source_doc, wrap_iter_value(op_val));
    return evaluated && is_truthy_expr_value(*evaluated);
  }
  // $nor / $not / etc. remain out of scope.
  return false;
}

bool match_doc(bson::BsonView doc, std::span<const std::uint8_t> filter) {
  FilterDepthGuard depth_guard;
  if (depth_guard.exceeded) return false;

  bson_t f;
  if (filter.size() < 5 || !bson_init_static(&f, filter.data(), filter.size())) {
    return filter.size() == 5;
  }
  bson_iter_t fit;
  if (!bson_iter_init(&fit, &f)) return false;
  while (bson_iter_next(&fit)) {
    const char* key = bson_iter_key(&fit);
    if (!key) return false;
    if (key[0] == '$') {
      if (!eval_top_operator(key, fit, doc)) return false;
    } else {
      if (!eval_field(fit, doc)) return false;
    }
  }
  return true;
}

}  // namespace

bool matches(bson::BsonView doc, std::span<const std::uint8_t> filter) {
  return match_doc(doc, filter);
}

}  // namespace savannah::jungle::query::v1
