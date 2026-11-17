// Measures of a run spec's own for the acceptance cases: one that computes, and two that cannot be called as a measure.
#pragma once

#include <algotradelib/backtest>

#include <cmath>
#include <stdexcept>

/**
 * @brief The total return again: the curve's last level over its first, minus 1.
 * @param in The replay.
 * @return The return, or NaN for an empty curve.
 */
inline double total_again(const algotradelib::backtest::MeasureInput& in) {
  const auto n = in.levels_.size();
  return n == 0 ? std::nan("") : in.levels_[n - 1] / in.levels_[0] - 1;
}

/**
 * @brief Takes the input by a reference it could change: not a measure.
 * @return 0.
 */
inline double wrong(algotradelib::backtest::MeasureInput&) {
  return 0;
}

/** @brief A result that converts to a double only when asked to. */
struct OnlyExplicit {
  /** @brief The value. */
  explicit operator double() const {
    return 0;
  }
};

/**
 * @brief Returns what is not a double without a cast: not a measure.
 * @return The result.
 */
inline OnlyExplicit explicit_only(const algotradelib::backtest::MeasureInput&) {
  return {};
}
