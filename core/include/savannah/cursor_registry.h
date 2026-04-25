#pragma once

// Slice D cursor registry — keeps `Iterator`s alive between getMore calls.
// Connection-lifecycle GC isn't wired in yet; mongodb@6 calls killCursors
// when iteration ends early, and natural exhaustion erases the entry, so
// this is acceptable for Phase 0.2. Revisit in 0.6 with session binding.

#include "savannah/jungle/v1/storage.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace savannah {

class CursorRegistry {
 public:
  // Hands ownership to the registry; returns a fresh non-zero id.
  std::int64_t register_cursor(
      std::unique_ptr<jungle::storage::v1::Iterator> iter, std::string ns);

  // Non-owning peek. Returns nullptr if no such id, or if `expected_ns`
  // doesn't match the registered ns. Keeps ownership in the registry.
  jungle::storage::v1::Iterator* borrow(std::int64_t id,
                                        const std::string& expected_ns);

  // Returns true if an entry was found and removed.
  bool erase(std::int64_t id);

 private:
  struct Entry {
    std::unique_ptr<jungle::storage::v1::Iterator> iter;
    std::string ns;  // "<db>.<collection>" — validated on getMore.
  };

  std::mutex mu_;
  std::unordered_map<std::int64_t, Entry> entries_;
  std::int64_t next_id_{1};  // 0 means "exhausted" on the wire — never assign.
};

}  // namespace savannah
