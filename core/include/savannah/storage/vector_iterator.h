#pragma once

// Helper iterator adapters used by the binding to compose query stages.

#include "savannah/bson/document.h"
#include "savannah/jungle/v1/storage.h"
#include "savannah/query/project.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace savannah::storage {

// Iterator that walks a pre-built vector of BsonViews. Used when find()
// materializes for sort/limit/skip. The BsonViews must point into memory
// that outlives the iterator (e.g. arena-owned slot bytes).
class VectorIterator final : public jungle::storage::v1::Iterator {
 public:
  explicit VectorIterator(std::vector<bson::BsonView> docs)
      : docs_(std::move(docs)) {}

  bool has_next() override { return index_ < docs_.size(); }
  bson::BsonView next() override { return docs_[index_++]; }

 private:
  std::vector<bson::BsonView> docs_;
  std::size_t index_{0};
};

// Iterator over owned BSON byte buffers. Used by aggregation where stage
// output no longer points at arena-owned slot bytes from the source store.
class OwnedBytesIterator final : public jungle::storage::v1::Iterator {
 public:
  explicit OwnedBytesIterator(std::vector<std::vector<std::uint8_t>> docs)
      : docs_(std::move(docs)) {}

  bool has_next() override { return index_ < docs_.size(); }

  bson::BsonView next() override {
    auto& bytes = docs_[index_++];
    return bson::BsonView(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()});
  }

 private:
  std::vector<std::vector<std::uint8_t>> docs_;
  std::size_t index_{0};
};

// Wraps another iterator and projects each yielded doc. Owns the projected
// bytes in a deque so addresses stay stable across pushes — the BsonView
// returned from next() points into the deque entry and is valid for the
// iterator's lifetime (the binding consumes it before the next next()
// call, but tying it to the iterator is the safer contract).
class ProjectingIterator final : public jungle::storage::v1::Iterator {
 public:
  ProjectingIterator(std::unique_ptr<jungle::storage::v1::Iterator> inner,
                     std::span<const std::uint8_t> spec)
      : inner_(std::move(inner)), spec_(spec.begin(), spec.end()) {}

  bool has_next() override { return inner_->has_next(); }

  bson::BsonView next() override {
    auto src = inner_->next();
    projected_.push_back(jungle::query::v1::project(
        src, std::span<const std::uint8_t>{spec_.data(), spec_.size()}));
    auto& bytes = projected_.back();
    return bson::BsonView(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()});
  }

 private:
  std::unique_ptr<jungle::storage::v1::Iterator> inner_;
  std::vector<std::uint8_t> spec_;
  std::deque<std::vector<std::uint8_t>> projected_;  // stable addresses
};

}  // namespace savannah::storage
