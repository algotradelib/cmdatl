#pragma once

// What cmdatl decides about the library's built-in brokers, as constants (and a message's list of them): its word for each, the login it asks for, the
// roles it permits, and the rules the library does not state. Everything the library states (demo or not, capabilities,
// intervals) is read from its traits (`brokers/derive.hpp`); `src/catalog.cpp` checks this table against the library's
// list at compile time.
//
// Inside cmdatl's code a built-in is named by its traits name (`name_`); the user's word (`word_`) is read only through
// `from_word` and written back where a user sees it.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace cmdatl {
namespace builtin {

/** @brief Where an account's market data comes from on demo. */
enum class MarketDataLogin {
  /** @brief The account's own login. */
  OWN,
  /** @brief On demo, the broker's real login with no account (a KIS demo host refuses reference data and the calendar). */
  REAL_WHEN_DEMO,
};

/** @brief The form the operator file's account number takes. */
enum class NumberForm {
  /** @brief Any account number text. */
  ANY,
  /** @brief Eight digits, `-` and the two-digit product code, as `12345678-01`. */
  ACCOUNT_AND_PRODUCT,
};

/** @brief The tape a built-in's market data reads. */
enum class Tape {
  /** @brief The library's default (KRX and NXT combined). */
  DEFAULT,
  /** @brief KRX's alone. */
  KRX,
};

/** @brief One field of a login, as `auth login` asks for it. */
struct LoginField {
  /** @brief Its name in the library's JSON document. */
  std::string_view json_;
  /** @brief What login asks. */
  std::string_view prompt_;
  /** @brief Whether it is a secret: read with echo off, never shown. */
  bool secret_{false};
  /** @brief The value Enter gives; empty for a required field. */
  std::string_view default_;
  /** @brief Whether `auth status` shows it masked (an account number). */
  bool shown_masked_{false};
};

/** @brief What cmdatl decides about one built-in broker; constants, so code is compiled for a role or not. */
struct Facts {
  /** @brief `BrokerTraits<Client>::name`, its name inside cmdatl's code: `kis`, `kiwoom`, `tossinvest`. */
  std::string_view name_;
  /**
   * @brief The user's word: the CLI, `bars.source`, `[account] broker`, the stored login's `<word>/<env>`, the store's
   *   `source=` and replies: `kis`, `kiwoom`, `toss`.
   */
  std::string_view word_;
  /** @brief Its name for a person. */
  std::string_view display_;
  /** @brief Its short name, in messages. */
  std::string_view short_;
  /** @brief The umbrella header a driver's `broker.hpp` includes. */
  std::string_view library_header_;
  /** @brief The blocking client a driver's `broker.hpp` names. */
  std::string_view client_type_;
  /** @brief Whether cmdatl permits it as an account. */
  bool account_{false};
  /** @brief Whether cmdatl permits it as `bars.source`. */
  bool bar_source_{false};
  /** @brief Where an account's market data comes from on demo. */
  MarketDataLogin market_data_{MarketDataLogin::OWN};
  /** @brief The form the operator file's account number takes. */
  NumberForm number_{NumberForm::ANY};
  /** @brief The tape its market data reads. */
  Tape tape_{Tape::DEFAULT};
  /** @brief The store's `source=` for its bars, which names the tape. */
  std::string_view store_source_;
  /** @brief The intervals a fetch by count cannot page, space-separated (KIS: `1m`, whose request never leaves its day). */
  std::string_view count_fetch_refused_;
  /** @brief The broker's code for a second access token asked within a minute; empty for none. */
  std::string_view token_minute_code_;
  /** @brief The fields `auth login` asks for, the first `field_count_` of them. */
  std::array<LoginField, 4> fields_;
  /** @brief How many of `fields_` it asks for. */
  std::size_t field_count_{0};
  /** @brief The environment variables the library's `from_env()` reads, the first `env_var_count_` of them. */
  std::array<std::string_view, 5> env_vars_;
  /** @brief How many of `env_vars_` there are. */
  std::size_t env_var_count_{0};
};

/** @brief The replay market of every built-in's bars, until a row differs. */
inline constexpr std::string_view MARKET = "kr";

/** @brief Every built-in client's pacing, in requests a second, until a row differs. */
inline constexpr int REQUESTS_PER_SECOND = 1;

/** @brief The built-ins, in the order `auth login` lists them. */
inline constexpr std::array<Facts, 3> FACTS{{
  {"kis", "kis", "Korea Investment & Securities (KIS)", "KIS", "<algotradelib/kis>",
    "algotradelib::kis::KisBlockingClient", true, false, MarketDataLogin::REAL_WHEN_DEMO,
    NumberForm::ACCOUNT_AND_PRODUCT, Tape::KRX, "kis-krx", "1m", "EGW00133",
    {{{"app_key", "App key: ", false, "", false}, {"app_secret", "App secret: ", true, "", false},
      {"account_number", "Account number (8 digits): ", false, "", true},
      {"account_product_code", "Account product code (2 digits, Enter for 01): ", false, "01", false}}},
    4, {{"KIS_APP_KEY", "KIS_APP_SECRET", "KIS_DEMO", "KIS_CANO", "KIS_ACNT_PRDT_CD"}}, 5},
  {"kiwoom", "kiwoom", "Kiwoom Securities", "Kiwoom", "<algotradelib/kiwoom>",
    "algotradelib::kiwoom::KiwoomBlockingClient", false, false, MarketDataLogin::OWN, NumberForm::ANY, Tape::KRX,
    "kiwoom-krx", "", "", {{{"appkey", "App key: ", false, "", false}, {"secretkey", "Secret key: ", true, "", false}}},
    2, {{"KIWOOM_APP_KEY", "KIWOOM_SECRET_KEY", "KIWOOM_DEMO"}}, 3},
  {"tossinvest", "toss", "Toss Securities", "Toss", "<algotradelib/tossinvest>",
    "algotradelib::tossinvest::TossBlockingClient", false, false, MarketDataLogin::OWN, NumberForm::ANY, Tape::DEFAULT,
    "toss", "", "",
    {{{"client_id", "Client id: ", false, "", false}, {"client_secret", "Client secret: ", true, "", false}}}, 2,
    {{"TOSS_CLIENT_ID", "TOSS_CLIENT_SECRET"}}, 2},
}};

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
