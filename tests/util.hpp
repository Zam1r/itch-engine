#pragma once
#include <cstdint>
#include <vector>
#include <string>
// Builders that emit ITCH 5.0 messages (big-endian) for tests.
struct W {
  std::vector<uint8_t> b;
  W& c(char x) { b.push_back(uint8_t(x)); return *this; }
  W& u16(uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); return *this; }
  W& u32(uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xff); return *this; }
  W& u48(uint64_t v) { for (int i = 5; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xff); return *this; }
  W& u64(uint64_t v) { for (int i = 7; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xff); return *this; }
  W& s(const std::string& x, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(i < x.size() ? x[i] : ' '); return *this; }
  W& hdr(char t, uint16_t loc, uint64_t ts = 0) { return c(t).u16(loc).u16(0).u48(ts); }
};
inline void frame(std::vector<uint8_t>& out, const W& m) { out.push_back(m.b.size() >> 8); out.push_back(m.b.size() & 0xff); out.insert(out.end(), m.b.begin(), m.b.end()); }
inline W add(uint16_t loc, uint64_t ref, char side, uint32_t sh, uint32_t px) { W w; w.hdr('A', loc).u64(ref).c(side).u32(sh).s("AAPL", 8).u32(px); return w; }
inline W exec(uint16_t loc, uint64_t ref, uint32_t sh) { W w; w.hdr('E', loc).u64(ref).u32(sh).u64(1); return w; }
inline W cancel(uint16_t loc, uint64_t ref, uint32_t sh) { W w; w.hdr('X', loc).u64(ref).u32(sh); return w; }
inline W del(uint16_t loc, uint64_t ref) { W w; w.hdr('D', loc).u64(ref); return w; }
inline W repl(uint16_t loc, uint64_t o, uint64_t n, uint32_t sh, uint32_t px) { W w; w.hdr('U', loc).u64(o).u64(n).u32(sh).u32(px); return w; }
