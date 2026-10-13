// NASDAQ TotalView-ITCH 5.0 message layout. Zero-copy: messages are read
// directly from the input buffer through big-endian field accessors; no
// structs are materialised and nothing is copied.
#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

namespace itch {

inline uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline uint64_t be64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
inline uint64_t be48(const uint8_t* p) { return (uint64_t(be16(p)) << 32) | be32(p + 2); }

// Payload length (excluding the 2-byte length prefix) for every ITCH 5.0 type.
constexpr uint16_t message_length(char t) {
  switch (t) {
    case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20;
    case 'L': return 26; case 'V': return 35; case 'W': return 12; case 'K': return 28;
    case 'J': return 35; case 'h': return 21; case 'A': return 36; case 'F': return 40;
    case 'E': return 31; case 'C': return 36; case 'X': return 23; case 'D': return 19;
    case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
    case 'I': return 50; case 'N': return 20; case 'O': return 48;
    default: return 0;
  }
}

// Common header: type(1) stock_locate(2) tracking_number(2) timestamp(6)
struct Header {
  const uint8_t* p;
  char type() const { return char(p[0]); }
  uint16_t locate() const { return be16(p + 1); }
  uint16_t tracking() const { return be16(p + 3); }
  uint64_t timestamp_ns() const { return be48(p + 5); }
};

struct SystemEvent : Header { char event_code() const { return char(p[11]); } };
struct StockDirectory : Header {
  std::string_view stock() const { return {reinterpret_cast<const char*>(p + 11), 8}; }
  char market_category() const { return char(p[19]); }
  uint32_t round_lot() const { return be32(p + 21); }
};
struct StockTradingAction : Header { char state() const { return char(p[19]); } };
struct AddOrder : Header {            // 'A' and 'F'
  uint64_t ref() const { return be64(p + 11); }
  char side() const { return char(p[19]); }
  uint32_t shares() const { return be32(p + 20); }
  std::string_view stock() const { return {reinterpret_cast<const char*>(p + 24), 8}; }
  uint32_t price() const { return be32(p + 32); }   // 4 implied decimals
};
struct AddOrderMPID : AddOrder { std::string_view mpid() const { return {reinterpret_cast<const char*>(p + 36), 4}; } };
struct OrderExecuted : Header {       // 'E'
  uint64_t ref() const { return be64(p + 11); }
  uint32_t shares() const { return be32(p + 19); }
  uint64_t match() const { return be64(p + 23); }
};
struct OrderExecutedWithPrice : OrderExecuted { // 'C'
  char printable() const { return char(p[31]); }
  uint32_t price() const { return be32(p + 32); }
};
struct OrderCancel : Header {         // 'X'
  uint64_t ref() const { return be64(p + 11); }
  uint32_t shares() const { return be32(p + 19); }
};
struct OrderDelete : Header { uint64_t ref() const { return be64(p + 11); } }; // 'D'
struct OrderReplace : Header {        // 'U'
  uint64_t old_ref() const { return be64(p + 11); }
  uint64_t new_ref() const { return be64(p + 19); }
  uint32_t shares() const { return be32(p + 27); }
  uint32_t price() const { return be32(p + 31); }
};
struct Trade : Header {               // 'P' non-cross trade
  uint64_t ref() const { return be64(p + 11); }
  char side() const { return char(p[19]); }
  uint32_t shares() const { return be32(p + 20); }
  std::string_view stock() const { return {reinterpret_cast<const char*>(p + 24), 8}; }
  uint32_t price() const { return be32(p + 32); }
  uint64_t match() const { return be64(p + 36); }
};
struct CrossTrade : Header {          // 'Q'
  uint64_t shares() const { return be64(p + 11); }
  std::string_view stock() const { return {reinterpret_cast<const char*>(p + 19), 8}; }
  uint32_t price() const { return be32(p + 27); }
  uint64_t match() const { return be64(p + 31); }
  char cross_type() const { return char(p[39]); }
};
struct BrokenTrade : Header { uint64_t match() const { return be64(p + 11); } }; // 'B'
struct Noii : Header {                // 'I'
  uint64_t paired() const { return be64(p + 11); }
  uint64_t imbalance() const { return be64(p + 19); }
  char direction() const { return char(p[27]); }
  uint32_t ref_price() const { return be32(p + 36); }
};

} // namespace itch
