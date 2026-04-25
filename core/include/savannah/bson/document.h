#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace savannah::bson {

// Zero-copy view over a single BSON document. Owns no memory — the caller
// must keep the backing buffer alive for the view's lifetime.
class BsonView {
 public:
  BsonView() = default;
  explicit BsonView(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  const std::uint8_t* data() const noexcept { return bytes_.data(); }
  std::size_t size() const noexcept { return bytes_.size(); }
  std::span<const std::uint8_t> span() const noexcept { return bytes_; }

  // BSON documents start with a little-endian int32 total length (inclusive).
  std::int32_t declared_length() const noexcept {
    if (bytes_.size() < 4) return 0;
    std::int32_t len;
    std::memcpy(&len, bytes_.data(), 4);
    return len;
  }

  bool valid() const noexcept {
    const auto declared = declared_length();
    return declared >= 5 &&
           static_cast<std::size_t>(declared) == bytes_.size() &&
           bytes_[bytes_.size() - 1] == 0;  // trailing null byte
  }

 private:
  std::span<const std::uint8_t> bytes_{};
};

}  // namespace savannah::bson
