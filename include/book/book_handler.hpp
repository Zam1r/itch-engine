// Glue: ITCH decoder callbacks -> order book operations. Works with any book
// exposing add/reduce/remove/replace (FastBook, MapBook).
#pragma once
#include "itch/decoder.hpp"
#include "book/common.hpp"
#include <array>
#include <string>

namespace book {
template <class Book>
struct BookHandler : itch::NullHandler {
  using itch::NullHandler::on_add_mpid;
  Book& b;
  uint64_t counts[256] = {};
  std::array<std::string, 65536>* names = nullptr;
  explicit BookHandler(Book& bk) : b(bk) {}
  void on_stock_directory(itch::StockDirectory m) {
    b.reserve_symbol(m.locate());
    if (names) (*names)[m.locate()] = std::string(m.stock());
  }
  void on_add(itch::AddOrder m) { b.add(m.locate(), m.ref(), side_of(m.side()), m.shares(), m.price()); }
  void on_add_mpid(itch::AddOrderMPID m) { on_add(m); }
  void on_executed(itch::OrderExecuted m) { b.reduce(m.ref(), m.shares()); }
  void on_executed_price(itch::OrderExecutedWithPrice m) { b.reduce(m.ref(), m.shares()); }
  void on_cancel(itch::OrderCancel m) { b.reduce(m.ref(), m.shares()); }
  void on_delete(itch::OrderDelete m) { b.remove(m.ref()); }
  void on_replace(itch::OrderReplace m) { b.replace(m.old_ref(), m.new_ref(), m.shares(), m.price()); }
};
}
