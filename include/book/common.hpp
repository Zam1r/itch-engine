#pragma once
#include <cstdint>
#include <vector>
#include <tuple>

namespace book {
enum Side : uint8_t { Bid = 0, Ask = 1 };
inline Side side_of(char c) { return c == 'B' ? Bid : Ask; }
struct LevelView { uint32_t price; uint64_t qty; uint32_t count;
  bool operator==(const LevelView&) const = default; };
// Snapshot of one side, best level first.
using SideSnapshot = std::vector<LevelView>;
}
