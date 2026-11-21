#pragma once

// What the library states about a built-in broker, read from its traits at compile time, beside cmdatl's own facts
// (`core/builtins.hpp`): demo or not, the tape setting and the login's loader; and the binary's catalog of them, defined
// in `src/catalog.cpp` (the one unit that reads the library's list; a driver never calls it).
// Includes only the facts and the library, so the catalog links no inline function a test builds with its hooks.

#include "../core/builtins.hpp"

#include <algotradelib/broker>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace cmdatl {
namespace brokers {

/** @brief cmdatl's interval words (`spec::INTERVALS`), of which a built-in's served intervals are taken. */
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
  // A fixed window phases only KRX: any other market's phase comes from the broker's stated session hours.
  static_assert(
    row->market_ == "kr" || algotradelib::BrokerCaps<Client>::has_calendar_sessions,
    "a builtin::FACTS row outside the kr market needs a client that states its session hours (has_calendar_sessions)"
  );
  return *row;
}

/**
 * @brief Whether a client has a row of `builtin::FACTS`: a listed client without one is not offered, and nothing but
 *   its traits name is compiled for it.
 * @tparam Client A client.
 */
template <typename Client>
inline constexpr bool has_facts = builtin::find(algotradelib::BrokerTraits<Client>::name) != nullptr;

/** @brief C++17 detection of a config's members `is_demo_` (`HasDemo`) and `market_tape_` (`HasTape`). */
template <typename Config, typename = void>
struct HasDemo : std::false_type {};
template <typename Config>
struct HasDemo<Config, std::void_t<decltype(std::declval<Config&>().is_demo_)>> : std::true_type {};
template <typename Config, typename = void>
struct HasTape : std::false_type {};
template <typename Config>
struct HasTape<Config, std::void_t<decltype(std::declval<Config&>().market_tape_)>> : std::true_type {};

/**
 * @brief Whether a config is a demo login's.
 * @param config The config.
 * @return Its `is_demo_` where it has one, else false (a broker without a demo).
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
 * @brief Reads KRX's tape alone wherever a config chooses one (KIS, Kiwoom; the library's default is KRX and NXT
 *   combined), so a daily bar closes at 15:30 and bars never come from another book than the quotes.
 * @param config The config.
 */
template <typename Config>
void set_tape(Config& config) {
  if constexpr (HasTape<Config>::value) {
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

/**
 * @brief A built-in broker: cmdatl's facts, whether its config has a demo, its login's loader (`read_login`), and of
 *   cmdatl's interval words those its traits serve (`supports_interval`).
 */
struct Entry {
  const builtin::Facts* facts_{nullptr};
  bool has_demo_{false};
  std::string (*read_login_)(const std::string& text, const std::string& origin){nullptr};
  std::vector<std::string> intervals_;
};

/**
 * @brief A client's entry, from its traits.
 * @tparam Client The client.
 * @return The entry.
 */
template <typename Client>
Entry entry() {
  Entry out{
    &facts_of<Client>(), HasDemo<typename algotradelib::BrokerTraits<Client>::Config>::value, &read_login<Client>, {}
  };
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
  return out;
}

/** @brief A client list's positions by traits name (`List` is an `algotradelib::type_list`). */
template <typename List>
struct ListedClients;
template <typename... Clients>
struct ListedClients<algotradelib::type_list<Clients...>> {
  /**
   * @brief The position of the client a name belongs to.
   * @param name A traits name.
   * @return Its position, or the list's size where no client has it.
   */
  static constexpr std::size_t position(std::string_view name) {
    const std::array<std::string_view, sizeof...(Clients)> names{algotradelib::BrokerTraits<Clients>::name...};
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) {
        return i;
      }
    }
    return names.size();
  }
  /** @brief The client at a position. */
  template <std::size_t I>
  using At = std::tuple_element_t<I, std::tuple<Clients...>>;
};

/**
 * @brief Whether a client list holds the client of every row of `builtin::FACTS` (a row naming no listed client is a
 *   typo or a broker the library removed).
 * @tparam List An `algotradelib::type_list`.
 * @return True when it does.
 */
template <typename List>
constexpr bool lists_every_row() {
  for (const auto& facts : builtin::FACTS) {
    if (ListedClients<List>::position(facts.name_) == List::size) {
      return false;
    }
  }
  return true;
}

/**
 * @brief The entries of the rows whose client a list holds, in `builtin::FACTS`' order; a listed client without a row
 *   has none.
 * @tparam List An `algotradelib::type_list`.
 * @tparam I The rows' positions.
 * @return The entries.
 */
template <typename List, std::size_t... I>
std::vector<Entry> catalog_of(std::index_sequence<I...>) {
  std::vector<Entry> out;
  const auto add = [&](auto row) {
    constexpr std::size_t at = ListedClients<List>::position(builtin::FACTS[decltype(row)::value].name_);
    if constexpr (at < List::size) {
      out.push_back(entry<typename ListedClients<List>::template At<at>>());
    }
  };
  (add(std::integral_constant<std::size_t, I>{}), ...);
  return out;
}

/**
 * @brief The entries of the rows whose client a list holds, in `builtin::FACTS`' order.
 * @tparam List An `algotradelib::type_list`.
 * @return The entries.
 */
template <typename List>
std::vector<Entry> catalog_of() {
  return catalog_of<List>(std::make_index_sequence<builtin::FACTS.size()>{});
}

/**
 * @brief The binary's built-in brokers (`src/catalog.cpp`, the static library `cmdatl_builtins`).
 * @return Them, in `builtin::FACTS`' order.
 */
const std::vector<Entry>& catalog();

/**
 * @brief A built-in broker by the user's word, mapped with `builtin::from_word`.
 * @param word The word, such as `toss`.
 * @return Its entry, or null.
 */
const Entry* find_entry(std::string_view word);

} // namespace brokers
} // namespace cmdatl
