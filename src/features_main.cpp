// itch_features: replays a NASDAQ ITCH 5.0 file through FastBook and writes a
// top-of-book event stream for selected symbols, for the order-book imbalance
// study (scripts/imbalance_study.py, bench/IMBALANCE_STUDY.md).
//
//   itch_features <file.itch> --count <out.csv>
//       per-symbol count of book messages (A/F/E/C/X/D/U) in regular hours
//   itch_features <file.itch> --symbols AAPL,MSFT,... --out <dir>
//       writes <dir>/<SYM>.bin: one fixed 64-byte record every time a tracked symbol's best bid/ask
//       price or size changes during regular hours (09:30-16:00 ET)
//
// Record layout (little-endian, see scripts/imbalance_study.py):
//   u64 ts_ns (since midnight ET) | u16 sym index | u16 pad | u32 pad
//   u32 bid_px | u32 ask_px (1e-4 $) | u32 bid_qty[5] | u32 ask_qty[5]
//   (levels beyond the book depth are 0); total 8+2+2+4+4+4+20+20 = 64 bytes.
// Books are built from the first message of the file (pre-market included);
// only emission is restricted to regular hours.
#include "book/book_handler.hpp"
#include "book/fast_book.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static constexpr uint64_t kOpen = 34200ull * 1000000000ull;   // 09:30:00 ET
static constexpr uint64_t kClose = 57600ull * 1000000000ull;  // 16:00:00 ET

struct Rec {
  uint64_t ts; uint16_t sym, pad0; uint32_t pad1;
  uint32_t bid_px, ask_px;
  uint32_t bid_qty[5], ask_qty[5];
};
static_assert(sizeof(Rec) == 64);

static std::string trim(std::string_view s) { while (!s.empty() && s.back() == ' ') s.remove_suffix(1); return std::string(s); }

struct Common : book::BookHandler<book::FastBook> {
  using Base = book::BookHandler<book::FastBook>;
  using Base::on_add;
  std::array<std::string, 65536> nm;
  explicit Common(book::FastBook& b) : Base(b) { names = &nm; }
};

// Counting pass.
struct CountH : Common {
  using Common::Common;
  std::vector<uint64_t> cnt = std::vector<uint64_t>(65536, 0);
  void hit(const itch::Header& h) { uint64_t t = h.timestamp_ns(); if (t >= kOpen && t < kClose) ++cnt[h.locate()]; }
  void on_add(itch::AddOrder m) { hit(m); }
  void on_add_mpid(itch::AddOrderMPID m) { hit(m); }
  void on_executed(itch::OrderExecuted m) { hit(m); }
  void on_executed_price(itch::OrderExecutedWithPrice m) { hit(m); }
  void on_cancel(itch::OrderCancel m) { hit(m); }
  void on_delete(itch::OrderDelete m) { hit(m); }
  void on_replace(itch::OrderReplace m) { hit(m); }
};

// Emission pass: book update, then emit if the tracked symbol's L1 changed.
struct EmitH : Common {
  std::vector<int> idx = std::vector<int>(65536, -1);   // locate -> tracked index
  std::vector<std::string> want;
  struct L1 { uint32_t bp = 0, ap = 0; uint64_t bq = 0, aq = 0; };
  std::vector<L1> last;
  std::vector<std::vector<Rec>> buf; std::vector<FILE*> out; uint64_t emitted = 0;
  std::vector<uint64_t> per_sym;
  EmitH(book::FastBook& b, std::vector<std::string> w, const std::string& dir) : Common(b), want(std::move(w)), last(want.size()), buf(want.size()), per_sym(want.size(), 0) {
    for (auto& s : want) {
      std::string path = dir + "/" + s + ".bin";
      FILE* f = std::fopen(path.c_str(), "wb"); if (!f) { std::perror(path.c_str()); std::exit(1); }
      out.push_back(f);
    }
    for (auto& v : buf) v.reserve(1 << 14);
  }
  ~EmitH() { flush(); for (FILE* f : out) std::fclose(f); }
  void flush() { for (size_t i = 0; i < buf.size(); ++i) flush(i); }
  void flush(size_t i) { if (!buf[i].empty()) std::fwrite(buf[i].data(), sizeof(Rec), buf[i].size(), out[i]); buf[i].clear(); }
  void on_stock_directory(itch::StockDirectory m) {
    Common::on_stock_directory(m);
    std::string s = trim(m.stock());
    for (size_t i = 0; i < want.size(); ++i) if (want[i] == s) idx[m.locate()] = int(i);
  }
  void after(uint16_t loc, uint64_t ts) {
    int i = idx[loc]; if (i < 0) return;
    L1 cur; b.best(loc, book::Bid, cur.bp, cur.bq); b.best(loc, book::Ask, cur.ap, cur.aq);
    L1& p = last[i];
    if (cur.bp == p.bp && cur.ap == p.ap && cur.bq == p.bq && cur.aq == p.aq) return;
    p = cur;
    if (ts < kOpen || ts >= kClose) return;
    Rec r{}; r.ts = ts; r.sym = uint16_t(i); r.bid_px = cur.bp; r.ask_px = cur.ap;
    for (int k = 0; k < 5; ++k) {
      uint32_t px; uint64_t q;
      r.bid_qty[k] = b.level(loc, book::Bid, k, px, q) ? uint32_t(q) : 0;
      r.ask_qty[k] = b.level(loc, book::Ask, k, px, q) ? uint32_t(q) : 0;
    }
    buf[i].push_back(r); ++emitted; ++per_sym[i];
    if (buf[i].size() == buf[i].capacity()) flush(i);
  }
  // Execute/cancel/delete/replace carry the locate of the order's symbol.
  void on_add(itch::AddOrder m) { Common::on_add(m); after(m.locate(), m.timestamp_ns()); }
  void on_add_mpid(itch::AddOrderMPID m) { on_add(m); }
  void on_executed(itch::OrderExecuted m) { Common::on_executed(m); after(m.locate(), m.timestamp_ns()); }
  void on_executed_price(itch::OrderExecutedWithPrice m) { Common::on_executed_price(m); after(m.locate(), m.timestamp_ns()); }
  void on_cancel(itch::OrderCancel m) { Common::on_cancel(m); after(m.locate(), m.timestamp_ns()); }
  void on_delete(itch::OrderDelete m) { Common::on_delete(m); after(m.locate(), m.timestamp_ns()); }
  void on_replace(itch::OrderReplace m) { Common::on_replace(m); after(m.locate(), m.timestamp_ns()); }
};

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: %s file.itch --count out.csv | --symbols A,B,... --out out.bin\n", argv[0]); return 2; }
  const char* countp = nullptr; const char* outp = nullptr; std::string syms;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--count")) countp = argv[++i];
    else if (!std::strcmp(argv[i], "--symbols")) syms = argv[++i];
    else if (!std::strcmp(argv[i], "--out")) outp = argv[++i];
  }
  int fd = open(argv[1], O_RDONLY); if (fd < 0) { std::perror(argv[1]); return 1; }
  struct stat st; fstat(fd, &st);
  auto* p = static_cast<const uint8_t*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
  if (p == MAP_FAILED) { std::perror("mmap"); return 1; }
  madvise(const_cast<uint8_t*>(p), st.st_size, MADV_SEQUENTIAL);
  // Full day: up to several million live orders, so size the map at 2^25.
  auto bk = std::make_unique<book::FastBook>(1u << 23, 25, 512);
  if (countp) {
    auto h = std::make_unique<CountH>(*bk);
    size_t n = itch::decode_buffer(p, st.st_size, *h);
    FILE* f = std::fopen(countp, "w"); std::fprintf(f, "symbol,rth_book_msgs\n");
    for (int l = 0; l < 65536; ++l) if (h->cnt[l]) std::fprintf(f, "%s,%llu\n", trim(h->nm[l]).c_str(), (unsigned long long)h->cnt[l]);
    std::fclose(f);
    std::fprintf(stderr, "messages=%zu bytes=%lld live_orders_end=%llu\n", n, (long long)st.st_size, (unsigned long long)bk->live_orders());
    return 0;
  }
  std::vector<std::string> want; { std::stringstream ss(syms); std::string s; while (std::getline(ss, s, ',')) if (!s.empty()) want.push_back(s); }
  size_t n;
  {
    auto h = std::make_unique<EmitH>(*bk, want, std::string(outp));
    n = itch::decode_buffer(p, st.st_size, *h);
    h->flush();
    std::fprintf(stderr, "messages=%zu emitted=%llu\n", n, (unsigned long long)h->emitted);
    for (size_t i = 0; i < want.size(); ++i) std::fprintf(stderr, "  %s %llu\n", want[i].c_str(), (unsigned long long)h->per_sym[i]);
  }
  return 0;
}
