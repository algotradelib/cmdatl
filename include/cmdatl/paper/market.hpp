#pragma once

// A simulated KRX market over stored daily bars: nothing here reaches a network or an account.
//
// Daily bars only, limit and market orders only, every fill the whole remainder, orders refused outside KRX's
// acceptance hours (08:30-15:30 KST on a date the symbol trades).
//
// Time model (KST = UTC+9).
// A daily bar is the session of its date: 09:00-15:30 KST = 00:00-06:30 UTC.
// Within a session the price walks open -> (the nearer extreme) -> (the other extreme) -> close, piecewise linear, on
// the tick grid.
// Before the open the price is the previous close; after the close it is the close.
//
// The market is a pure function of its bars, its cash and the inputs it was given (clock advances, placements and
// cancellations): it reads no wall clock and no random number, so replaying the same inputs gives the same ids,
// fills and holdings.
// Its matching, costs, acceptance hours and rounding are the paper log's `market` version 1.

#include "./bars.hpp"
#include "./time.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace paper {

namespace atl = algotradelib;

/** @brief The largest amount an integer amount may hold: 2^53, below which every double is a whole number exactly. */
inline constexpr double MAX_AMOUNT = 9007199254740992.0;

/** @brief An amount of the paper account left the range cmdatl can state exactly. */
class OutOfRange : public std::runtime_error {
public:
  /** @brief Builds the error. */
  OutOfRange()
  : std::runtime_error("the paper account's amounts left the supported range; the operator moves its directory aside") {
  }
};

/**
 * @brief A whole amount from the market's arithmetic: finite and at most 2^53 in magnitude, or `OutOfRange`.
 * @param value The value, already rounded as the caller wants it.
 * @return The amount.
 * @throws OutOfRange When the value is not finite or too large.
 */
inline std::int64_t amount_of(double value) {
  if (!std::isfinite(value) || std::fabs(value) > MAX_AMOUNT) {
    throw OutOfRange();
  }
  return static_cast<std::int64_t>(value);
}

/**
 * @brief A whole amount as a decimal.
 * @param value The value, already rounded.
 * @return The decimal.
 * @throws OutOfRange When the value is not finite or too large.
 */
inline atl::Decimal64 decimal_of(double value) {
  return atl::Decimal64(amount_of(value), 0);
}

/** @brief One order the market holds. */
struct PaperOrder {
  /** @brief The market's id: `1`, `2`, ... in the order accepted. */
  std::string id_;
  /** @brief The request as placed. */
  atl::OrderRequest request_;
  /** @brief The quantity, whole shares. */
  std::int64_t quantity_{0};
  /** @brief The limit, for a limit order. */
  std::optional<double> limit_;
  /** @brief Its status. */
  atl::OrderStatus status_{atl::OrderStatus::PENDING};
  /** @brief Shares filled. */
  std::int64_t filled_{0};
  /** @brief The value filled. */
  double filled_value_{0};
  /** @brief The commission charged. */
  double commission_{0};
  /** @brief The tax charged. */
  double tax_{0};
  /** @brief When it was placed; moves to the next session's open for a DAY order placed outside a session. */
  Clock::time_point ordered_at_{};
  /** @brief When the market accepted it; never moves (the listing lag is measured from it). */
  Clock::time_point accepted_at_{};
  /** @brief When it filled, expired or was canceled. */
  std::optional<Clock::time_point> done_at_;
  /** @brief Each execution: when, and how many shares (so holdings can lag a fill). */
  std::vector<std::pair<Clock::time_point, std::int64_t>> fills_;
};

/** @brief One symbol's holding. */
struct Holding {
  /** @brief Shares held. */
  std::int64_t quantity_{0};
  /** @brief The total purchase value of the shares held, for the average price. */
  double cost_{0};
};

/** @brief What the market charges: fixed in `market` version 1. */
struct Costs {
  /** @brief The commission, a fraction of each fill's value, rounded to the won. */
  double commission_{0.00015};
  /** @brief The transaction tax on a sell, a fraction of its value, rounded to the won. */
  double sell_tax_{0.0020};
  /** @brief Ticks against a market order. */
  int slippage_ticks_{1};
};

/** @brief Something the market did while its clock moved. */
struct MarketEvent {
  /** @brief When. */
  Clock::time_point at_{};
  /** @brief `filled`, `expired` or `canceled`. */
  std::string kind_;
  /** @brief The order. */
  std::string order_id_;
  /** @brief Its symbol. */
  std::string symbol_;
  /** @brief `buy` or `sell`. */
  std::string side_;
  /** @brief The shares filled, or left when it expired or was canceled. */
  std::int64_t quantity_{0};
  /** @brief The fill's price. */
  std::optional<double> price_;
  /** @brief The fill's commission. */
  double commission_{0};
  /** @brief The fill's tax. */
  double tax_{0};
};

/** @brief The simulated market. */
class SimMarket {
public:
  /**
   * @brief Builds the market at its first instant.
   * @param data The bars.
   * @param cash The opening cash.
   * @param start The first instant.
   */
  SimMarket(MarketData data, double cash, Clock::time_point start)
  : bars_(std::move(data.bars_)), cash_(cash), now_(start) {}

  /**
   * @brief Whether an order is still open.
   * @param order The order.
   * @return Whether it is pending.
   */
  static bool is_open(const PaperOrder& order) {
    return order.status_ == atl::OrderStatus::PENDING || order.status_ == atl::OrderStatus::PARTIAL_FILLED;
  }

  /**
   * @brief The shares an order has left.
   * @param order The order.
   * @return The shares.
   */
  static std::int64_t remaining(const PaperOrder& order) {
    return order.quantity_ - order.filled_;
  }

  /**
   * @brief The market's clock.
   * @return The instant.
   */
  Clock::time_point now() const {
    return now_;
  }

  /**
   * @brief The cash after every fill's value, commission and tax.
   * @return The cash.
   */
  double cash() const {
    return cash_;
  }

  /**
   * @brief The holdings.
   * @return Each symbol's holding.
   */
  const std::map<std::string, Holding>& holdings() const {
    return holdings_;
  }

  /**
   * @brief Every order the market accepted.
   * @return The orders, in the order accepted.
   */
  const std::vector<PaperOrder>& orders() const {
    return orders_;
  }

  /**
   * @brief Takes what happened since the last call.
   * @return The events.
   */
  std::vector<MarketEvent> drain_events() {
    return std::exchange(events_, {});
  }

  /**
   * @brief Whether the bars have a symbol.
   * @param symbol The symbol.
   * @return Whether they do.
   */
  bool has_symbol(const std::string& symbol) const {
    return bars_.count(symbol) != 0;
  }

  /**
   * @brief Every symbol of the bars.
   * @return The bars by symbol.
   */
  const std::map<std::string, std::vector<DayBar>>& bars() const {
    return bars_;
  }

  /**
   * @brief The last date any symbol trades, as days from 1970-01-01.
   * @return The date.
   */
  std::int64_t last_day() const {
    std::int64_t last = FIRST_DAY;
    for (const auto& entry : bars_) {
      if (!entry.second.empty()) {
        last = std::max(last, entry.second.back().day_);
      }
    }
    return last;
  }

  /**
   * @brief The first date any symbol trades, as days from 1970-01-01.
   * @return The date.
   */
  std::int64_t first_day() const {
    std::int64_t first = LAST_DAY;
    for (const auto& entry : bars_) {
      if (!entry.second.empty()) {
        first = std::min(first, entry.second.front().day_);
      }
    }
    return first;
  }

  /**
   * @brief The open of the first session after an instant: the 00:00 UTC of the next date on which any symbol has a
   *   bar.
   * @param at The instant.
   * @return The open, or nothing when no later session is stored.
   */
  std::optional<Clock::time_point> next_open_after(Clock::time_point at) const {
    std::optional<std::int64_t> best;
    for (const auto& entry : bars_) {
      const auto& series = entry.second;
      const auto found = std::upper_bound(series.begin(), series.end(), at, [](Clock::time_point t, const DayBar& bar) {
        return t < open_of(bar.day_);
      });
      if (found != series.end() && (!best || found->day_ < *best)) {
        best = found->day_;
      }
    }
    if (!best) {
      return std::nullopt;
    }
    return open_of(*best);
  }

  /**
   * @brief The bar of the session on an instant's UTC date, if that date traded.
   * @param symbol The symbol.
   * @param at The instant.
   * @return The bar, or null.
   */
  const DayBar* session_bar(const std::string& symbol, Clock::time_point at) const {
    const auto series = bars_.find(symbol);
    if (series == bars_.end()) {
      return nullptr;
    }
    const auto day = utc_day(at);
    const auto& bars = series->second;
    const auto found = std::lower_bound(bars.begin(), bars.end(), day, [](const DayBar& bar, std::int64_t d) {
      return bar.day_ < d;
    });
    return found != bars.end() && found->day_ == day ? &*found : nullptr;
  }

  /**
   * @brief Whether any symbol trades on an instant's UTC date.
   * @param at The instant.
   * @return Whether one does.
   */
  bool trades_on(Clock::time_point at) const {
    return std::any_of(bars_.begin(), bars_.end(), [&](const auto& entry) {
      return session_bar(entry.first, at) != nullptr;
    });
  }

  /**
   * @brief The last bar whose session has closed by an instant.
   * @param symbol The symbol.
   * @param at The instant.
   * @return The bar, or null.
   */
  const DayBar* last_closed(const std::string& symbol, Clock::time_point at) const {
    const auto series = bars_.find(symbol);
    if (series == bars_.end()) {
      return nullptr;
    }
    const auto& bars = series->second;
    // The first bar whose session is still running or ahead at `at`; the one before it has closed.
    const auto found = std::partition_point(bars.begin(), bars.end(), [&](const DayBar& bar) {
      return open_of(bar.day_) + SESSION_LENGTH <= at;
    });
    return found == bars.begin() ? nullptr : &*(found - 1);
  }

  /**
   * @brief Whether a symbol's session is running at an instant.
   * @param symbol The symbol.
   * @param at The instant.
   * @return Whether it is.
   */
  bool in_session(const std::string& symbol, Clock::time_point at) const {
    const auto* bar = session_bar(symbol, at);
    return bar != nullptr && open_of(bar->day_) <= at && at < open_of(bar->day_) + SESSION_LENGTH;
  }

  /**
   * @brief Whether the market takes an order for a symbol at an instant: 08:30-15:30 KST on a date it trades.
   * @param symbol The symbol.
   * @param at The instant.
   * @return Whether it does.
   */
  bool accepts_orders_at(const std::string& symbol, Clock::time_point at) const {
    // UTC; 08:30 KST is 23:30 UTC of the day before.
    const auto into_day = at - day_start(at);
    const bool morning = into_day >= std::chrono::hours(23) + std::chrono::minutes(30);
    const auto* bar = session_bar(symbol, morning ? at + std::chrono::hours(1) : at);
    return bar != nullptr && (morning || into_day < SESSION_LENGTH);
  }

  /**
   * @brief The traded price at an instant, on the path through the session's bar.
   * @param symbol The symbol.
   * @param at The instant.
   * @return The price, or nothing before the symbol's first session.
   */
  std::optional<double> price_at(const std::string& symbol, Clock::time_point at) const {
    if (const auto* bar = session_bar(symbol, at); bar != nullptr && at >= open_of(bar->day_)) {
      const double f = std::min(
        1.0, std::chrono::duration<double>(at - open_of(bar->day_)) / std::chrono::duration<double>(SESSION_LENGTH)
      );
      const double o = bar->open_;
      const double h = bar->high_;
      const double l = bar->low_;
      const double c = bar->close_;
      const bool low_first = (o - l) < (h - o);
      const double p1 = low_first ? l : h;
      const double p2 = low_first ? h : l;
      double p = 0;
      if (f < 1.0 / 3) {
        p = o + (p1 - o) * (f * 3);
      } else if (f < 2.0 / 3) {
        p = p1 + (p2 - p1) * ((f - 1.0 / 3) * 3);
      } else {
        p = p2 + (c - p2) * ((f - 2.0 / 3) * 3);
      }
      return on_grid(p);
    }
    if (const auto* bar = last_closed(symbol, at)) {
      return bar->close_;
    }
    return std::nullopt;
  }

  /**
   * @brief KRX's band: +-30 % of the previous close, rounded inward to the tick.
   * @param symbol The symbol.
   * @param at The instant.
   * @return The upper and lower limits, or nothing while the symbol has no previous close.
   */
  std::optional<std::pair<double, double>> band(const std::string& symbol, Clock::time_point at) const {
    const auto* prev = last_closed(symbol, in_session(symbol, at) ? day_start(at) : at);
    if (prev == nullptr) {
      return std::nullopt;
    }
    const double c = prev->close_;
    const auto rule = atl::PriceRule::krx_equity();
    const auto up = rule.round_to_tick(decimal_of(std::round(c * 1.3)), atl::OrderSide::SELL);
    const auto down = rule.round_to_tick(decimal_of(std::ceil(c * 0.7)), atl::OrderSide::BUY);
    return std::make_pair(up ? to_double(*up) : c * 1.3, down ? to_double(*down) : c * 0.7);
  }

  /**
   * @brief Places an order the caller has already checked by the shared input rules (a symbol in the bars, a limit
   *   or market order, a whole quantity, a limit with a price): the market's own refusals remain.
   * @param request The request.
   * @param quantity Its quantity, whole shares.
   * @param limit Its limit, for a limit order.
   * @param error Set to why it was refused.
   * @return The new order's id, or empty when refused.
   */
  std::string place(
    const atl::OrderRequest& request, std::int64_t quantity, std::optional<double> limit, std::string& error
  ) {
    const auto& symbol = request.instrument_.symbol_;
    if (!accepts_orders_at(symbol, now_)) {
      error = "not an order-acceptance time";
      return {};
    }
    if (limit) {
      if (const auto b = band(symbol, now_)) {
        if (*limit > b->first || *limit < b->second) {
          error = "price outside the daily band";
          return {};
        }
      }
    }
    const auto found = holdings_.find(symbol);
    const std::int64_t held = found == holdings_.end() ? 0 : found->second.quantity_;
    if (request.side_ == atl::OrderSide::SELL) {
      if (quantity > held - resting(symbol, atl::OrderSide::SELL)) {
        error = "not enough sellable quantity";
        return {};
      }
    } else {
      if (held + resting(symbol, atl::OrderSide::BUY) + quantity > MAX_SHARES) {
        error = "the holding would pass " + std::to_string(MAX_SHARES) + " shares";
        return {};
      }
      // A market buy is reserved at the upper band, as KRX brokers do; a limit at its price.
      const double reserve = limit ? *limit : reserve_price(symbol);
      const double needed = static_cast<double>(quantity) * reserve * (1 + costs_.commission_);
      if (needed > cash_ - reserved_buys() + 1e-6) {
        error = "insufficient buying power";
        return {};
      }
    }
    PaperOrder order;
    order.id_ = std::to_string(++next_id_);
    order.request_ = request;
    order.quantity_ = quantity;
    order.limit_ = limit;
    order.ordered_at_ = now_;
    order.accepted_at_ = now_;
    orders_.push_back(std::move(order));
    return orders_.back().id_;
  }

  /**
   * @brief Cancels an open order.
   * @param id The order's id.
   * @param error Set to why it was refused.
   * @return Whether it was canceled.
   */
  bool cancel(const std::string& id, std::string& error) {
    for (auto& order : orders_) {
      if (order.id_ != id) {
        continue;
      }
      if (!is_open(order)) {
        error = "order is not open";
        return false;
      }
      order.status_ = atl::OrderStatus::CANCELED;
      order.done_at_ = now_;
      events_.push_back(event_of(order, "canceled", now_, remaining(order)));
      return true;
    }
    error = "no such order";
    return false;
  }

  /**
   * @brief Moves the clock forward, matching open orders in five-minute steps; with nothing open it moves at once.
   * @param target The new instant, not before now.
   */
  void advance_to(Clock::time_point target) {
    const auto open_order = [](const PaperOrder& order) {
      return is_open(order);
    };
    while (now_ < target) {
      if (std::none_of(orders_.begin(), orders_.end(), open_order)) {
        now_ = target;
        return;
      }
      const auto step = std::min(target, now_ + Clock::duration(std::chrono::minutes(5)));
      match(step);
      now_ = step;
    }
  }

private:
  /**
   * @brief A decimal as a double.
   * @param value The decimal.
   * @return The double.
   */
  static double to_double(const atl::Decimal64& value) {
    return static_cast<double>(value.coefficient()) / std::pow(10.0, value.scale());
  }

  /**
   * @brief A price on the KRX tick grid.
   * @param p The price.
   * @return The price rounded to the tick below.
   */
  static double on_grid(double p) {
    const auto rounded = atl::PriceRule::krx_equity().round_to_tick(decimal_of(std::round(p)), atl::OrderSide::BUY);
    return rounded ? to_double(*rounded) : std::round(p);
  }

  /**
   * @brief An order's side as a word.
   * @param order The order.
   * @return `buy` or `sell`.
   */
  static std::string side_of(const PaperOrder& order) {
    return order.request_.side_ == atl::OrderSide::BUY ? "buy" : "sell";
  }

  /**
   * @brief An event about an order.
   * @param order The order.
   * @param kind The event.
   * @param at When.
   * @param quantity The shares.
   * @return The event.
   */
  static MarketEvent event_of(const PaperOrder& order, const char* kind, Clock::time_point at, std::int64_t quantity) {
    MarketEvent event;
    event.at_ = at;
    event.kind_ = kind;
    event.order_id_ = order.id_;
    event.symbol_ = order.request_.instrument_.symbol_;
    event.side_ = side_of(order);
    event.quantity_ = quantity;
    return event;
  }

  /**
   * @brief The price a market buy is reserved at: the upper band, else the price, else 0.
   * @param symbol The symbol.
   * @return The price.
   */
  double reserve_price(const std::string& symbol) const {
    if (const auto b = band(symbol, now_)) {
      return b->first;
    }
    return price_at(symbol, now_).value_or(0);
  }

  /**
   * @brief The shares resting in open orders of one side.
   * @param symbol The symbol.
   * @param side The side.
   * @return The shares.
   */
  std::int64_t resting(const std::string& symbol, atl::OrderSide side) const {
    std::int64_t total = 0;
    for (const auto& order : orders_) {
      if (is_open(order) && order.request_.side_ == side && order.request_.instrument_.symbol_ == symbol) {
        total += remaining(order);
      }
    }
    return total;
  }

  /**
   * @brief The cash open buys reserve.
   * @return The cash.
   */
  double reserved_buys() const {
    double total = 0;
    for (const auto& order : orders_) {
      if (!is_open(order) || order.request_.side_ != atl::OrderSide::BUY) {
        continue;
      }
      const double p = order.limit_ ? *order.limit_ : reserve_price(order.request_.instrument_.symbol_);
      total += static_cast<double>(remaining(order)) * p * (1 + costs_.commission_);
    }
    return total;
  }

  /**
   * @brief Matches every open order at an instant: a DAY order expires at its session's close, a market order fills
   *   one tick against it, a limit when the price reaches it; a fill is the whole remainder.
   * @param t The instant.
   */
  void match(Clock::time_point t) {
    const auto rule = atl::PriceRule::krx_equity();
    for (auto& order : orders_) {
      if (!is_open(order)) {
        continue;
      }
      const auto& symbol = order.request_.instrument_.symbol_;
      const auto* bar = session_bar(symbol, t);
      const bool open_now = bar != nullptr && open_of(bar->day_) <= t && t <= open_of(bar->day_) + SESSION_LENGTH;
      // A DAY order placed during a session expires at that session's close; one placed outside a session waits for
      // the next one.
      const auto* placed = session_bar(symbol, order.ordered_at_);
      const bool placed_in_session = placed != nullptr && open_of(placed->day_) <= order.ordered_at_ &&
        order.ordered_at_ < open_of(placed->day_) + SESSION_LENGTH;
      if (placed_in_session && t >= open_of(placed->day_) + SESSION_LENGTH) {
        const auto expiry = open_of(placed->day_) + SESSION_LENGTH;
        order.status_ = atl::OrderStatus::CANCELED;
        order.done_at_ = expiry;
        events_.push_back(event_of(order, "expired", expiry, remaining(order)));
        continue;
      }
      if (!placed_in_session && open_now) {
        // Carried to this session; it now expires at this close.
        order.ordered_at_ = std::max(order.ordered_at_, open_of(bar->day_));
      }
      if (!open_now) {
        continue;
      }
      const auto price = price_at(symbol, std::max(t, open_of(bar->day_)));
      if (!price) {
        continue;
      }
      const bool buy = order.request_.side_ == atl::OrderSide::BUY;
      double fill = *price;
      if (!order.limit_) {
        // One tick against the order per slippage tick: a buy rounded up, a sell down.
        for (int k = 0; k < costs_.slippage_ticks_; ++k) {
          const auto nudged =
            rule.round_to_tick(decimal_of(buy ? fill + 1 : fill - 1), buy ? atl::OrderSide::SELL : atl::OrderSide::BUY);
          if (nudged) {
            fill = to_double(*nudged);
          }
        }
      } else {
        const double limit = *order.limit_;
        if (buy ? *price > limit : *price < limit) {
          continue;
        }
        fill = buy ? std::min(*price, limit) : std::max(*price, limit);
      }
      const std::int64_t q = remaining(order);
      const double value = static_cast<double>(q) * fill;
      const double commission = std::round(value * costs_.commission_);
      const double tax = buy ? 0 : std::round(value * costs_.sell_tax_);
      auto& holding = holdings_[symbol];
      if (buy) {
        cash_ -= value + commission;
        holding.quantity_ += q;
        holding.cost_ += value;
      } else {
        const auto before = static_cast<double>(holding.quantity_);
        cash_ += value - commission - tax;
        holding.cost_ = before > 0 ? holding.cost_ * (before - static_cast<double>(q)) / before : 0;
        holding.quantity_ -= q;
      }
      order.filled_ += q;
      order.fills_.emplace_back(t, q);
      order.filled_value_ += value;
      order.commission_ += commission;
      order.tax_ += tax;
      order.status_ = atl::OrderStatus::FILLED;
      order.done_at_ = t;
      auto event = event_of(order, "filled", t, q);
      event.price_ = fill;
      event.commission_ = commission;
      event.tax_ = tax;
      events_.push_back(std::move(event));
    }
  }

  std::map<std::string, std::vector<DayBar>> bars_;
  Costs costs_;
  double cash_;
  Clock::time_point now_;
  std::map<std::string, Holding> holdings_;
  std::vector<PaperOrder> orders_;
  std::vector<MarketEvent> events_;
  std::int64_t next_id_{0};
};

} // namespace paper
} // namespace cmdatl
