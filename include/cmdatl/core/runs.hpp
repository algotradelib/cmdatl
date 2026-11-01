#pragma once

// What a run's driver and `run start` both read: the run id's grammar, atlc's arguments, the market settings a run
// refuses, and the bound on a run's description.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

/** @brief The largest run description (`run.json`), in bytes: `run start` refuses a larger one, `run status` reads no more. */
inline constexpr std::size_t RUN_RECORD_MAX = 64u << 10;

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

/** @brief Why a run refuses a program with session legs: one wording for `run start` and the driver. */
inline constexpr const char* LEGS_UNSUPPORTED =
  "the program has on_open/on_close (session legs), which no live source delivers";

/** @brief What to do with a program with session legs. */
inline constexpr const char* LEGS_UNSUPPORTED_NEXT =
  "run it with cmdatl strategy backtest; a run takes programs without session legs";

/**
 * @brief Why a run spec's market settings are refused before anything is loaded, or nothing: the market `kr`, no store,
 *   no short sales, no session legs.
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
  if (atlc_value(args, "data.store")) {
    return std::make_pair(std::string("data.store"), std::string("a run reads files, not a store: give [data] a file"));
  }
  if (atlc_value(args, "replay.shorts").value_or("false") == "true") {
    return std::make_pair(
      std::string("replay.shorts"), std::string("a run keeps the sellable check on: remove [replay] shorts")
    );
  }
  if (atlc_value(args, "replay.fill").value_or("next-open") == "legs") {
    return std::make_pair(
      std::string("replay.fill"), std::string("a run has no session legs: remove [replay] fill = \"legs\"")
    );
  }
  return std::nullopt;
}

} // namespace cmdatl
