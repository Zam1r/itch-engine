#include <catch2/catch_test_macros.hpp>
#include "itch/decoder.hpp"
#include "util.hpp"

struct Rec : itch::NullHandler {
  std::string seq; uint64_t ref = 0; uint32_t px = 0, sh = 0; uint64_t ts = 0; char side = 0;
  void on_add(itch::AddOrder m) { seq += 'A'; ref = m.ref(); px = m.price(); sh = m.shares(); ts = m.timestamp_ns(); side = m.side(); }
  void on_add_mpid(itch::AddOrderMPID m) { seq += 'F'; ref = m.ref(); }
  void on_executed(itch::OrderExecuted m) { seq += 'E'; sh = m.shares(); }
  void on_cancel(itch::OrderCancel) { seq += 'X'; }
  void on_delete(itch::OrderDelete) { seq += 'D'; }
  void on_replace(itch::OrderReplace m) { seq += 'U'; ref = m.new_ref(); px = m.price(); }
  void on_other(itch::Header m) { seq += m.type(); }
  void on_unknown(itch::Header) { seq += '?'; }
};

TEST_CASE("message lengths match ITCH 5.0 spec") {
  CHECK(itch::message_length('A') == 36); CHECK(itch::message_length('F') == 40);
  CHECK(itch::message_length('E') == 31); CHECK(itch::message_length('C') == 36);
  CHECK(itch::message_length('X') == 23); CHECK(itch::message_length('D') == 19);
  CHECK(itch::message_length('U') == 35); CHECK(itch::message_length('P') == 44);
  CHECK(itch::message_length('R') == 39); CHECK(itch::message_length('I') == 50);
  CHECK(itch::message_length('Z') == 0);
}

TEST_CASE("decodes big-endian fields of an add order") {
  W w; w.hdr('A', 7, 0x0000123456789AULL).u64(0x0102030405060708ULL).c('S').u32(300).s("MSFT", 8).u32(1234500);
  REQUIRE(w.b.size() == 36);
  Rec r; REQUIRE(itch::dispatch(w.b.data(), r));
  CHECK(r.seq == "A"); CHECK(r.ref == 0x0102030405060708ULL); CHECK(r.side == 'S');
  CHECK(r.sh == 300); CHECK(r.px == 1234500); CHECK(r.ts == 0x0000123456789AULL);
  CHECK(itch::AddOrder{{w.b.data()}}.stock() == "MSFT    ");
}

TEST_CASE("buffer iteration handles framing and truncated tail") {
  std::vector<uint8_t> buf;
  frame(buf, add(1, 10, 'B', 100, 5000)); frame(buf, exec(1, 10, 40)); frame(buf, cancel(1, 10, 10));
  frame(buf, repl(1, 10, 11, 50, 5100)); frame(buf, del(1, 11));
  W y; y.hdr('Y', 1).c('N'); frame(buf, y);
  W bad; bad.c('Z').u16(0); frame(buf, bad);
  size_t full = buf.size();
  frame(buf, add(1, 12, 'B', 1, 1)); buf.resize(buf.size() - 5);   // truncated
  Rec r; size_t n = itch::decode_buffer(buf.data(), buf.size(), r);
  CHECK(n == 7); CHECK(r.seq == "AEXUDY?"); CHECK(r.ref == 11);
  (void)full;
}
