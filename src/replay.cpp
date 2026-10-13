// itch_replay: replays a NASDAQ ITCH 5.0 binary file from memory into an
// order book and reports throughput and/or per-message latency.
//   itch_replay <file> [--book fast|map] [--mode throughput|latency]
//               [--hist out.csv] [--json]
#include "alloc_counter.hpp"
#include "book/book_handler.hpp"
#include "book/fast_book.hpp"
#include "book/map_book.hpp"
#include <x86intrin.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <set>

using clk = std::chrono::steady_clock;

static std::vector<uint8_t> load(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { std::perror(path); std::exit(1); }
  size_t n = f.tellg(); f.seekg(0);
  std::vector<uint8_t> buf(n); f.read(reinterpret_cast<char*>(buf.data()), n);
  return buf;
}
static inline uint64_t tsc_begin() { _mm_lfence(); return __rdtsc(); }
static inline uint64_t tsc_end() { unsigned a; uint64_t t = __rdtscp(&a); _mm_lfence(); return t; }

static double tsc_per_ns() {
  auto t0 = clk::now(); uint64_t c0 = __rdtsc();
  while (clk::now() - t0 < std::chrono::milliseconds(300)) {}
  auto t1 = clk::now(); uint64_t c1 = __rdtsc();
  return double(c1 - c0) / std::chrono::duration<double, std::nano>(t1 - t0).count();
}

// index of message offsets (built before timing so framing isn't measured twice)
static std::vector<uint32_t> index_msgs(const std::vector<uint8_t>& b) {
  std::vector<uint32_t> idx; size_t off = 0;
  while (off + 2 <= b.size()) { uint16_t l = itch::be16(&b[off]); if (off + 2 + l > b.size()) break; idx.push_back(uint32_t(off + 2)); off += 2 + l; }
  return idx;
}

template <class Book>
static int run(const std::vector<uint8_t>& buf, const std::string& mode, const char* hist_path, bool json, const char* bookname) {
  auto book = std::make_unique<Book>();
  book::BookHandler<Book> h(*book);
  const uint8_t* base = buf.data();
  std::set<uint16_t> locs;
  if (mode == "throughput") {
    uint64_t a0 = g_allocs.load();
    auto t0 = clk::now();
    size_t n = itch::decode_buffer(base, buf.size(), h);
    auto t1 = clk::now();
    uint64_t allocs = g_allocs.load() - a0;
    double s = std::chrono::duration<double>(t1 - t0).count();
    std::printf(json ? "{\"book\":\"%s\",\"mode\":\"throughput\",\"messages\":%zu,\"seconds\":%.6f,\"msgs_per_sec\":%.0f,\"heap_allocs\":%llu,\"live_orders_end\":%llu}\n"
                     : "book=%s mode=throughput messages=%zu seconds=%.6f msgs_per_sec=%.0f heap_allocs=%llu live_orders_end=%llu\n",
        bookname, n, s, n / s, (unsigned long long)allocs, (unsigned long long)book->live_orders());
    return 0;
  }
  // latency mode: time each message (decode + book update) with rdtsc/rdtscp
  auto idx = index_msgs(buf);
  double tpn = tsc_per_ns();
  // timer overhead: median of empty measured regions
  std::vector<uint64_t> ov(100000);
  for (auto& x : ov) { uint64_t a = tsc_begin(); uint64_t b = tsc_end(); x = b - a; }
  std::nth_element(ov.begin(), ov.begin() + ov.size() / 2, ov.end());
  uint64_t overhead = ov[ov.size() / 2];
  constexpr size_t kBins = 1'000'000; // 1 ns bins up to 1 ms
  std::vector<uint64_t> hist(kBins + 1, 0);
  uint64_t maxc = 0;
  // warm-up: first 1% of messages processed untimed? No: we time all messages
  // (the book state must evolve identically); the first 100k are excluded from
  // the statistics as warm-up.
  const size_t warm = 100000;
  uint64_t a0 = 0;
  for (size_t i = 0; i < idx.size(); ++i) {
    if (i == warm) a0 = g_allocs.load();
    const uint8_t* p = base + idx[i];
    uint64_t t0 = tsc_begin();
    itch::dispatch(p, h);
    uint64_t t1 = tsc_end();
    if (i < warm) continue;
    uint64_t c = t1 - t0; c = c > overhead ? c - overhead : 0;
    if (c > maxc) maxc = c;
    size_t ns = size_t(c / tpn);
    ++hist[ns < kBins ? ns : kBins];
  }
  uint64_t allocs = g_allocs.load() - a0;
  uint64_t total = 0; for (auto v : hist) total += v;
  auto pct = [&](double q) { uint64_t target = uint64_t(q * total); uint64_t acc = 0;
    for (size_t i = 0; i <= kBins; ++i) { acc += hist[i]; if (acc > target) return double(i); } return double(kBins); };
  double mean = 0; for (size_t i = 0; i < kBins; ++i) mean += double(i) * hist[i]; mean /= total;
  if (hist_path) { std::FILE* f = std::fopen(hist_path, "w"); std::fprintf(f, "ns,count\n");
    for (size_t i = 0; i < kBins; ++i) if (hist[i]) std::fprintf(f, "%zu,%llu\n", i, (unsigned long long)hist[i]);
    if (hist[kBins]) { std::fprintf(f, ">=%zu,%llu\n", kBins, (unsigned long long)hist[kBins]); }
    std::fclose(f); }
  std::printf(json ? "{\"book\":\"%s\",\"mode\":\"latency\",\"messages_timed\":%llu,\"warmup_excluded\":%zu,\"tsc_per_ns\":%.4f,\"timer_overhead_cycles\":%llu,\"p50_ns\":%.0f,\"p90_ns\":%.0f,\"p99_ns\":%.0f,\"p999_ns\":%.0f,\"max_ns\":%.0f,\"mean_ns\":%.2f,\"heap_allocs_after_warmup\":%llu}\n"
                   : "book=%s mode=latency timed=%llu warmup=%zu tsc_per_ns=%.4f overhead_cyc=%llu p50=%.0f p90=%.0f p99=%.0f p99.9=%.0f max=%.0f mean=%.2f allocs=%llu\n",
      bookname, (unsigned long long)total, warm, tpn, (unsigned long long)overhead, pct(0.5), pct(0.9), pct(0.99), pct(0.999), double(maxc) / tpn, mean, (unsigned long long)allocs);
  return 0;
}

// stats mode: message-type counts, distinct locates, time span
struct Stats : itch::NullHandler {
  uint64_t c[256] = {}; std::set<uint16_t> locs, book_locs; uint64_t first_ts = 0, last_ts = 0; bool seen = false;
  void tick(itch::Header h) { c[uint8_t(h.type())]++; locs.insert(h.locate()); if (!seen) { first_ts = h.timestamp_ns(); seen = true; } last_ts = h.timestamp_ns(); }
};
struct StatsH : Stats {
  void on_system_event(itch::SystemEvent m) { tick(m); } void on_stock_directory(itch::StockDirectory m) { tick(m); }
  void on_trading_action(itch::StockTradingAction m) { tick(m); }
  void on_add(itch::AddOrder m) { tick(m); book_locs.insert(m.locate()); } void on_add_mpid(itch::AddOrderMPID m) { tick(m); book_locs.insert(m.locate()); }
  void on_executed(itch::OrderExecuted m) { tick(m); } void on_executed_price(itch::OrderExecutedWithPrice m) { tick(m); }
  void on_cancel(itch::OrderCancel m) { tick(m); } void on_delete(itch::OrderDelete m) { tick(m); } void on_replace(itch::OrderReplace m) { tick(m); }
  void on_trade(itch::Trade m) { tick(m); } void on_cross_trade(itch::CrossTrade m) { tick(m); } void on_broken_trade(itch::BrokenTrade m) { tick(m); }
  void on_noii(itch::Noii m) { tick(m); } void on_other(itch::Header m) { tick(m); } void on_unknown(itch::Header m) { tick(m); }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s file [--book fast|map] [--mode throughput|latency|stats] [--hist f.csv] [--json]\n", argv[0]); return 2; }
  std::string bk = "fast", mode = "throughput"; const char* hist = nullptr; bool json = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--book")) bk = argv[++i];
    else if (!std::strcmp(argv[i], "--mode")) mode = argv[++i];
    else if (!std::strcmp(argv[i], "--hist")) hist = argv[++i];
    else if (!std::strcmp(argv[i], "--json")) json = true;
  }
  auto buf = load(argv[1]);
  if (mode == "stats") {
    auto s = std::make_unique<StatsH>(); size_t n = itch::decode_buffer(buf.data(), buf.size(), *s);
    std::printf("{\"messages\":%zu,\"bytes\":%zu,\"distinct_locates\":%zu,\"locates_with_orders\":%zu,\"first_ts_ns\":%llu,\"last_ts_ns\":%llu,\"type_counts\":{",
      n, buf.size(), s->locs.size(), s->book_locs.size(), (unsigned long long)s->first_ts, (unsigned long long)s->last_ts);
    bool first = true; for (int t = 0; t < 256; ++t) if (s->c[t]) { std::printf("%s\"%c\":%llu", first ? "" : ",", t, (unsigned long long)s->c[t]); first = false; }
    std::printf("}}\n"); return 0;
  }
  if (bk == "fast") return run<book::FastBook>(buf, mode, hist, json, "fast");
  return run<book::MapBook>(buf, mode, hist, json, "map");
}
