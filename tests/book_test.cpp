#include <catch2/catch_test_macros.hpp>
#include "book/fast_book.hpp"
#include "book/map_book.hpp"
#include "book/book_handler.hpp"
#include "util.hpp"
#include <random>
#include <memory>

using namespace book;

TEST_CASE("fast book basic add/execute/cancel/delete/replace") {
  auto b = std::make_unique<FastBook>(1024, 12, 8);
  b->add(1, 1, Bid, 100, 1000); b->add(1, 2, Bid, 50, 1000); b->add(1, 3, Bid, 10, 990);
  b->add(1, 4, Ask, 70, 1010);
  auto s = b->snapshot(1, Bid);
  REQUIRE(s.size() == 2); CHECK(s[0] == LevelView{1000, 150, 2}); CHECK(s[1] == LevelView{990, 10, 1});
  b->reduce(1, 30);  CHECK(b->snapshot(1, Bid)[0] == LevelView{1000, 120, 2});
  b->reduce(1, 70);  CHECK(b->snapshot(1, Bid)[0] == LevelView{1000, 50, 1});
  b->remove(2);      CHECK(b->snapshot(1, Bid)[0] == LevelView{990, 10, 1});
  b->replace(4, 5, 20, 1005); CHECK(b->snapshot(1, Ask) == SideSnapshot{{1005, 20, 1}});
  CHECK(b->live_orders() == 2);
  b->remove(999);  // unknown ref ignored
}

template <class B> static void apply(B& b, int op, uint16_t loc, uint64_t ref, uint64_t nref, Side s, uint32_t sh, uint32_t px) {
  switch (op) { case 0: b.add(loc, ref, s, sh, px); break; case 1: b.reduce(ref, sh); break;
                case 2: b.remove(ref); break; default: b.replace(ref, nref, sh, px); }
}

TEST_CASE("fast book is equivalent to std::map baseline on random flow") {
  // Small pool forces pool growth; small level reserve forces level growth.
  auto f = std::make_unique<FastBook>(64, 16, 2); auto m = std::make_unique<MapBook>();
  std::mt19937_64 rng(7); std::vector<std::pair<uint64_t, uint16_t>> live; uint64_t next = 1;
  for (int i = 0; i < 300000; ++i) {
    int op = live.empty() ? 0 : int(rng() % 4);
    if (live.size() < 1500 && rng() % 3 == 0) op = 0;
    if (live.size() > 3000 && op == 0) op = 2;
    uint16_t loc = uint16_t(rng() % 5); Side s = Side(rng() % 2);
    uint32_t px = 10000 + uint32_t(rng() % 200) * (s == Bid ? 1 : 1) + (s == Ask ? 150 : 0);
    uint32_t sh = 1 + uint32_t(rng() % 500);
    if (op == 0) { uint64_t r = next++; apply(*f, 0, loc, r, 0, s, sh, px); apply(*m, 0, loc, r, 0, s, sh, px); live.push_back({r, loc}); }
    else {
      size_t k = rng() % live.size(); uint64_t r = live[k].first; uint64_t nr = next++;
      apply(*f, op, 0, r, nr, s, sh, px); apply(*m, op, 0, r, nr, s, sh, px);
      if (op == 3) live[k].first = nr;
      if (op == 2 || (op == 1 && f->live_orders() < m->live_orders() + 1 && false)) { live[k] = live.back(); live.pop_back(); }
    }
    if (i % 997 == 0) for (uint16_t l = 0; l < 5; ++l) for (int sd = 0; sd < 2; ++sd)
      REQUIRE(f->snapshot(l, Side(sd)) == m->snapshot(l, Side(sd)));
  }
  CHECK(f->live_orders() == m->live_orders());
  CHECK(f->pool_grows() > 0);
}

TEST_CASE("handler wires ITCH messages into the book") {
  std::vector<uint8_t> buf;
  frame(buf, add(3, 1, 'B', 100, 5000)); frame(buf, add(3, 2, 'S', 100, 5100));
  frame(buf, exec(3, 1, 40)); frame(buf, repl(3, 2, 9, 30, 5050));
  auto b = std::make_unique<FastBook>(1024, 12, 8); BookHandler<FastBook> h(*b);
  itch::decode_buffer(buf.data(), buf.size(), h);
  CHECK(b->snapshot(3, Bid) == SideSnapshot{{5000, 60, 1}});
  CHECK(b->snapshot(3, Ask) == SideSnapshot{{5050, 30, 1}});
}
