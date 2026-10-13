// Cache-friendly L3 book for all symbols.
//  * Orders live in a preallocated pool (struct-of-32-bytes, free list).
//  * Order ref -> pool index via OrderMap (open addressing, no allocation).
//  * Each symbol side is a flat vector of price levels sorted so that the BEST
//    level is at the back: most updates touch the top of book, so a short
//    backwards linear scan finds the level, and inserting/removing near the
//    top moves very few elements.
//  * Orders within a level form an intrusive doubly linked FIFO (price-time).
// After warm-up (pool/map/level vectors reserved) the hot path performs no
// heap allocation unless a level vector exceeds its reserved capacity; the
// replay tool counts allocations to verify this.
#pragma once
#include "book/common.hpp"
#include "book/order_map.hpp"
#include <vector>
#include <algorithm>

namespace book {

class FastBook {
 public:
  static constexpr uint32_t kNil = 0xFFFFFFFFu;
  struct Order { uint64_t ref; uint32_t price, shares, prev, next; uint16_t locate; uint8_t side; };
  struct Level { uint32_t price, count; uint64_t qty; uint32_t head, tail; };
  struct SideBook { std::vector<Level> lv; };  // best at back
  struct SymBook { SideBook s[2]; };

  FastBook(uint32_t pool_cap = 1u << 22, uint32_t map_log2 = 23, uint32_t level_reserve = 512)
      : map_(map_log2), level_reserve_(level_reserve) {
    pool_.resize(pool_cap);
    for (uint32_t i = 0; i < pool_cap; ++i) pool_[i].next = i + 1 < pool_cap ? i + 1 : kNil;
    free_ = 0;
    syms_.resize(65536);
  }
  // Called on Stock Directory ('R'): warm-up reservation outside the hot path.
  void reserve_symbol(uint16_t loc) {
    for (auto& s : syms_[loc].s) s.lv.reserve(level_reserve_);
  }

  void add(uint16_t loc, uint64_t ref, Side side, uint32_t shares, uint32_t price) {
    uint32_t oi = alloc();
    Order& o = pool_[oi];
    o.ref = ref; o.price = price; o.shares = shares; o.locate = loc; o.side = side; o.next = kNil;
    map_.insert(ref, oi);
    Level& l = level_for_insert(syms_[loc].s[side].lv, side, price);
    o.prev = l.tail;
    if (l.tail != kNil) pool_[l.tail].next = oi; else l.head = oi;
    l.tail = oi; l.qty += shares; ++l.count;
  }
  // Reduce shares (execute or partial cancel). Removes the order at zero.
  void reduce(uint64_t ref, uint32_t shares) {
    uint32_t oi = map_.find(ref);
    if (oi == OrderMap::kEmpty) return;
    Order& o = pool_[oi];
    auto& lv = syms_[o.locate].s[o.side].lv;
    size_t li = find_level(lv, Side(o.side), o.price);
    uint32_t d = shares < o.shares ? shares : o.shares;
    o.shares -= d; lv[li].qty -= d;
    if (o.shares == 0) unlink(oi, lv, li);
  }
  void remove(uint64_t ref) {
    uint32_t oi = map_.find(ref);
    if (oi == OrderMap::kEmpty) return;
    Order& o = pool_[oi];
    auto& lv = syms_[o.locate].s[o.side].lv;
    size_t li = find_level(lv, Side(o.side), o.price);
    lv[li].qty -= o.shares;
    unlink(oi, lv, li);
  }
  // Replace: old order removed, new order (new ref) added at back of queue.
  void replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
    uint32_t oi = map_.find(old_ref);
    if (oi == OrderMap::kEmpty) return;
    uint16_t loc = pool_[oi].locate; Side s = Side(pool_[oi].side);
    remove(old_ref);
    add(loc, new_ref, s, shares, price);
  }

  SideSnapshot snapshot(uint16_t loc, Side s) const {
    SideSnapshot out; const auto& lv = syms_[loc].s[s].lv;
    for (auto it = lv.rbegin(); it != lv.rend(); ++it) out.push_back({it->price, it->qty, it->count});
    return out;
  }
  uint64_t live_orders() const { return map_.size(); }
  bool best(uint16_t loc, Side s, uint32_t& px, uint64_t& qty) const {
    const auto& lv = syms_[loc].s[s].lv; if (lv.empty()) return false;
    px = lv.back().price; qty = lv.back().qty; return true;
  }
  uint64_t pool_grows() const { return pool_grows_; }

 private:
  // "better" = closer to top of book
  static bool better(Side s, uint32_t a, uint32_t b) { return s == Bid ? a > b : a < b; }

  static size_t find_level(std::vector<Level>& lv, Side s, uint32_t px) {
    size_t n = lv.size();
    for (size_t k = 0; k < 8 && k < n; ++k) if (lv[n - 1 - k].price == px) return n - 1 - k;
    // vector ascending in "worseness" from back: front is worst.
    auto it = std::lower_bound(lv.begin(), lv.end(), px,
        [s](const Level& l, uint32_t p) { return better(s, p, l.price); });
    return size_t(it - lv.begin());
  }
  Level& level_for_insert(std::vector<Level>& lv, Side s, uint32_t px) {
    size_t n = lv.size();
    // fast path: scan from the top of book
    size_t k = 0;
    for (; k < 8 && k < n; ++k) {
      Level& l = lv[n - 1 - k];
      if (l.price == px) return l;
      if (better(s, l.price, px)) continue;  // still above our price
      break;
    }
    size_t pos;
    if (k < 8 || k == n) pos = n - k;
    else {
      auto it = std::lower_bound(lv.begin(), lv.end(), px,
          [s](const Level& l, uint32_t p) { return better(s, p, l.price); });
      if (it != lv.end() && it->price == px) return *it;
      pos = size_t(it - lv.begin());
    }
    return *lv.insert(lv.begin() + pos, Level{px, 0, 0, kNil, kNil});
  }
  void unlink(uint32_t oi, std::vector<Level>& lv, size_t li) {
    Order& o = pool_[oi]; Level& l = lv[li];
    if (o.prev != kNil) pool_[o.prev].next = o.next; else l.head = o.next;
    if (o.next != kNil) pool_[o.next].prev = o.prev; else l.tail = o.prev;
    if (--l.count == 0) lv.erase(lv.begin() + li);
    map_.erase(o.ref);
    o.next = free_; free_ = oi;
  }
  uint32_t alloc() {
    if (free_ == kNil) {  // pool exhausted: grow (allocation; counted)
      uint32_t old = uint32_t(pool_.size());
      pool_.resize(old * 2); ++pool_grows_;
      for (uint32_t i = old; i < old * 2; ++i) pool_[i].next = i + 1 < old * 2 ? i + 1 : kNil;
      free_ = old;
    }
    uint32_t i = free_; free_ = pool_[i].next; return i;
  }

  std::vector<Order> pool_;
  uint32_t free_;
  OrderMap map_;
  std::vector<SymBook> syms_;
  uint32_t level_reserve_;
  uint64_t pool_grows_ = 0;
};
}
