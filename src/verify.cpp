// itch_verify: replays a file into FastBook and MapBook and checks that every
// symbol's full-depth book (price, qty, order count per level) is identical,
// at regular checkpoints and at the end.
#include "book/book_handler.hpp"
#include "book/fast_book.hpp"
#include "book/map_book.hpp"
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include <set>

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::ifstream f(argv[1], std::ios::binary | std::ios::ate); size_t n = f.tellg(); f.seekg(0);
  std::vector<uint8_t> buf(n); f.read((char*)buf.data(), n);
  auto fb = std::make_unique<book::FastBook>(); auto mb = std::make_unique<book::MapBook>();
  book::BookHandler<book::FastBook> hf(*fb); book::BookHandler<book::MapBook> hm(*mb);
  std::set<uint16_t> locs; size_t off = 0, msgs = 0, checks = 0, mism = 0;
  auto check = [&] {
    for (uint16_t l : locs) for (int s = 0; s < 2; ++s) {
      ++checks; if (fb->snapshot(l, book::Side(s)) != mb->snapshot(l, book::Side(s))) { if (++mism < 5) std::printf("MISMATCH locate=%u side=%d at msg %zu\n", l, s, msgs); }
    }
  };
  while (off + 2 <= n) {
    uint16_t ml = itch::be16(&buf[off]); if (off + 2 + ml > n) break;
    const uint8_t* p = &buf[off + 2];
    if (p[0] == 'A' || p[0] == 'F') locs.insert(itch::be16(p + 1));
    itch::dispatch(p, hf); itch::dispatch(p, hm);
    off += 2 + ml; ++msgs;
    if (msgs % 5'000'000 == 0) check();
  }
  check();
  std::printf("{\"messages\":%zu,\"symbols_checked\":%zu,\"side_comparisons\":%zu,\"mismatches\":%zu,\"live_orders_fast\":%llu,\"live_orders_map\":%llu}\n",
    msgs, locs.size(), checks, mism, (unsigned long long)fb->live_orders(), (unsigned long long)mb->live_orders());
  return mism ? 1 : 0;
}
