#pragma once

// What the library states about a built-in broker, read from its traits at compile time, beside cmdatl's own facts
// (`core/builtins.hpp`): demo or not, the intervals, the tape setting and the login's loader.
// Includes only the facts and the library: never `system/`, so the catalog (`src/catalog.cpp`) links no inline function
// of cmdatl's that a test builds with its hooks.

#include "../core/builtins.hpp"

#include <algotradelib/broker>

#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cmdatl {
namespace brokers {

/** @brief cmdatl's interval words, of which a built-in's served intervals are taken (`spec::INTERVALS`). */
inline constexpr std::array<std::string_view, 8> INTERVAL_WORDS{"1m", "5m", "15m", "30m", "1h", "1d", "1w", "1mo"};

/**
 * @brief The row of `builtin::FACTS` for a client, found at compile time; a client without one does not compile.
 * @tparam Client A blocking client.
 * @return The row.
 */
template <typename Client>
constexpr const builtin::Facts& facts_of() {
  constexpr const builtin::Facts* row = builtin::find(algotradelib::BrokerTraits<Client>::name);
  static_assert(row != nullptr, "the client has no row in cmdatl's builtin::FACTS (core/builtins.hpp)");
  return *row;
}

/** @brief Whether a config has the member `is_demo_` (C++17 detection). */
template <typename Config, typename = void>
struct HasDemo : std::false_type {};

/** @brief Whether a config has the member `is_demo_`: it has. */
template <typename Config>
struct HasDemo<Config, std::void_t<decltype(std::declval<Config&>().is_demo_)>> : std::true_type {};

/** @brief Whether a config has the member `market_tape_` (C++17 detection). */
template <typename Config, typename = void>
struct HasTape : std::false_type {};

/** @brief Whether a config has the member `market_tape_`: it has. */
template <typename Config>
struct HasTape<Config, std::void_t<decltype(std::declval<Config&>().market_tape_)>> : std::true_type {};

/** @brief Whether a built-in has a demo environment: its config states `is_demo_`. */
template <typename Client>
inline constexpr bool has_demo = HasDemo<typename algotradelib::BrokerTraits<Client>::Config>::value;

/**
 * @brief Whether a config is a demo login's.
 * @param config The config.
 * @return Its `is_demo_` where it has one, else false.
 */
template <typename Config>
bool is_demo(const Config& config) {
  if constexpr (HasDemo<Config>::value) {
    return config.is_demo_;
  } else {
    static_cast<void>(config);
    return false;
  }
}

/**
 * @brief Sets the tape cmdatl's facts choose for a client's market data; a tape asked of a config without one does not
 *   compile.
 * @tparam Client The client.
 * @param config Its config.
 */
template <typename Client>
void set_tape(typename algotradelib::BrokerTraits<Client>::Config& config) {
  using Config = typename algotradelib::BrokerTraits<Client>::Config;
  if constexpr (facts_of<Client>().tape_ == builtin::Tape::KRX) {
    static_assert(
      HasTape<Config>::value, "the facts ask for the KRX tape, and the client's config has no market_tape_"
    );
    config.market_tape_ = decltype(config.market_tape_)::KRX;
  } else {
    static_cast<void>(config);
  }
}

/**
 * @brief Reads a stored login with the library's loaders, as a run will: the config, and the account where the login
 *   carries it (a broker that lists no accounts).
 * @tparam Client The client.
 * @param text The login.
 * @param origin Where it came from, for the loaders' messages.
 * @return The library's identity of the login's key, which names its saved access token.
 * @throws std::runtime_error with the library's message (the field, never its value).
 */
template <typename Client>
std::string read_login(const std::string& text, const std::string& origin) {
  using Traits = algotradelib::BrokerTraits<Client>;
  typename Traits::Config config;
  config.from_json_text(text, origin);
  if constexpr (!algotradelib::BrokerCaps<Client>::has_account_discovery) {
    typename Traits::Account account;
    account.from_json_text(text, origin);
  }
  return config.token_identity();
}

/** @brief A built-in broker: cmdatl's facts and what the library states. */
struct Entry {
  /** @brief cmdatl's facts. */
  const builtin::Facts* facts_{nullptr};
  /** @brief Whether it has a demo environment. */
  bool has_demo_{false};
  /** @brief Of cmdatl's interval words, those the library serves for it, in that order. */
  std::vector<std::string> intervals_;
  /** @brief Reads a stored login with the library's loaders (`read_login`). */
  std::string (*read_login_)(const std::string& text, const std::string& origin){nullptr};
};

/**
 * @brief A client's entry, from its traits.
 * @tparam Client The client.
 * @return The entry.
 */
template <typename Client>
Entry entry() {
  Entry out;
  out.facts_ = &facts_of<Client>();
  out.has_demo_ = has_demo<Client>;
  for (const auto word : INTERVAL_WORDS) {
    for (const auto interval : algotradelib::all_bar_intervals) {
      if (
        algotradelib::bar_interval_name(interval) == word &&
        algotradelib::BrokerCaps<Client>::supports_interval(interval)
      ) {
        out.intervals_.emplace_back(word);
      }
    }
  }
  out.read_login_ = &read_login<Client>;
  return out;
}

} // namespace brokers
} // namespace cmdatl
