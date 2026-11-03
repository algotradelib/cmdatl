// The textbook convention: a market order fills at the close it was decided on (the first bar, which has none, at its
// open).
#pragma once

#include <algotradelib/backtest>

inline std::optional<algotradelib::Decimal64> decision_close(const algotradelib::backtest::MarketPriceInput& in) {
  return in.previous_ ? std::optional<algotradelib::Decimal64>(in.previous_->close_) : in.next_.bar_.open_;
}
