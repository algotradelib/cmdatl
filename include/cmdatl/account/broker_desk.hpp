#pragma once

// A broker's desk: what the account reads (`reads.hpp`) see of a broker's account, borrowing the two sessions a run
// builds (the desk interface is written out above `AccountDesk` in `account.hpp`).
// It places and cancels nothing: it has no placement boundary, and no read calls one.

#include "../core/time.hpp"

#include <algotradelib/broker>
#include <algotradelib/strategy>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cmdatl {

/** @brief Thrown by a broker's desk before a broker call once a stop was asked for: no further read is made. */
struct Stopped : std::runtime_error {
  /** @brief The stop. */
  Stopped() : std::runtime_error("stopped before a broker read") {}
};

/**
 * @brief A broker's account as the reads see it, as of one instant.
 *
 * `begin()` reads the calendar, then takes the instant; the phase and every acceptance are evaluated at it on that
 * calendar, and the figures read after it are later by the seconds the reads take.
 * The calendar gives days, not hours or halts: the phase is the library's KRX window on them.
 * @tparam Trading The account's session.
 * @tparam Market The session market data and the calendar are read from (the trading one, or one that cannot trade).
 */
template <typename Trading, typename Market>
class BrokerDesk {
public:
  /** @brief Not simulated: no cash of its own, no clock moved by command, no end to its data. */
  static constexpr bool simulated = false;
  /** @brief The broker, as its messages name it. */
  static constexpr std::string_view broker_name = Market::broker_name();
  /** @brief The trading session. */
  using Session = Trading;
  /** @brief The market-data session. */
  using MarketSession = Market;

  /**
   * @brief Borrows the sessions; nothing is read.
   * @param trading The account's session.
   * @param market The market-data session.
   * @param clock The instant `begin()` takes.
   * @param stopped Whether a stop was asked for, checked before every broker call.
   */
  BrokerDesk(Trading& trading, Market& market, std::function<Clock::time_point()> clock, std::function<bool()> stopped)
  : trading_(trading), market_(market), clock_(std::move(clock)), stopped_(std::move(stopped)) {}

  /** @brief Reads the calendar (none when it cannot be read: the phase is then unknown), then takes the instant. */
  void begin() {
    auto calendar = market_session().calendar(algotradelib::Country::KR);
    if (calendar) {
      calendar_ = std::move(calendar.value());
    }
    now_ = clock_();
  }

  /**
   * @brief The trading session, after the stop check.
   * @return The session.
   * @throws Stopped Once a stop was asked for.
   */
  Session& session() {
    stop_check();
    return trading_;
  }

  /**
   * @brief The market-data session, after the stop check.
   * @return The session.
   * @throws Stopped Once a stop was asked for.
   */
  MarketSession& market_session() {
    stop_check();
    return market_;
  }

  /**
   * @brief The instant the reply is as of.
   * @return It.
   */
  Clock::time_point now() const {
    return now_;
  }

  /**
   * @brief Whether the broker has a symbol: assumed, since checking costs a request a symbol (a wrong one shows as
   *   `no_price`).
   * @param symbol The symbol.
   * @return True.
   */
  bool has_symbol(const std::string& symbol) const {
    (void)symbol;
    return true;
  }

  /**
   * @brief The market phase at the instant.
   * @return Its name, such as `open`; `unknown` when the calendar does not cover the date.
   */
  std::string phase() const {
    return std::string(algotradelib::strategy::market_phase_name(phase_at(now_)));
  }

  /**
   * @brief Whether the calendar names the phase at an instant.
   * @param symbol The symbol (a last price is the quote's own gate).
   * @param at The instant.
   * @return Whether it does.
   */
  bool accepts_orders_at(const std::string& symbol, Clock::time_point at) const {
    (void)symbol;
    return phase_at(at) != algotradelib::strategy::MarketPhase::UNKNOWN;
  }

  /**
   * @brief What the broker shows for an order: `pending` when the working orders list it, else nothing; never filled,
   *   since the list holds open orders only.
   * @tparam Working The working-order listing.
   * @param id The order's id.
   * @param working The working orders, read once.
   * @return The status, or nothing.
   */
  template <typename Working>
  std::optional<std::string> order_status(const std::string& id, const Working& working) const {
    for (const auto& row : working.orders_) {
      if (row.order_.order_id_ == id) {
        return std::string("pending");
      }
    }
    return std::nullopt;
  }

private:
  /**
   * @brief The phase at an instant, on the calendar read.
   * @param at The instant.
   * @return The phase.
   */
  algotradelib::strategy::MarketPhase phase_at(Clock::time_point at) const {
    const auto window = algotradelib::strategy::SessionWindow::krx();
    const auto trading = calendar_ ? calendar_->is_trading_day(window.local_date(at)) : std::optional<bool>();
    return window.phase(at, trading);
  }

  /** @brief Throws once a stop was asked for. */
  void stop_check() {
    if (stopped_()) {
      throw Stopped();
    }
  }

  Trading& trading_;
  Market& market_;
  std::function<Clock::time_point()> clock_;
  std::function<bool()> stopped_;
  std::optional<algotradelib::TradingCalendar> calendar_;
  Clock::time_point now_{};
};

} // namespace cmdatl
