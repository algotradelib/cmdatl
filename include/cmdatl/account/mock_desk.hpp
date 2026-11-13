#pragma once

// The mock desk: the library's mock market kept in a state file in the account's directory, traded through the
// placement boundary. The market is a pure function of the operator's bars, cash and start and of the changes the file
// records, so the file is the account's whole market state between commands.

#include "../system/lock.hpp"
#include "./bars.hpp"
#include "./operator.hpp"
#include "./session.hpp"

#include <algotradelib/broker>
#include <algotradelib/krx>
#include <algotradelib/mock>
#include <algotradelib/strategy>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

template <typename Desk>
class BasicAccount;
struct MockDeskTest;

/** @brief The key only an account can make: a desk is built only by an account holding its lock. */
class MockDeskKey {
  template <typename Desk>
  friend class BasicAccount;
  friend struct MockDeskTest;
  MockDeskKey() = default;
};

/** @brief How a clock move ended. */
enum class Moved {
  /** @brief The clock moved. */
  OK,
  /** @brief The account's lock was replaced; nothing was written. */
  LOCK_LOST,
  /** @brief The state file did not record the move; the clock did not move. */
  NOT_WRITTEN,
  /** @brief The state file may hold the move, which was not made here. */
  UNCONFIRMED,
  /** @brief The market halted on this move or before it; `halted()` says why. */
  HALTED,
};

/** @brief What the market did to one order during a clock move. */
struct MockEvent {
  /** @brief When. */
  Clock::time_point at_{};
  /** @brief `filled` or `expired`. */
  std::string kind_;
  /** @brief The order. */
  std::string order_id_;
  /** @brief Its symbol. */
  std::string symbol_;
  /** @brief `buy` or `sell`. */
  std::string side_;
  /** @brief The shares filled, or left when it expired. */
  std::string quantity_;
  /** @brief For a fill: its price, commission and tax. */
  std::optional<std::string> price_;
  /** @brief The fill's commission. */
  std::string commission_;
  /** @brief The fill's tax. */
  std::string tax_;
};

/** @brief The state file's name in the account's directory. */
inline constexpr const char* MOCK_STATE_FILE = "market.jsonl";

/**
 * @brief A decimal as whole-number text when it is whole, without trailing zeros otherwise.
 * @param value The decimal.
 * @return The text.
 */
inline std::string decimal_text_of(const atl::Decimal64& value) {
  return atl::decimal_normalized(value).str();
}

/**
 * @brief One symbol's bars as the mock's daily series: each date's session from 09:00 to 15:30 KST, stamped at its close.
 * @param symbol The symbol.
 * @param bars Its bars.
 * @return The series.
 */
inline atl::BarSeries mock_series(const std::string& symbol, const std::vector<DayBar>& bars) {
  const auto price = [](double value) {
    // A bar's price has at most four decimals and is at most 10^7, so it is exact in ten-thousandths.
    return atl::decimal_normalized(atl::Decimal64(std::llround(value * 10000), 4));
  };
  atl::BarSeries series;
  series.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol};
  series.interval_ = atl::BarInterval::ONE_DAY;
  series.stamp_ = atl::BarStamp::CLOSE;
  series.currency_ = atl::Currency::krw();
  for (const auto& day : bars) {
    atl::Bar bar;
    bar.timestamp_ = open_of(day.day_) + SESSION_LENGTH;
    bar.open_ = price(day.open_);
    bar.high_ = price(day.high_);
    bar.low_ = price(day.low_);
    bar.close_ = price(day.close_);
    bar.volume_ = atl::Decimal64(day.volume_, 0);
    series.bars_.push_back(bar);
  }
  return series;
}

/**
 * @brief The mock market, account and clock of one account, as its state file leaves them: a desk (the interface the
 *   commands use, written out in `account.hpp`).
 *
 * Its trading session is the placement boundary (`GuardedSession`) over the library's session on the kept market:
 * every placement passes the armed file, the lock, the order type, the collar and the operator's phases before it can
 * reach the market, and every cancellation the lock.
 */
class MockDesk {
public:
  /** @brief The environment this session is: every reply's `env`. */
  static constexpr const char* env_name = "mock";
  /** @brief The broker this session is. */
  static constexpr const char* broker_name = "mock";
  /** @brief A simulated broker: it has its own cash, a clock moved by command and an end to its data. */
  static constexpr bool simulated = true;

  /** @brief The library's session on the kept market. */
  using MarketSession = atl::BlockingSession<atl::mock::MockClient>;
  /** @brief The trading session: the placement boundary over the market session. */
  using Session = GuardedSession<MarketSession>;

  /**
   * @brief Opens the state file in the lock's directory, creating it when absent, and replays it into a market built
   *   from the bars, the operator's cash and start and the KRX tick grid, with the mock's own fill model (next open, no
   *   charges).
   *
   * Only an account holding this account's lock can. The file never throws: check `opened()` and `rejection()`.
   * @param key The account's key.
   * @param config The operator's file.
   * @param lock The held lock; its directory must be this config's account directory.
   * @param data The bars, read and checked.
   * @throws std::logic_error When the lock is not held or is another account's (a defect).
   */
  MockDesk(MockDeskKey key, const OperatorConfig& config, const OwnerLock& lock, const MarketData& data) : lock_(lock) {
    (void)key;
    const auto dir = account_dir(config);
    if (!lock.held() || !dir || lock.dir() != *dir) {
      throw std::logic_error("a mock desk needs its own account's lock, held");
    }
    std::vector<atl::BarSeries> series;
    std::vector<atl::Instrument> instruments;
    for (const auto& entry : data.bars_) {
      series.push_back(mock_series(entry.first, entry.second));
      atl::Instrument instrument;
      instrument.id_ = series.back().instrument_;
      instrument.name_ = entry.first;
      instrument.type_ = atl::SecurityType::STOCK;
      instrument.currency_ = atl::Currency::krw();
      instrument.lot_size_ = atl::Decimal64(1, 0);
      instrument.price_rule_ = atl::krx::equity_price_rule();
      instruments.push_back(std::move(instrument));
      for (const auto& bar : entry.second) {
        days_.push_back(bar.day_);
      }
    }
    std::sort(days_.begin(), days_.end());
    days_.erase(std::unique(days_.begin(), days_.end()), days_.end());
    atl::mock::MarketSettings settings;
    settings.cash_ = atl::Money{atl::Decimal64(config.paper_.cash_, 0), atl::Currency::krw()};
    settings.start_ = config.paper_.start_at_;
    file_ = std::make_unique<atl::mock::MarketFile>(
      lock.dir() + "/" + MOCK_STATE_FILE, std::move(series), std::move(settings), std::move(instruments)
    );
    client_ = std::make_shared<atl::mock::MockClient>(file_->market());
    market_session_ = std::make_unique<MarketSession>(client_, atl::mock::MockAccount{});
    auto* market = &file_->market();
    session_ = std::make_unique<Session>(
      *market_session_, placement_policy(config, lock),
      [market]() {
        return market->now();
      },
      nullptr
    );
  }

  MockDesk(const MockDesk&) = delete;
  MockDesk& operator=(const MockDesk&) = delete;
  MockDesk(MockDesk&&) = delete;
  MockDesk& operator=(MockDesk&&) = delete;

  /**
   * @brief Whether the state file opened and replayed.
   * @return Whether it did; a desk whose file did not open changes nothing.
   */
  bool opened() const noexcept {
    return file_->open();
  }

  /**
   * @brief Why the state file did not open.
   * @return The reason in the library's fixed words, or empty.
   */
  const std::string& rejection() const noexcept {
    return file_->rejection();
  }

  /**
   * @brief Why the market halted, if it did.
   * @return The reason, or nothing.
   */
  const std::optional<std::string>& halted() const noexcept {
    return file_->market().halted();
  }

  /**
   * @brief The trading session, borrowed.
   * @return The session.
   */
  Session& session() {
    return *session_;
  }

  /**
   * @brief The session quotes and the calendar are read from, borrowed.
   * @return The session.
   */
  MarketSession& market_session() {
    return *market_session_;
  }

  /**
   * @brief The market's clock.
   * @return The instant.
   */
  Clock::time_point now() const {
    return file_->market().now();
  }

  /** @brief The market's clock: the mock's figures and decisions share one clock. */
  Clock::time_point live_now() const {
    return now();
  }

  /**
   * @brief How many placements and cancellations this process sent towards the market, as the boundary counts them.
   * @return The count.
   */
  std::uint64_t sends() const noexcept {
    return session_->sends();
  }

  /**
   * @brief The cash after every fill and charge, before what open buys reserve, floored to the won.
   * @return The amount.
   */
  std::string paper_cash() const {
    const auto cash = file_->market().cash().amount_;
    return decimal_text_of(atl::decimal_floor_to_multiple(cash, atl::Decimal64(1, 0)).value_or(cash));
  }

  /**
   * @brief Whether the market has a symbol, by its reference data.
   * @param symbol The symbol.
   * @return Whether it does.
   */
  bool has_symbol(const std::string& symbol) {
    return market_session_->instrument(id_of(symbol)).has_value();
  }

  /**
   * @brief What the market shows for an order, from the working-order listing and then the order history.
   * @param id The order's id.
   * @return `pending`, `filled` or `canceled`, or nothing for an order in neither.
   */
  std::optional<std::string> order_status(const std::string& id) {
    if (const auto working = market_session_->working_orders()) {
      for (const auto& row : working.value().orders_) {
        if (row.order_.order_id_ == id) {
          return std::string("pending");
        }
      }
    }
    const auto history = market_session_->order_history(atl::OrderHistoryQuery{});
    if (!history) {
      return std::nullopt;
    }
    for (const auto& order : history.value().orders_) {
      if (order.order_id_ != id) {
        continue;
      }
      switch (order.status_) {
        case atl::OrderStatus::PENDING:
        case atl::OrderStatus::PARTIAL_FILLED:
          return std::string("pending");
        case atl::OrderStatus::FILLED:
          return std::string("filled");
        default:
          return std::string("canceled");
      }
    }
    return std::nullopt;
  }

  /**
   * @brief As `order_status(id)`: the mock reads its own listing and history, which costs nothing.
   * @tparam Working The working-order listing.
   * @param id The order's id.
   * @param working The working orders read by the command (unused).
   * @return As `order_status(id)`.
   */
  template <typename Working>
  std::optional<std::string> order_status(const std::string& id, const Working& working) {
    (void)working;
    return order_status(id);
  }

  /**
   * @brief Whether the market takes an order on a symbol at an instant: the calendar names the phase then, and the
   *   symbol has a last price.
   * @param symbol The symbol.
   * @param at The instant.
   * @return Whether it does.
   */
  bool accepts_orders_at(const std::string& symbol, Clock::time_point at) {
    const auto window = atl::strategy::SessionWindow::krx();
    const auto calendar = market_session_->calendar(atl::Country::KR);
    if (!calendar || !market_session_->quote(id_of(symbol))) {
      return false;
    }
    const auto trading = calendar.value().is_trading_day(window.local_date(at));
    return window.phase(at, trading) != atl::strategy::MarketPhase::UNKNOWN;
  }

  /**
   * @brief The last date any symbol trades.
   * @return The date, as days from 1970-01-01.
   */
  std::int64_t data_end() const {
    return days_.empty() ? FIRST_DAY : days_.back();
  }

  /**
   * @brief The latest instant the clock may reach: the last session's close plus a day, inside the supported interval.
   * @return The instant.
   */
  Clock::time_point clock_limit() const {
    const auto limit = open_of(data_end()) + SESSION_LENGTH + std::chrono::hours(24);
    return std::min(limit, end_instant() - Clock::duration(std::chrono::minutes(1)));
  }

  /**
   * @brief The open of the next session after an instant.
   * @param at The instant.
   * @return The open, or nothing past the data.
   */
  std::optional<Clock::time_point> next_session_after(Clock::time_point at) const {
    for (const auto day : days_) {
      if (at < open_of(day)) {
        return open_of(day);
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Moves the clock, after checking the lock: the move recorded and flushed, then made; what the market did to
   *   each order on the way is kept for `drain_events`.
   * @param target The new instant, after now and at most `clock_limit()`.
   * @return How it ended; the clock moved only when `OK`, or partway when `HALTED`.
   */
  Moved advance_to(Clock::time_point target) {
    if (!lock_.still_at_path()) {
      return Moved::LOCK_LOST;
    }
    auto& market = file_->market();
    const auto before = market.order_history(atl::OrderHistoryQuery{}).orders_;
    auto moved = Moved::OK;
    try {
      market.advance_to(target);
    } catch (const atl::mock::MarketFileError& error) {
      return error.kind() == atl::mock::MarketFileError::Kind::NOT_WRITTEN ? Moved::NOT_WRITTEN : Moved::UNCONFIRMED;
    } catch (const std::runtime_error&) {
      moved = Moved::HALTED;
    }
    const auto after = market.order_history(atl::OrderHistoryQuery{}).orders_;
    for (std::size_t k = 0; k < before.size() && k < after.size(); ++k) {
      const auto& order = after[k];
      if (before[k].status_ != atl::OrderStatus::PENDING || order.status_ == atl::OrderStatus::PENDING) {
        continue;
      }
      MockEvent event;
      event.order_id_ = order.order_id_;
      event.symbol_ = order.instrument_.symbol_;
      event.side_ = order.side_ == atl::OrderSide::BUY ? "buy" : "sell";
      const auto& done = order.execution_;
      if (order.status_ == atl::OrderStatus::FILLED && done.average_filled_price_) {
        event.kind_ = "filled";
        event.at_ = done.filled_at_.value_or(market.now());
        event.quantity_ = decimal_text_of(done.filled_quantity_);
        event.price_ = decimal_text_of(*done.average_filled_price_);
        event.commission_ = done.commission_ ? decimal_text_of(done.commission_->amount_) : "0";
        event.tax_ = done.tax_ ? decimal_text_of(done.tax_->amount_) : "0";
      } else {
        event.kind_ = "expired";
        event.at_ = order.canceled_at_.value_or(market.now());
        event.quantity_ = decimal_text_of(order.quantity_);
      }
      events_.push_back(std::move(event));
    }
    return moved;
  }

  /**
   * @brief Takes what the market did since the last call.
   * @return The events, in the order the orders were placed.
   */
  std::vector<MockEvent> drain_events() {
    return std::exchange(events_, {});
  }

private:
  /**
   * @brief A KRX symbol's instrument id.
   * @param symbol The symbol.
   * @return The id.
   */
  static atl::InstrumentId id_of(const std::string& symbol) {
    return atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol};
  }

  const OwnerLock& lock_;
  std::vector<std::int64_t> days_;
  std::unique_ptr<atl::mock::MarketFile> file_;
  std::shared_ptr<atl::mock::MockClient> client_;
  std::unique_ptr<MarketSession> market_session_;
  std::unique_ptr<Session> session_;
  std::vector<MockEvent> events_;
};

} // namespace cmdatl
