#include "savannah/query/project.h"

#include <bson/bson.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

namespace savannah::jungle::query::v1 {

namespace {

bool is_truthy(bson_iter_t it) {
  const bson_type_t t = bson_iter_type(&it);
  if (t == BSON_TYPE_BOOL) return bson_iter_bool(&it);
  if (t == BSON_TYPE_INT32) return bson_iter_int32(&it) != 0;
  if (t == BSON_TYPE_INT64) return bson_iter_int64(&it) != 0;
  if (t == BSON_TYPE_DOUBLE) return bson_iter_double(&it) != 0.0;
  return false;
}

enum class Mode { Inclusion, Exclusion, IdOnly };

struct ParsedSpec {
  Mode mode{Mode::IdOnly};
  bool include_id{true};
  std::unordered_set<std::string> fields;  // membership set (excludes _id)
  bool conflict{false};                    // both incl. and excl. non-_id
};

ParsedSpec parse_spec(const bson_t& spec) {
  ParsedSpec out;
  bool saw_inclusion = false;
  bool saw_exclusion = false;

  bson_iter_t it;
  if (!bson_iter_init(&it, &spec)) return out;
  while (bson_iter_next(&it)) {
    const char* k = bson_iter_key(&it);
    if (!k) continue;
    const bool truthy = is_truthy(it);
    if (std::string_view(k) == "_id") {
      out.include_id = truthy;
      continue;
    }
    if (truthy) {
      saw_inclusion = true;
      out.fields.insert(k);
    } else {
      saw_exclusion = true;
      out.fields.insert(k);
    }
  }

  if (saw_inclusion && saw_exclusion) {
    out.conflict = true;
    return out;
  }
  if (saw_inclusion) {
    out.mode = Mode::Inclusion;
    // Inclusion mode defaults _id IN unless explicitly excluded.
  } else if (saw_exclusion) {
    out.mode = Mode::Exclusion;
    // Exclusion mode also defaults _id IN unless explicitly excluded.
  } else {
    out.mode = Mode::IdOnly;  // empty or only an _id directive
  }
  return out;
}

bool keep_field(std::string_view key, const ParsedSpec& spec) {
  if (key == "_id") return spec.include_id;
  switch (spec.mode) {
    case Mode::Inclusion:
      return spec.fields.contains(std::string(key));
    case Mode::Exclusion:
      return !spec.fields.contains(std::string(key));
    case Mode::IdOnly:
      return true;  // only _id was directed; everything else passes
  }
  return true;
}

}  // namespace

bool has_projection(std::span<const std::uint8_t> spec) {
  if (spec.size() < 5) return false;
  bson_t s;
  if (!bson_init_static(&s, spec.data(), spec.size())) return false;
  bson_iter_t it;
  if (!bson_iter_init(&it, &s)) return false;
  return bson_iter_next(&it);  // any field at all
}

std::vector<std::uint8_t> project(bson::BsonView doc,
                                  std::span<const std::uint8_t> spec_bytes) {
  if (!has_projection(spec_bytes)) {
    return std::vector<std::uint8_t>(doc.data(), doc.data() + doc.size());
  }
  bson_t spec;
  bson_init_static(&spec, spec_bytes.data(), spec_bytes.size());
  ParsedSpec parsed = parse_spec(spec);
  if (parsed.conflict) {
    // Conservative: passthrough rather than guess semantics.
    return std::vector<std::uint8_t>(doc.data(), doc.data() + doc.size());
  }

  bson_t orig;
  if (!bson_init_static(&orig, doc.data(), doc.size())) {
    return std::vector<std::uint8_t>(doc.data(), doc.data() + doc.size());
  }
  bson_t out;
  bson_init(&out);

  // Inclusion: emit _id first if kept, then iterate spec order to preserve
  // declared order of included fields. Exclusion: walk original in order.
  if (parsed.mode == Mode::Inclusion) {
    if (parsed.include_id) {
      bson_iter_t oid;
      if (bson_iter_init(&oid, &orig) && bson_iter_find(&oid, "_id")) {
        bson_append_iter(&out, "_id", -1, &oid);
      }
    }
    bson_iter_t sit;
    bson_iter_init(&sit, &spec);
    while (bson_iter_next(&sit)) {
      const char* k = bson_iter_key(&sit);
      if (!k || std::string_view(k) == "_id") continue;
      if (!is_truthy(sit)) continue;
      bson_iter_t oit;
      if (bson_iter_init(&oit, &orig) && bson_iter_find(&oit, k)) {
        bson_append_iter(&out, k, -1, &oit);
      }
    }
  } else {
    bson_iter_t oit;
    bson_iter_init(&oit, &orig);
    while (bson_iter_next(&oit)) {
      const char* k = bson_iter_key(&oit);
      if (!k) continue;
      if (!keep_field(k, parsed)) continue;
      bson_append_iter(&out, k, -1, &oit);
    }
  }

  const std::uint8_t* data = bson_get_data(&out);
  std::vector<std::uint8_t> bytes(data, data + out.len);
  bson_destroy(&out);
  return bytes;
}

}  // namespace savannah::jungle::query::v1
