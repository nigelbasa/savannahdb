#pragma once

// MQL aggregation expression evaluator + the small "wrapped value" helpers
// it shares with pipeline stages.
//
// `evaluate_expression(source, expr_bytes)` evaluates an MQL expression
// against a source document and returns a `{v: <value>}` envelope (or
// nullopt if the expression resolves to missing). The envelope shape is
// what lets the rest of the pipeline pass arbitrary BSON values around as
// owned byte buffers without having to deal with libbson's bson_value_t
// lifecycle.
//
// The wrap/unwrap helpers are exposed so pipeline stages (group, project,
// replaceRoot, etc.) can build, inspect, and emit values without
// reimplementing the envelope convention.

#include "savannah/bson/document.h"

#include <bson/bson.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savannah::jungle::query::v1 {

// Generic BSON helpers ------------------------------------------------------

bool init_static_bson(std::span<const std::uint8_t> bytes, bson_t* out);
std::span<const std::uint8_t> empty_bson();
std::vector<std::uint8_t> copy_bytes(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> owned_bytes(bson::BsonView view);
std::vector<std::uint8_t> bytes_from_bson(const bson_t& doc);

// Wrapped-value helpers -----------------------------------------------------
//
// A "wrapped value" is a self-contained BSON document of the form
// `{v: <value>}`. Iterating into it via unwrap_iter() gives a bson_iter_t
// pointing at the value, which feeds the shared value_compare/value_equal
// machinery in query/value.h.

std::vector<std::uint8_t> wrap_iter_value(bson_iter_t iter);
std::vector<std::uint8_t> wrap_null();
std::vector<std::uint8_t> wrap_utf8(std::string_view value);
std::vector<std::uint8_t> wrap_int64(std::int64_t value);

// Append a wrapped value into `out` under `key`. Returns false if the
// envelope is malformed. Caller must keep `wrapped` alive for the call.
bool append_wrapped_value(bson_t* out, std::string_view key,
                          const std::vector<std::uint8_t>& wrapped);
bool append_wrapped_array_item(bson_t* out, std::size_t index,
                               const std::vector<std::uint8_t>& wrapped);

// `holder` is an output bson_t the caller owns (for libbson's static-init
// lifetime); `out` is the iter to use afterwards. Returns true on success.
bool unwrap_iter(const std::vector<std::uint8_t>& wrapped, bson_t* holder,
                 bson_iter_t* out);

// Convenience: unwrap and re-extract just the document/array payload as
// owned bytes. Returns nullopt on type mismatch or malformed envelope.
std::optional<std::vector<std::uint8_t>> unwrap_document_bytes(
    const std::vector<std::uint8_t>& wrapped);
std::optional<std::vector<std::uint8_t>> unwrap_array_bytes(
    const std::vector<std::uint8_t>& wrapped);

bool nullish_wrapped(const std::vector<std::uint8_t>& wrapped);

// Type predicates -----------------------------------------------------------

// Truthy in the MQL projection-flag sense (boolean, or non-zero numeric).
bool is_truthy_projection_value(bson_iter_t iter);

// "$field.path" → out := "field.path", returning true iff iter is a UTF8
// value starting with `$`. Used by expression eval to detect field refs.
bool field_ref_from_iter(bson_iter_t iter, std::string* out);

// Evaluator -----------------------------------------------------------------
//
// Recursively evaluate `expr_bytes` (a wrapped-value envelope holding the
// expression) against `source_doc`. Returns a wrapped-value envelope
// holding the result, or nullopt if the expression resolves to missing.
std::optional<std::vector<std::uint8_t>> evaluate_expression(
    const bson_t& source_doc, const std::vector<std::uint8_t>& expr_bytes);

}  // namespace savannah::jungle::query::v1
