#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace savannah::storage {

// Bump allocator: linear allocation out of chunked slabs. Frees everything on
// destruction. Used as a cheap document-lifetime allocator for the in-memory
// backend. Not thread-safe.
class Arena {
 public:
  explicit Arena(std::size_t chunk_size = 64 * 1024) : chunk_size_(chunk_size) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  std::span<std::uint8_t> allocate(std::size_t bytes);
  std::span<std::uint8_t> copy(std::span<const std::uint8_t> src);

  std::size_t bytes_used() const noexcept { return used_total_; }

 private:
  struct Chunk {
    std::unique_ptr<std::uint8_t[]> data;
    std::size_t size{0};
    std::size_t used{0};
  };

  std::vector<Chunk> chunks_;
  std::size_t chunk_size_;
  std::size_t used_total_{0};
};

}  // namespace savannah::storage
