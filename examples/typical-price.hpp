// A market-price rule for `replay.fill_header`: a market order fills at its fill bar's typical price, (high + low +
// close) / 3, a more cautious price than the open when a strategy's orders cannot all trade at the opening auction.
#pragma once

#include <algotradelib/backtest>

#include <optional>

/**
 * @brief The fill bar's typical price, to four decimals.
 * @param in The order, the price it was decided on and the fill bar.
 * @return The price, or nothing when it does not fit an exact decimal (a day order then waits for a later bar).
 */
inline std::optional<algotradelib::Decimal64> typical_price(const algotradelib::backtest::MarketPriceInput& in) {
  namespace atl = algotradelib;
  const auto& bar = in.next_.bar_;
  const auto sum = atl::decimal_add(bar.high_, bar.low_);
  const auto all = sum ? atl::decimal_add(*sum, bar.close_) : std::nullopt;
  return all ? atl::decimal_divide(*all, atl::Decimal64(3, 0), 4) : std::nullopt;
}
