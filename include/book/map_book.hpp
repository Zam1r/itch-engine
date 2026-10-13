// Naive baseline: std::map price levels holding std::list FIFOs, plus an
// std::unordered_map from order ref to (symbol, side, price, list iterator).
// Every add allocates (list node + hash node, sometimes a map node).
#pragma once
#include "book/common.hpp"
#include <map>
#include <list>
#include <unordered_map>
#include <functional>
#include <vector>

namespace book {
class MapBook {
 public:
  struct Level { uint64_t qty = 0; std::list<uint64_t> orders; };
  struct OrderInfo { uint16_t loc; Side side; uint32_t price, shares; std::list<uint64_t>::iterator it; };
  struct SymBook { std::map<uint32_t, Level, std::greater<uint32_t>> bids; std::map<uint32_t, Level> asks; };

  MapBook() : syms_(65536) {}
  void reserve_symbol(uint16_t) {}

  void add(uint16_t loc, uint64_t ref, Side side, uint32_t shares, uint32_t price) {
    auto& l = side == Bid ? syms_[loc].bids[price] : syms_[loc].asks[price];
    l.orders.push_back(ref); l.qty += shares;
    orders_[ref] = OrderInfo{loc, side, price, shares, std::prev(l.orders.end())};
  }
  void reduce(uint64_t ref, uint32_t shares) {
    auto it = orders_.find(ref); if (it == orders_.end()) return;
    auto& o = it->second; uint32_t d = shares < o.shares ? shares : o.shares;
    o.shares -= d;
    if (o.side == Bid) dec(syms_[o.loc].bids, it, d); else dec(syms_[o.loc].asks, it, d);
  }
  void remove(uint64_t ref) {
    auto it = orders_.find(ref); if (it == orders_.end()) return;
    auto& o = it->second; uint32_t d = o.shares; o.shares = 0;
    if (o.side == Bid) dec(syms_[o.loc].bids, it, d); else dec(syms_[o.loc].asks, it, d);
  }
  void replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
    auto it = orders_.find(old_ref); if (it == orders_.end()) return;
    uint16_t loc = it->second.loc; Side s = it->second.side;
    remove(old_ref); add(loc, new_ref, s, shares, price);
  }
  SideSnapshot snapshot(uint16_t loc, Side s) const {
    SideSnapshot out;
    if (s == Bid) for (auto& [p, l] : syms_[loc].bids) out.push_back({p, l.qty, uint32_t(l.orders.size())});
    else for (auto& [p, l] : syms_[loc].asks) out.push_back({p, l.qty, uint32_t(l.orders.size())});
    return out;
  }
  uint64_t live_orders() const { return orders_.size(); }
 private:
  template <class M>
  void dec(M& m, std::unordered_map<uint64_t, OrderInfo>::iterator it, uint32_t d) {
    auto& o = it->second; auto lit = m.find(o.price); lit->second.qty -= d;
    if (o.shares == 0) {
      lit->second.orders.erase(o.it);
      if (lit->second.orders.empty()) m.erase(lit);
      orders_.erase(it);
    }
  }
  std::vector<SymBook> syms_;
  std::unordered_map<uint64_t, OrderInfo> orders_;
};
}
