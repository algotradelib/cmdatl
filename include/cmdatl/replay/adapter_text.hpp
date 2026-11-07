#pragma once

// What cmdatl shows of a broker folder adapter's error: its category, HTTP status and a short broker code, never its
// text (a vendor may put its token in a URL, and the adapter is not cmdatl's code).

#include <algotradelib/core>

#include <algorithm>
#include <string>

namespace cmdatl {
namespace replay {

namespace atl = algotradelib;

/**
 * @brief An error's category, as a word.
 * @param category The category.
 * @return The word.
 */
inline const char* category_word(atl::ErrorCategory category) {
  switch (category) {
    case atl::ErrorCategory::CANCELLED:
      return "cancelled";
    case atl::ErrorCategory::TRANSPORT:
      return "transport";
    case atl::ErrorCategory::HTTP:
      return "http";
    case atl::ErrorCategory::AUTHENTICATION:
      return "authentication";
    case atl::ErrorCategory::RATE_LIMIT:
      return "rate_limit";
    case atl::ErrorCategory::PROTOCOL:
      return "protocol";
    case atl::ErrorCategory::INVALID_ARGUMENT:
      return "invalid_argument";
  }
  return "unknown";
}

/**
 * @brief A broker code as shown: a short printable one, else none (the adapter sets it, and it is not cmdatl's).
 * @param code The code.
 * @return It, or empty.
 */
inline std::string shown_code(const std::string& code) {
  const bool shown = !code.empty() && code.size() <= 32 && std::all_of(code.begin(), code.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
      c == '.';
  });
  return shown ? code : std::string();
}

/**
 * @brief What is shown of a broker folder adapter's error: its category, HTTP status and broker code, and whether it
 *   reached the broker; never its text.
 * @param error The error.
 * @return The text.
 */
inline std::string adapter_error_text(const atl::BrokerError& error) {
  std::string out = std::string("the broker folder's adapter answered ") + category_word(error.category_);
  if (error.http_status_ != 0) {
    out += ", HTTP " + std::to_string(error.http_status_);
  }
  const auto code = shown_code(error.broker_code_);
  if (!code.empty()) {
    out += ", code " + code;
  }
  out += error.reached_broker_ ? " (it reached the broker)" : " (before reaching the broker)";
  return out + "; its text is not shown";
}

} // namespace replay
} // namespace cmdatl
