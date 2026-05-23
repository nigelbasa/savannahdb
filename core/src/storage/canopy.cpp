#include "savannah/storage/canopy.h"

#include <cstdint>
#include <string>

namespace savannah::storage::canopy {

namespace {

// Namespace components become path segments on disk. Encoding every byte keeps
// logical names one-to-one even on case-insensitive filesystems and avoids
// "..", slashes, or reserved characters aliasing other collections.
std::string encode_path_component(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "n-";
  out.reserve(2 + value.size() * 2);
  for (unsigned char byte : std::string(value)) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

}  // namespace

std::filesystem::path Layout::manifest_path() const {
  return root_dir_ / "manifest.json";
}

std::filesystem::path Layout::wal_dir() const {
  return root_dir_ / "wal";
}

std::filesystem::path Layout::collections_dir() const {
  return root_dir_ / "collections";
}

std::filesystem::path Layout::db_dir(std::string_view db_name) const {
  return collections_dir() / encode_path_component(db_name);
}

std::filesystem::path Layout::collection_dir(
    std::string_view db_name, std::string_view coll_name) const {
  return db_dir(db_name) / encode_path_component(coll_name);
}

std::filesystem::path Layout::collection_log_path(
    std::string_view db_name, std::string_view coll_name) const {
  return collection_dir(db_name, coll_name) / "ops.bin";
}

std::filesystem::path Layout::collection_state_path(
    std::string_view db_name, std::string_view coll_name) const {
  return collection_dir(db_name, coll_name) / "state.bin";
}

}  // namespace savannah::storage::canopy
