#pragma once
#include "itch/messages.hpp"
#include <cstddef>

namespace itch {

// Handler concept: any type with on_*(Msg) members; defaults are no-ops via
// NullHandler inheritance. Dispatch is a switch on the type byte, fully
// inlined (CRTP-free; templates do the work).
struct NullHandler {
  void on_system_event(SystemEvent) {}
  void on_stock_directory(StockDirectory) {}
  void on_trading_action(StockTradingAction) {}
  void on_add(AddOrder) {}
  void on_add_mpid(AddOrderMPID m) { on_add(m); }
  void on_executed(OrderExecuted) {}
  void on_executed_price(OrderExecutedWithPrice) {}
  void on_cancel(OrderCancel) {}
  void on_delete(OrderDelete) {}
  void on_replace(OrderReplace) {}
  void on_trade(Trade) {}
  void on_cross_trade(CrossTrade) {}
  void on_broken_trade(BrokenTrade) {}
  void on_noii(Noii) {}
  void on_other(Header) {}   // Y L V W K J h N O
  void on_unknown(Header) {}
};

// Decode one message payload (no length prefix). Returns false for unknown type.
template <class H>
inline bool dispatch(const uint8_t* p, H& h) {
  switch (char(p[0])) {
    case 'A': h.on_add(AddOrder{{p}}); return true;
    case 'F': h.on_add_mpid(AddOrderMPID{{{p}}}); return true;
    case 'E': h.on_executed(OrderExecuted{{p}}); return true;
    case 'C': h.on_executed_price(OrderExecutedWithPrice{{{p}}}); return true;
    case 'X': h.on_cancel(OrderCancel{{p}}); return true;
    case 'D': h.on_delete(OrderDelete{{p}}); return true;
    case 'U': h.on_replace(OrderReplace{{p}}); return true;
    case 'P': h.on_trade(Trade{{p}}); return true;
    case 'Q': h.on_cross_trade(CrossTrade{{p}}); return true;
    case 'B': h.on_broken_trade(BrokenTrade{{p}}); return true;
    case 'I': h.on_noii(Noii{{p}}); return true;
    case 'S': h.on_system_event(SystemEvent{{p}}); return true;
    case 'R': h.on_stock_directory(StockDirectory{{p}}); return true;
    case 'H': h.on_trading_action(StockTradingAction{{p}}); return true;
    case 'Y': case 'L': case 'V': case 'W': case 'K': case 'J': case 'h': case 'N': case 'O':
      h.on_other(Header{p}); return true;
    default: h.on_unknown(Header{p}); return false;
  }
}

// Iterates a buffer of length-prefixed messages (NASDAQ binary file framing:
// 2-byte big-endian length + payload). Stops at a truncated tail.
template <class H>
inline size_t decode_buffer(const uint8_t* buf, size_t len, H& h) {
  size_t off = 0, n = 0;
  while (off + 2 <= len) {
    uint16_t ml = be16(buf + off);
    if (off + 2 + ml > len) break;
    dispatch(buf + off + 2, h);
    off += 2 + ml; ++n;
  }
  return n;
}

} // namespace itch
