#pragma once

#include <filesystem>
#include <string_view>

namespace savannah::storage::canopy {

// Canopy is SavannahDB's custom durable-engine line. The first durable slice
// uses append-only collection logs plus in-memory replay so the query engine
// can stay unchanged while the filesystem contract settles.
class Layout {
 public:
  explicit Layout(std::filesystem::path root_dir)
      : root_dir_(std::move(root_dir)) {}

  const std::filesystem::path& root_dir() const noexcept { return root_dir_; }
  std::filesystem::path manifest_path() const;
  std::filesystem::path wal_dir() const;
  std::filesystem::path collections_dir() const;
  std::filesystem::path db_dir(std::string_view db_name) const;
  std::filesystem::path collection_dir(
      std::string_view db_name, std::string_view coll_name) const;
  std::filesystem::path collection_log_path(
      std::string_view db_name, std::string_view coll_name) const;
  std::filesystem::path collection_state_path(
      std::string_view db_name, std::string_view coll_name) const;

 private:
  std::filesystem::path root_dir_;
};

}  // namespace savannah::storage::canopy
