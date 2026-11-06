#pragma once

// What a run's driver and `run` both read: the run id's grammar, atlc's arguments, and the market settings a run
// refuses.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

/**
 * @brief Whether a text is a run id: `r` and 12 lowercase hex digits.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_run_id(const std::string& text) {
  return text.size() == 13 && text[0] == 'r' && std::all_of(text.begin() + 1, text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

/**
 * @brief The last value of an atlc argument.
 * @param args atlc's arguments.
 * @param key The key.
 * @return Its value, if given.
 */
inline std::optional<std::string> atlc_value(const std::vector<std::string>& args, const std::string& key) {
  std::optional<std::string> found;
  for (const auto& arg : args) {
    if (arg.size() > key.size() && arg.compare(0, key.size(), key) == 0 && arg[key.size()] == '=') {
      found = arg.substr(key.size() + 1);
    }
  }
  return found;
}

/** @brief Why a run refuses a program with session handlers: one wording for `run` and the driver. */
inline constexpr const char* SESSION_UNSUPPORTED =
  "the program has on_open/on_close, and a run feeds daily bars as they close, so no bar opens a session before it "
  "closes";

/** @brief What to do with a program with session handlers. */
inline constexpr const char* SESSION_UNSUPPORTED_NEXT =
  "run it with cmdatl strategy backtest (intraday bars, or bars.open_point on daily ones); a run takes programs "
  "without on_open/on_close";

/**
 * @brief Why a run spec's market settings are refused before anything is loaded, or nothing: the market `kr`, no store,
 *   no short sales, no fill at a decided close, no open points.
 * @param args atlc's arguments.
 * @return The key at fault and why, or nothing.
 */
inline std::optional<std::pair<std::string, std::string>> spec_refusal(const std::vector<std::string>& args) {
  const auto market = atlc_value(args, "replay.market");
  if (!market || *market != "kr") {
    return std::make_pair(
      std::string("replay.market"),
      std::string("a run trades KRX shares in won, under the operator's limits: set [replay] market = \"kr\"")
    );
  }
  if (atlc_value(args, "bars.store")) {
    return std::make_pair(std::string("bars.store"), std::string("a run reads files, not a store: give [bars] a file"));
  }
  if (atlc_value(args, "replay.shorts").value_or("false") == "true") {
    return std::make_pair(
      std::string("replay.shorts"), std::string("a run keeps the sellable check on: remove [replay] shorts")
    );
  }
  if (atlc_value(args, "replay.fill").value_or("next-open") != "next-open") {
    return std::make_pair(
      std::string("replay.fill"),
      std::string("a market-price rule prices a backtest's fills; a run's fills are the market's: remove [replay] fill")
    );
  }
  if (atlc_value(args, "bars.open_point").value_or("false") == "true") {
    return std::make_pair(
      std::string("bars.open_point"),
      std::string("bars.open_point invents a bar a run never sees: remove [bars] open_point")
    );
  }
  return std::nullopt;
}

} // namespace cmdatl
