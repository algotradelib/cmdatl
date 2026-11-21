#pragma once

// The market an account trades, from its broker's row of the facts (`core/builtins.hpp`): `kr` for KIS, the mock and
// broker folders, `us` for Alpaca. Every account path takes its instruments, the calendar's country, the currency, the
// account's day and the phase from here, never from a KRX constant.

#include "../core/builtins.hpp"
#include "../core/time.hpp"
#include "./operator.hpp"

#include <algotradelib/broker>
#include <algotradelib/strategy>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace cmdatl {

/** @brief The market an account trades. */
struct AccountMarket {
  /** @brief `kr` or `us`. */
  std::string word_;
  /** @brief The calendar's country and the instruments'. */
  algotradelib::Country country_{algotradelib::Country::KR};
  /** @brief The instruments' venue: KRX and NXT combined in `kr`, unspecified in `us` (the broker routes). */
  algotradelib::Venue venue_{algotradelib::Venue::COMBINED};
  /** @brief Every amount's currency. */
  algotradelib::Currency currency_{algotradelib::Currency::krw()};
  /**
   * @brief The regular session at a fixed offset: it dates the account's day, and phases only where the broker states
   *   no session hours (`market_phase`).
   */
  algotradelib::strategy::SessionWindow window_{algotradelib::strategy::SessionWindow::krx()};
};

/**
 * @brief A market by its word.
 * @param word `kr`, `us`; any other word is `kr`.
 * @return The market: KRX's for `kr`; for `us` the US regular session, 09:30-16:00 at a fixed UTC-5, as replay's `us`
 *   preset.
 */
inline AccountMarket market_of_word(const std::string& word) {
  AccountMarket market;
  if (word != "us") {
    market.word_ = "kr";
    return market;
  }
  market.word_ = "us";
  market.country_ = algotradelib::Country::US;
  market.venue_ = algotradelib::Venue::UNSPECIFIED;
  market.currency_ = algotradelib::Currency::usd();
  market.window_.open_ = std::chrono::hours(9) + std::chrono::minutes(30);
  market.window_.close_ = std::chrono::hours(16);
  market.window_.utc_offset_ = std::chrono::hours(-5);
  market.window_.source_ =
    "US regular session, 09:30-16:00 at UTC-5 (cmdatl's us market; daylight saving is not modelled)";
  return market;
}

/**
 * @brief The market of an operator file's account: its built-in broker's row, `kr` for the mock and a broker folder.
 * @param config The operator's file.
 * @return The market.
 */
inline AccountMarket account_market(const OperatorConfig& config) {
  return market_of_word(std::string(builtin::account_market_word(config.broker_)));
}

/**
 * @brief An instrument of the market.
 * @param market The market.
 * @param symbol The symbol.
 * @return The instrument.
 */
inline algotradelib::InstrumentId instrument_of(const AccountMarket& market, const std::string& symbol) {
  return {market.country_, market.venue_, symbol};
}

/**
 * @brief The account's date of an instant, at the window's offset: the KST date in `kr`; in `us` a day that starts at
 *   05:00 UTC (00:00 EST, 01:00 EDT), outside every US session in both seasons.
 * @param market The market.
 * @param at The instant.
 * @return Days from 1970-01-01.
 */
inline std::int64_t market_day(const AccountMarket& market, Clock::time_point at) {
  return utc_day(at + market.window_.utc_offset_);
}

/**
 * @brief The market's phase at an instant: from the session hours the calendar states where the broker states them
 *   (`Caps::has_calendar_sessions`), else the window's on the calendar's trading days.
 * @tparam Caps The market session's capabilities.
 * @param market The market.
 * @param calendar The calendar read, or null when none could be read.
 * @param at The instant.
 * @return The phase; `UNKNOWN` without a calendar where the broker states session hours.
 */
template <typename Caps>
algotradelib::strategy::MarketPhase market_phase(
  const AccountMarket& market, const algotradelib::TradingCalendar* calendar, Clock::time_point at
) {
  if constexpr (Caps::has_calendar_sessions) {
    return calendar != nullptr ?
      algotradelib::strategy::stated_session_phase(*calendar, at) :
      algotradelib::strategy::MarketPhase::UNKNOWN;
  } else {
    const auto trading =
      calendar != nullptr ? calendar->is_trading_day(market.window_.local_date(at)) : std::optional<bool>();
    return market.window_.phase(at, trading);
  }
}

} // namespace cmdatl
