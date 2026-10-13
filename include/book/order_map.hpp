// Open-addressing (linear probing) hash map: order ref -> pool index.
// Fixed power-of-two capacity allocated once; backward-shift deletion so there
// are no tombstones and probe lengths stay short.
#pragma once
#include <cstdint>
#include <memory>
#include <cassert>

namespace book {
class OrderMap {
 public:
  static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
  explicit OrderMap(uint32_t log2cap) : mask_((1ull << log2cap) - 1), shift_(64 - log2cap),
      keys_(new uint64_t[mask_ + 1]), vals_(new uint32_t[mask_ + 1]) {
    for (uint64_t i = 0; i <= mask_; ++i) vals_[i] = kEmpty;
  }
  uint32_t find(uint64_t k) const {
    for (uint64_t i = slot(k);; i = (i + 1) & mask_) {
      if (vals_[i] == kEmpty) return kEmpty;
      if (keys_[i] == k) return vals_[i];
    }
  }
  void insert(uint64_t k, uint32_t v) {
    assert(size_ < mask_);  // caller sizes the table generously
    uint64_t i = slot(k);
    while (vals_[i] != kEmpty) { if (keys_[i] == k) { vals_[i] = v; return; } i = (i + 1) & mask_; }
    keys_[i] = k; vals_[i] = v; ++size_;
  }
  bool erase(uint64_t k) {
    uint64_t i = slot(k);
    while (true) {
      if (vals_[i] == kEmpty) return false;
      if (keys_[i] == k) break;
      i = (i + 1) & mask_;
    }
    // backward-shift deletion
    uint64_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (vals_[j] == kEmpty) break;
      uint64_t h = slot(keys_[j]);
      // move j to i if h is cyclically outside (i, j]
      if ((j > i && (h <= i || h > j)) || (j < i && (h <= i && h > j))) {
        keys_[i] = keys_[j]; vals_[i] = vals_[j]; i = j;
      }
    }
    vals_[i] = kEmpty; --size_;
    return true;
  }
  uint64_t size() const { return size_; }
  uint64_t capacity() const { return mask_ + 1; }
 private:
  uint64_t slot(uint64_t k) const { return (k * 0x9E3779B97F4A7C15ull) >> shift_; }
  uint64_t mask_; unsigned shift_;
  std::unique_ptr<uint64_t[]> keys_; std::unique_ptr<uint32_t[]> vals_;
  uint64_t size_ = 0;
};
}
