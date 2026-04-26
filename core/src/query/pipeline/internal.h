#pragma once

// Private header shared across pipeline/*.cpp.
//
// Stages live in their own translation units (match.cpp, shaping.cpp,
// project.cpp, ...) and dispatch.cpp routes to them by name. Anything in
// here is either (a) a tiny utility used by 2+ stages, or (b) a forward
// declaration of an apply_*_stage that dispatch.cpp needs to call.
//
// Per-stage helpers stay in the stage's own .cpp file in an anonymous
// namespace — only promote a helper here when a second stage needs it.

#include "savannah/storage/backend.h"

#include <bson/bson.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace savannah::jungle::query::v1 {

// Many stages reject dotted output paths until subdoc rebuilding lands.
bool top_level_only(std::string_view path);

// Stage entry points called by dispatch.cpp's routing loop.
//
// Each is a pure function: takes the input doc set (and any per-stage
// args), returns a fresh doc set. The pipeline buffer is owned by the
// dispatcher; stages don't touch storage directly except $lookup, which
// receives the backend so it can pull the foreign collection.

std::vector<std::vector<std::uint8_t>> apply_match_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes);

std::vector<std::vector<std::uint8_t>> apply_sort_stage(
    std::vector<std::vector<std::uint8_t>> docs,
    std::span<const std::uint8_t> spec_bytes);

std::vector<std::vector<std::uint8_t>> apply_skip_stage(
    std::vector<std::vector<std::uint8_t>> docs, std::size_t skip);

std::vector<std::vector<std::uint8_t>> apply_limit_stage(
    std::vector<std::vector<std::uint8_t>> docs, std::size_t limit);

std::vector<std::vector<std::uint8_t>> apply_count_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::string_view field_name);

std::vector<std::vector<std::uint8_t>> apply_project_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes);

std::vector<std::vector<std::uint8_t>> apply_add_fields_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    std::span<const std::uint8_t> spec_bytes);

std::vector<std::vector<std::uint8_t>> apply_unset_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    const std::unordered_set<std::string>& fields);

std::vector<std::vector<std::uint8_t>> apply_replace_root_stage(
    const std::vector<std::vector<std::uint8_t>>& docs,
    const std::vector<std::uint8_t>& expr_bytes);

// $group, $sortByCount, $lookup, $unwind still live in dispatch.cpp during
// Pass A of the pipeline split — they share gnarly helpers that aren't
// worth extracting until the simple stages are out of the way. Pass B
// promotes them to their own files and adds the forward declarations here.

}  // namespace savannah::jungle::query::v1
