#include <catch2/catch_test_macros.hpp>
#include "book/order_map.hpp"
#include <unordered_map>
#include <random>

TEST_CASE("OrderMap matches unordered_map under random insert/erase") {
  book::OrderMap m(12); std::unordered_map<uint64_t, uint32_t> ref;
  std::mt19937_64 rng(42);
  for (int i = 0; i < 200000; ++i) {
    uint64_t k = rng() % 3000; int op = rng() % 3;
    if (op == 0 && ref.size() < 3000) { m.insert(k, uint32_t(i)); ref[k] = uint32_t(i); }
    else if (op == 1) { CHECK(m.erase(k) == (ref.erase(k) == 1)); }
    else { auto it = ref.find(k); uint32_t v = m.find(k); if (it == ref.end()) REQUIRE(v == book::OrderMap::kEmpty); else REQUIRE(v == it->second); }
  }
  CHECK(m.size() == ref.size());
}
