#pragma once

// What cmdatl decides about the library's built-in brokers, as constants: its word for each, the login it asks for, the
// roles it permits, and the rules the library does not state. Everything the library states (demo or not, capabilities)
// is read from its traits (`brokers/derive.hpp`); `src/catalog.cpp` checks at compile time that every row names a client
// of the library's list (a listed client without a row is not offered).
//
// Inside cmdatl's code a built-in is named by its traits name (`name_`); the user's word (`word_`) is read only through
// `from_word` and written back where a user sees it.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace cmdatl {
namespace builtin {

/**
 * @brief One field of a login, as `auth login` asks for it: its name in the library's JSON document, the prompt, whether
 *   it is a secret (read with echo off), the value Enter gives (empty for a required field), and whether `auth status`
 *   shows it masked (an account number).
 */
struct LoginField {
  std::string_view json_;
  std::string_view prompt_;
  bool secret_{false};
  std::string_view default_;
  bool shown_masked_{false};
};

/**
 * @brief What cmdatl decides about one built-in broker; constants, so code is compiled for a role or not.
 *
 * `name_` is `BrokerTraits<Client>::name` (`tossinvest`); `word_` the user's (`toss`): the CLI, `[account] broker`, the
 * stored login's `<word>/<env>` and replies. `display_` and `short_` name it in prompts and messages; `client_type_` is
 * the blocking client a driver's `broker.hpp` names. `account_`: cmdatl permits it as an account.
 * `real_market_data_on_demo_`: a demo account reads market data with the real login (a KIS demo host refuses reference
 * data and the calendar). `account_and_product_`: the operator file's number is eight digits, `-`, two digits.
 * `token_minute_code_`: the broker's code for a second access token asked within a minute, or empty. The login's fields
 * and the environment variables the library's `from_env()` reads are the first `field_count_` and `env_var_count_`.
 * As a bar source: `store_source_` is the store's `source=` for its bars, naming the tape (`kis-krx`); intervals a fetch
 * by count cannot page are `count_fetch_refused_` (KIS's `1m`, whose request never leaves its day).
 * `market_` is the market of its bars and its account (`kr` or `us`, `MARKETS`); `feed_` the feed a run's start line
 * names (`KRX`, or empty); `demo_bars_`: a backtest's fetch reads the demo login, not the real one.
 */
struct Facts {
  std::string_view name_;
  std::string_view word_;
  std::string_view display_;
  std::string_view short_;
  std::string_view client_type_;
  bool account_{false};
  bool real_market_data_on_demo_{false};
  bool account_and_product_{false};
  std::string_view token_minute_code_;
  std::array<LoginField, 4> fields_;
  std::size_t field_count_{0};
  std::array<std::string_view, 5> env_vars_;
  std::size_t env_var_count_{0};
  std::string_view store_source_;
  std::string_view count_fetch_refused_;
  std::string_view market_;
  std::string_view feed_;
  bool demo_bars_{false};
};

/**
 * @brief A market an account or a built-in's bars may be in, for messages: its word (a run spec's `replay.market`), what
 *   it trades, its currency's code and the day an account's daily allowance is counted on.
 */
struct Market {
  std::string_view word_;
  std::string_view phrase_;
  std::string_view currency_;
  std::string_view day_;
};

/** @brief The markets: KRX shares in won, US shares in dollars. */
inline constexpr std::array<Market, 2> MARKETS{{
  {"kr", "KRX shares in won", "KRW", "KST date"},
  {"us", "US shares in dollars", "USD", "US trading day, counted from 00:00 at UTC-5"},
}};

/** @brief The built-ins cmdatl offers, in the order `auth login` lists them; each names a client of the library's list. */
inline constexpr std::array<Facts, 4> FACTS{{
  {"kis", "kis", "Korea Investment & Securities (KIS)", "KIS", "algotradelib::kis::KisBlockingClient", true, true, true,
    "EGW00133",
    {{{"app_key", "App key: ", false, "", false}, {"app_secret", "App secret: ", true, "", false},
      {"account_number", "Account number (8 digits): ", false, "", true},
      {"account_product_code", "Account product code (2 digits, Enter for 01): ", false, "01", false}}},
    4, {{"KIS_APP_KEY", "KIS_APP_SECRET", "KIS_DEMO", "KIS_CANO", "KIS_ACNT_PRDT_CD"}}, 5, "kis-krx", "1m", "kr", "KRX",
    false},
  {"kiwoom", "kiwoom", "Kiwoom Securities", "Kiwoom", "algotradelib::kiwoom::KiwoomBlockingClient", false, false, false,
    "", {{{"appkey", "App key: ", false, "", false}, {"secretkey", "Secret key: ", true, "", false}}}, 2,
    {{"KIWOOM_APP_KEY", "KIWOOM_SECRET_KEY", "KIWOOM_DEMO"}}, 3, "kiwoom-krx", "", "kr", "KRX", false},
  {"tossinvest", "toss", "Toss Securities", "Toss", "algotradelib::tossinvest::TossBlockingClient", false, false, false,
    "", {{{"client_id", "Client id: ", false, "", false}, {"client_secret", "Client secret: ", true, "", false}}}, 2,
    {{"TOSS_CLIENT_ID", "TOSS_CLIENT_SECRET"}}, 2, "toss", "", "kr", "", false},
  {"alpaca", "alpaca", "Alpaca", "Alpaca", "algotradelib::alpaca::AlpacaBlockingClient", true, false, false, "",
    {{{"key_id", "API key ID: ", false, "", false}, {"secret_key", "Secret key: ", true, "", false}}}, 2,
    {{"ALPACA_KEY", "ALPACA_SECRET", "ALPACA_DEMO"}}, 3, "alpaca-iex", "", "us", "IEX", true},
}};

/**
 * @brief Whether no two rows share a traits name or a user's word.
 * @return True when every `name_` and every `word_` is unique.
 */
constexpr bool rows_are_distinct() {
  for (std::size_t i = 0; i < FACTS.size(); ++i) {
    for (std::size_t j = i + 1; j < FACTS.size(); ++j) {
      if (FACTS[i].name_ == FACTS[j].name_ || FACTS[i].word_ == FACTS[j].word_) {
        return false;
      }
    }
  }
  return true;
}

// A row copied from another and left with its name or word would send one broker's commands to the other.
static_assert(rows_are_distinct(), "two rows of builtin::FACTS share a name_ or a word_");

/**
 * @brief The row of a built-in by its traits name.
 * @param name `BrokerTraits<Client>::name`.
 * @return The row, or null.
 */
constexpr const Facts* find(std::string_view name) {
  for (const auto& facts : FACTS) {
    if (facts.name_ == name) {
      return &facts;
    }
  }
  return nullptr;
}

/**
 * @brief The row of a built-in by the user's word: the one place a user's word is mapped to a broker.
 * @param word The word, such as `toss`.
 * @return The row, or null.
 */
constexpr const Facts* from_word(std::string_view word) {
  for (const auto& facts : FACTS) {
    if (facts.word_ == word) {
      return &facts;
    }
  }
  return nullptr;
}

/**
 * @brief Whether every row's market is one of `MARKETS`.
 * @return True when it is.
 */
constexpr bool rows_name_markets() {
  for (const auto& facts : FACTS) {
    bool known = false;
    for (const auto& market : MARKETS) {
      known = known || facts.market_ == market.word_;
    }
    if (!known) {
      return false;
    }
  }
  return true;
}

static_assert(rows_name_markets(), "a row of builtin::FACTS names a market_ that MARKETS does not list");

/**
 * @brief The market of an account by its broker's word: the row's for a built-in, `kr` for the mock, a broker folder
 *   or an unknown word.
 * @param word `[account] broker`.
 * @return `kr` or `us`.
 */
constexpr std::string_view account_market_word(std::string_view word) {
  const auto* facts = from_word(word);
  return facts != nullptr ? facts->market_ : std::string_view("kr");
}

/**
 * @brief A market's row, for a message.
 * @param word `kr` or `us`.
 * @return Its row; `kr`'s for an unknown word.
 */
constexpr const Market& market_row(std::string_view word) {
  for (const auto& market : MARKETS) {
    if (market.word_ == word) {
      return market;
    }
  }
  return MARKETS[0];
}

/**
 * @brief The built-ins cmdatl permits as accounts, for a message: each word quoted, separated by commas.
 * @return Them, such as `"kis"`.
 */
inline std::string account_words() {
  std::string words;
  for (const auto& facts : FACTS) {
    if (facts.account_) {
      words += (words.empty() ? "\"" : ", \"") + std::string(facts.word_) + "\"";
    }
  }
  return words;
}

} // namespace builtin
} // namespace cmdatl
