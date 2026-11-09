#pragma once

// The KRX stock tick table, from the library before and after it moved out of core into <algotradelib/krx>.
// Drop the old branch when that library is published (plan/todo/krx-include-cleanup.md).

#include <algotradelib/core>

#if __has_include(<algotradelib/krx>)
#include <algotradelib/krx>
#endif

namespace cmdatl {
namespace paper {

/**
 * @brief The KRX stock tick table.
 * @return The library's rule for KRX stocks.
 */
inline algotradelib::PriceRule krx_equity_rule() {
#if __has_include(<algotradelib/krx>)
  return algotradelib::krx::equity_price_rule();
#else
  return algotradelib::PriceRule::krx_equity();
#endif
}

} // namespace paper
} // namespace cmdatl
