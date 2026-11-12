// The external indicator and function of mean-gap.atls: sma's running sum, and a cap from another header.
#pragma once

#include "clip.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace atls::user {

/** @brief The mean of the last n values, by a running sum, as the built-in sma keeps it. */
class mean_of {
public:
  /**
   * @brief A window of n values.
   * @param n The window.
   */
  explicit mean_of(std::int64_t n) : n_(static_cast<std::size_t>(n)) {}

  /**
   * @brief Takes the newest value.
   * @param x The value.
   */
  void update(double x) {
    xs_.push_back(x);
    sum_ += x;
    if (xs_.size() > n_) {
      sum_ -= xs_.front();
      xs_.pop_front();
    }
  }

  /**
   * @brief The mean.
   * @return The window's sum over n.
   */
  double mean() const {
    return sum_ / static_cast<double>(n_);
  }

private:
  std::size_t n_;
  std::deque<double> xs_;
  double sum_{0};
};

} // namespace atls::user
