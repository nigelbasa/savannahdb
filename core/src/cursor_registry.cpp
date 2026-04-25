#include "savannah/cursor_registry.h"

#include <utility>

namespace savannah {

std::int64_t CursorRegistry::register_cursor(
    std::unique_ptr<jungle::storage::v1::Iterator> iter, std::string ns) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::int64_t id = next_id_++;
  entries_.emplace(id, Entry{std::move(iter), std::move(ns)});
  return id;
}

jungle::storage::v1::Iterator* CursorRegistry::borrow(
    std::int64_t id, const std::string& expected_ns) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(id);
  if (it == entries_.end()) return nullptr;
  if (it->second.ns != expected_ns) return nullptr;  // ns mismatch → 404.
  return it->second.iter.get();
}

bool CursorRegistry::erase(std::int64_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.erase(id) > 0;
}

}  // namespace savannah
