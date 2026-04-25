#pragma once

// MQL projection. Slice F3 supports top-level inclusion `{a:1, b:1}` and
// exclusion `{a:0}` modes, plus the `_id` opt-out (`{_id: 0}` paired with
// either mode is allowed). Mixing inclusion and exclusion of non-_id fields
// is a Mongo error (code 31254); we conservatively passthrough the doc
// unchanged in that case rather than make up semantics.
//
// Out of scope (Phase 0.4+): dotted projections (`{"a.b": 1}` would need
// subdoc rebuilding), `$slice`, `$elemMatch`, computed fields, `$meta`.

#include "savannah/bson/document.h"

#include <cstdint>
#include <span>
#include <vector>

namespace savannah::jungle::query::v1 {

// Returns the projected doc bytes. If the spec is empty/unrecognized the
// caller can either skip the call or pass through; we return the original
// bytes verbatim so it is safe to call unconditionally.
std::vector<std::uint8_t> project(bson::BsonView doc,
                                  std::span<const std::uint8_t> spec);

// True iff the spec has at least one meaningful key — useful for the
// binding to decide whether to wrap the iterator in a projector at all.
bool has_projection(std::span<const std::uint8_t> spec);

}  // namespace savannah::jungle::query::v1
