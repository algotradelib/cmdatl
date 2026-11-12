// The external function of mean-gap.atls.
#pragma once

#include <algorithm>

namespace atls::user {

/**
 * @brief A value kept within a cap of zero.
 * @param z The value.
 * @param cap The cap.
 * @return z, at least -cap and at most cap.
 */
inline double clip(double z, double cap) {
  return std::min(std::max(z, -cap), cap);
}

} // namespace atls::user
