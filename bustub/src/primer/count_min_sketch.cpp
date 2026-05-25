//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// count_min_sketch.cpp
//
// Identification: src/primer/count_min_sketch.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "primer/count_min_sketch.h"

#include <stdexcept>
#include <string>

namespace bustub {

/**
 * Constructor for the count-min sketch.
 *
 * @param width The width of the sketch matrix.
 * @param depth The depth of the sketch matrix.
 * @throws std::invalid_argument if width or depth are zero.
 */
template <typename KeyType>
CountMinSketch<KeyType>::CountMinSketch(uint32_t width, uint32_t depth) : width_(width), depth_(depth) {
  /** @TODO(student) Implement this function! */
  if (width == 0 || depth == 0) {
    throw std::invalid_argument("width and depth bad");
  }

  matrix_.reserve(depth);
  for (size_t i = 0; i < depth; i++) {
    auto row = std::make_unique<std::atomic<uint32_t>[]>(width);
    for (size_t j = 0; j < width; j++) {
      row[j].store(0, std::memory_order_relaxed);
    }
    matrix_.push_back(std::move(row));
  }
  /** @spring2026 PLEASE DO NOT MODIFY THE FOLLOWING */
  // Initialize seeded hash functions
  hash_functions_.reserve(depth_);
  for (size_t i = 0; i < depth_; i++) {
    hash_functions_.push_back(this->HashFunction(i));
  }
}

template <typename KeyType>
CountMinSketch<KeyType>::CountMinSketch(CountMinSketch &&other) noexcept
    : width_(other.width_),
      depth_(other.depth_),
      hash_functions_(std::move(other.hash_functions_)),
      matrix_(std::move(other.matrix_)) {
  /** @TODO(student) Implement this function! */
}

template <typename KeyType>
auto CountMinSketch<KeyType>::operator=(CountMinSketch &&other) noexcept -> CountMinSketch & {
  /** @TODO(student) Implement this function! */
  this->depth_ = other.depth_;
  this->width_ = other.width_;
  this->hash_functions_ = std::move(other.hash_functions_);
  this->matrix_ = std::move(other.matrix_);
  return *this;
}

template <typename KeyType>
void CountMinSketch<KeyType>::Insert(const KeyType &item) {
  /** @TODO(student) Implement this function! */
  for (size_t i = 0; i < depth_; i++) {
    size_t col = hash_functions_[i](item);
    matrix_[i][col].fetch_add(1, std::memory_order_relaxed);
  }
}

template <typename KeyType>
void CountMinSketch<KeyType>::Merge(const CountMinSketch<KeyType> &other) {
  if (width_ != other.width_ || depth_ != other.depth_) {
    throw std::invalid_argument("Incompatible CountMinSketch dimensions for merge.");
  }
  for (size_t i = 0; i < depth_; i++) {
    for (size_t j = 0; j < width_; j++) {
      uint32_t v = other.matrix_[i][j].load(std::memory_order_relaxed);
      matrix_[i][j].fetch_add(v, std::memory_order_relaxed);
    }
  }
}

template <typename KeyType>
auto CountMinSketch<KeyType>::Count(const KeyType &item) const -> uint32_t {
  uint32_t min_count = matrix_[0][hash_functions_[0](item)].load(std::memory_order_relaxed);
  for (size_t i = 1; i < depth_; i++) {
    size_t col = hash_functions_[i](item);
    uint32_t v = matrix_[i][col].load(std::memory_order_relaxed);
    min_count = std::min(min_count, v);
  }
  return min_count;
}

template <typename KeyType>
void CountMinSketch<KeyType>::Clear() {
  for (size_t i = 0; i < depth_; i++) {
    for (size_t j = 0; j < width_; j++) {
      matrix_[i][j].store(0, std::memory_order_relaxed);
    }
  }
}

template <typename KeyType>
auto CountMinSketch<KeyType>::TopK(uint16_t k, const std::vector<KeyType> &candidates)
    -> std::vector<std::pair<KeyType, uint32_t>> {
  /** @TODO(student) Implement this function! */
  std::vector<std::pair<KeyType, uint32_t>> result;
  result.reserve(candidates.size());

  for (const auto &item : candidates) {
    result.emplace_back(item, Count(item));
  }

  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

  if (result.size() > k) {
    result.resize(k);
  }
  return result;
}

// Explicit instantiations for all types used in tests
template class CountMinSketch<std::string>;
template class CountMinSketch<int64_t>;  // For int64_t tests
template class CountMinSketch<int>;      // This covers both int and int32_t
}  // namespace bustub
