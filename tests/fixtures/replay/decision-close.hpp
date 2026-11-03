// The textbook convention: a market order fills at the close it was decided on (with no price yet, at the next open).
#pragma once

#include <algotradelib/backtest>

inline std::optional<algotradelib::Decimal64> decision_close(const algotradelib::backtest::MarketPriceInput& in) {
  return in.decision_price_ ? in.decision_price_ : in.next_.bar_.open_;
}
