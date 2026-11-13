#pragma once

// A broker's desk: what the account and order commands see of a broker's account, borrowing the two sessions a run
// builds (the desk interface is written out above `AccountDesk` in `account.hpp`).
// Its trading session is the placement boundary (`GuardedSession`), the only session with `place` or `cancel` that
// leaves the desk; the market session is a read-only view, since on a real account it is the trading session itself.

#include "../core/time.hpp"
#include "../system/lock.hpp"
#include "./operator.hpp"
#include "./session.hpp"

#include <algotradelib/broker>
#include <algotradelib/strategy>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cmdatl {

/** @brief Thrown by a broker's desk before a broker read once a stop was asked for: no further read is made. */
struct Stopped : std::runtime_error {
  /** @brief The stop. */
  Stopped() : std::runtime_error("stopped before a broker read") {}
};

/**
 * @brief A market-data session as a desk lends it: quotes, price bands, bars and the calendar, and nothing that places,
 *   cancels, amends or hands out the session.
 * @tparam Market The session, borrowed.
 */
template <typename Market>
class MarketView {
public:
  /** @brief The session's capabilities. */
  using Caps = typename Market::Caps;

  /** @brief The broker, as its messages name it. */
  static constexpr std::string_view broker_name() {
    return Market::broker_name();
  }

  /**
   * @brief Borrows the session.
   * @param market The session; it must outlive the view.
   */
  explicit MarketView(Market& market) : market_(market) {}

  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto quote(atl::InstrumentId instrument) {
    return market_.quote(std::move(instrument));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto price_limits(atl::InstrumentId instrument) {
    return market_.price_limits(std::move(instrument));
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto bars(atl::BarQuery query) {
    return market_.bars(std::move(query));
  }
  /** @brief Forwarded. @param country The country. @param from The first date. @return The answer. */
  auto calendar(atl::Country country, std::optional<atl::Date> from = std::nullopt) {
    return market_.calendar(country, std::move(from));
  }

private:
  Market& market_;
};

/**
 * @brief A broker's account as the commands see it, its figures as of one instant.
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
  /** @brief The trading session: the placement boundary over the account's session. */
  using Session = GuardedSession<Trading, Market>;
  /** @brief The market-data session, read-only. */
  using MarketSession = MarketView<Market>;

  /**
   * @brief Borrows the sessions and puts the boundary over the trading one; nothing is read.
   * @param trading The account's session.
   * @param market The market-data session.
   * @param config The operator's file.
   * @param lock The account's lock, held; it must outlive the desk.
   * @param clock The live clock: the instant `begin()` takes, and the boundary's.
   * @param stopped Whether a stop was asked for: checked before every read, and by the boundary before a placement.
   */
  BrokerDesk(
    Trading& trading, Market& market, const OperatorConfig& config, const OwnerLock& lock,
    std::function<Clock::time_point()> clock, std::function<bool()> stopped
  )
  : clock_(std::move(clock)),
    stopped_(std::move(stopped)),
    session_(trading, market, placement_policy(config, lock), clock_, nullptr),
    market_(market) {
    session_.set_stopping(stopped_);
  }

  BrokerDesk(const BrokerDesk&) = delete;
  BrokerDesk& operator=(const BrokerDesk&) = delete;

  /** @brief Reads the calendar (none when it cannot be read: the phase is then unknown), then takes the instant. */
  void begin() {
    stop_check();
    auto calendar = market_.calendar(algotradelib::Country::KR);
    if (calendar) {
      calendar_ = std::move(calendar.value());
    }
    now_ = clock_();
  }

  /** @brief The placement boundary, borrowed. */
  Session& session() {
    return session_;
  }

  /** @brief The market-data session, read-only. */
  MarketSession& market_session() {
    return market_;
  }

  /**
   * @brief Throws once a stop was asked for; the reads call it before each broker read.
   * @throws Stopped Once a stop was asked for.
   */
  void stop_check() {
    if (stopped_()) {
      throw Stopped();
    }
  }

  /** @brief The instant the reply is as of. */
  Clock::time_point now() const {
    return now_;
  }

  /** @brief The live clock, which dates decisions, the journal's lines and plans. */
  Clock::time_point live_now() const {
    return clock_();
  }

  /** @brief How many placements and cancellations the boundary sent towards the broker. */
  std::uint64_t sends() const noexcept {
    return session_.sends();
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

  /** @brief The market phase's name at the instant: `unknown` when the calendar does not cover the date. */
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

  std::function<Clock::time_point()> clock_;
  std::function<bool()> stopped_;
  Session session_;
  MarketSession market_;
  std::optional<algotradelib::TradingCalendar> calendar_;
  Clock::time_point now_{};
};

} // namespace cmdatl
