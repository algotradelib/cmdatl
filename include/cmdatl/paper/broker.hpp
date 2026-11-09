#pragma once

// The paper market as a broker: a blocking client and its BrokerTraits, written outside the library the way a third
// party adds a broker.
// Bar queries are refused (nothing in cmdatl reads them yet), and orders carry no trigger price.

#include "./krx_grid.hpp"
#include "./market.hpp"

#include <algotradelib/broker>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cmdatl {
namespace paper {

/** @brief The paper broker's configuration: nothing to configure. */
struct PaperConfig {
  /** @brief A label. */
  std::string label_{"paper"};
};

/** @brief The paper account: its number, from the operator's file. */
struct PaperAccount {
  /** @brief The number. */
  std::string id_;
};

/** @brief The market's acknowledgement of a placement or a cancellation. */
struct PaperAck {
  /** @brief The order's id. */
  std::string id_;
  /** @brief When. */
  Clock::time_point at_;
};

/** @brief A cancellation or an amendment, by the order's id. */
struct PaperAmendment {
  /** @brief The order's id. */
  std::string id_;
  /** @brief A new quantity, for an amendment. */
  std::optional<atl::Decimal64> quantity_;
  /** @brief A new price, for an amendment. */
  std::optional<atl::Decimal64> price_;
};

/** @brief One open order as the listing shows it. */
struct PaperOpenOrder {
  /** @brief The order. */
  atl::OrderSnapshot order_;
  /** @brief Its id. */
  std::string id_;
  /** @brief The shares still open. */
  atl::Decimal64 open_quantity_;
};

/** @brief The working-order listing. */
struct PaperOpenOrders {
  /** @brief The open orders. */
  std::vector<PaperOpenOrder> rows_;
  /** @brief When it was read. */
  Clock::time_point read_at_;
};

/** @brief An order-history query: a date range only. */
struct PaperHistoryQuery {
  /** @brief The first date. */
  std::optional<atl::Date> from_;
  /** @brief The last date. */
  std::optional<atl::Date> to_;
};

/** @brief The paper broker's tag. */
struct Broker {
  /** @brief Its name. */
  static constexpr std::string_view name = "paper";
};

/**
 * @brief A refusal by the paper broker.
 * @param code The broker's code.
 * @param message Why.
 * @param reached Whether the market saw the request.
 * @return The error.
 */
inline atl::BrokerError refusal(std::string code, std::string message, bool reached = true) {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = reached;
  error.broker_code_ = std::move(code);
  error.message_ = std::move(message);
  return error;
}

/**
 * @brief A KRX instrument as the paper broker names it.
 * @param symbol The symbol.
 * @return The instrument.
 */
inline atl::InstrumentId kr(const std::string& symbol) {
  return {atl::Country::KR, atl::Venue::COMBINED, symbol};
}

/** @brief The blocking client over the market. */
class PaperClient {
public:
  /**
   * @brief Builds the client over a shared market.
   * @param market The market.
   */
  explicit PaperClient(std::shared_ptr<SimMarket> market) : market_(std::move(market)) {}

  /**
   * @brief The market.
   * @return It.
   */
  SimMarket& market() {
    return *market_;
  }

  /**
   * @brief The market.
   * @return It.
   */
  const SimMarket& market() const {
    return *market_;
  }

  /** @brief How long a fill stays out of the holdings answer (the operator's `holdings_lag`). */
  Clock::duration holdings_lag_{};
  /** @brief How long an accepted order stays out of the working-order listing (the operator's `listing_lag`). */
  Clock::duration listing_lag_{};

  /**
   * @brief Bars: not answered by the paper broker in this version.
   * @param query The query.
   * @return The refusal.
   */
  atl::Result<atl::BarSeries> bars(const atl::BarQuery& query) {
    (void)query;
    return refusal("bars_unsupported", "the paper broker answers no bar query in this version", false);
  }

  /**
   * @brief The last price.
   * @param id The instrument.
   * @return The quote, or `no_price` before the symbol's first session.
   */
  atl::Result<atl::QuoteSnapshot> quote(const atl::InstrumentId& id) {
    const auto price = market_->price_at(id.symbol_, market_->now());
    if (!price) {
      return refusal("no_price", "no price yet for " + id.symbol_);
    }
    atl::QuoteSnapshot q;
    q.instrument_ = id;
    q.last_price_ = decimal_of(std::floor(*price));
    q.currency_ = atl::Currency{"KRW"};
    q.exchange_time_ = market_->now();
    q.received_at_ = market_->now();
    return q;
  }

  /**
   * @brief Each symbol's quantity change from fills newer than the holdings lag, to be taken back out.
   * @return The changes.
   */
  std::map<std::string, std::int64_t> unreported_fills() const {
    std::map<std::string, std::int64_t> out;
    if (holdings_lag_ == Clock::duration::zero()) {
      return out;
    }
    for (const auto& order : market_->orders()) {
      for (const auto& fill : order.fills_) {
        if (fill.first + holdings_lag_ <= market_->now()) {
          continue;
        }
        out[order.request_.instrument_.symbol_] +=
          order.request_.side_ == atl::OrderSide::BUY ? fill.second : -fill.second;
      }
    }
    return out;
  }

  /**
   * @brief The holdings, as they stood the holdings lag ago.
   * @param account The account.
   * @return The holdings.
   * @throws OutOfRange When an amount leaves the supported range.
   */
  atl::Result<atl::HoldingsSnapshot> holdings(const PaperAccount& account) {
    (void)account;
    const auto unreported = unreported_fills();
    atl::HoldingsSnapshot snapshot;
    snapshot.received_at_ = market_->now();
    snapshot.sellable_basis_.coverage_ = atl::SellableBasis::Coverage::WHOLE_HOLDING;
    snapshot.sellable_basis_.resting_ = atl::SellableBasis::RestingTreatment::DEDUCT;
    snapshot.sellable_basis_.verified_ = true;
    double total_cost = 0;
    double total_value = 0;
    for (const auto& entry : market_->holdings()) {
      const auto& symbol = entry.first;
      const auto& holding = entry.second;
      const auto late = unreported.count(symbol) != 0 ? unreported.at(symbol) : 0;
      const std::int64_t reported = holding.quantity_ - late;
      if (reported == 0) {
        continue;
      }
      atl::HoldingPosition p;
      p.instrument_ = kr(symbol);
      p.name_ = symbol;
      p.quantity_ = atl::Decimal64(reported, 0);
      std::int64_t resting = 0;
      for (const auto& order : market_->orders()) {
        if (
          SimMarket::is_open(order) && order.request_.side_ == atl::OrderSide::SELL &&
          order.request_.instrument_.symbol_ == symbol
        ) {
          resting += SimMarket::remaining(order);
        }
      }
      p.sellable_quantity_ = atl::Decimal64(std::max<std::int64_t>(0, reported - resting), 0);
      // Cost and value at the reported quantity: a lagging answer is consistent with itself.
      const auto q = static_cast<double>(reported);
      const double last = std::floor(market_->price_at(symbol, market_->now()).value_or(0));
      const auto held = static_cast<double>(holding.quantity_);
      const double cost = held > 0 ? holding.cost_ * q / held : 0;
      p.average_purchase_price_ = decimal_of(std::round(q > 0 ? cost / q : 0));
      p.last_price_ = decimal_of(last);
      p.purchase_value_ = atl::Money{decimal_of(std::round(cost)), atl::Currency{"KRW"}};
      p.market_value_ = atl::Money{decimal_of(std::round(q * last)), atl::Currency{"KRW"}};
      p.unrealized_profit_loss_ = atl::Money{decimal_of(std::round(q * last - cost)), atl::Currency{"KRW"}};
      total_cost += cost;
      total_value += q * last;
      snapshot.positions_.push_back(std::move(p));
    }
    atl::HoldingsTotals totals;
    totals.currency_ = atl::Currency{"KRW"};
    totals.purchase_value_ = decimal_of(std::round(total_cost));
    totals.market_value_ = decimal_of(std::round(total_value));
    totals.unrealized_profit_loss_ = decimal_of(std::round(total_value - total_cost));
    snapshot.totals_.push_back(totals);
    return snapshot;
  }

  /**
   * @brief The buying power: the cash, floored (resting buys are not reserved in it).
   * @param account The account.
   * @param currency The currency.
   * @return The buying power.
   * @throws OutOfRange When the cash leaves the supported range.
   */
  atl::Result<atl::BuyingPower> buying_power(const PaperAccount& account, const atl::Currency& currency) {
    (void)account;
    atl::BuyingPower power;
    power.cash_ = atl::Money{decimal_of(std::floor(market_->cash())), currency};
    power.received_at_ = market_->now();
    return power;
  }

  /**
   * @brief Places an order the logged client has checked by the shared input rules.
   * @param account The account.
   * @param request The request.
   * @param quantity Its quantity, whole shares.
   * @param limit Its limit, for a limit order.
   * @return The acknowledgement, or the market's refusal.
   */
  atl::Result<PaperAck> place_checked(
    const PaperAccount& account, const atl::OrderRequest& request, std::int64_t quantity, std::optional<double> limit
  ) {
    (void)account;
    std::string error;
    const auto id = market_->place(request, quantity, limit, error);
    if (id.empty()) {
      return refusal("refused", error);
    }
    return PaperAck{id, market_->now()};
  }

  /**
   * @brief Cancels an order by its id.
   * @param account The account.
   * @param amendment The order.
   * @return The acknowledgement, or the market's refusal.
   */
  atl::Result<PaperAck> cancel(const PaperAccount& account, const PaperAmendment& amendment) {
    (void)account;
    std::string error;
    if (!market_->cancel(amendment.id_, error)) {
      return refusal("cancel_refused", error);
    }
    return PaperAck{amendment.id_, market_->now()};
  }

  /**
   * @brief Amends an order: refused, the paper broker does not amend.
   * @param account The account.
   * @param amendment The amendment.
   * @return The refusal.
   */
  atl::Result<PaperAck> replace(const PaperAccount& account, const PaperAmendment& amendment) {
    (void)account;
    (void)amendment;
    return refusal("replace_unsupported", "the paper broker does not amend; cancel and place");
  }

  /**
   * @brief The working-order listing, without orders younger than the listing lag.
   * @param account The account.
   * @return The listing.
   */
  atl::Result<PaperOpenOrders> open_orders(const PaperAccount& account) {
    (void)account;
    PaperOpenOrders out;
    out.read_at_ = market_->now();
    for (const auto& order : market_->orders()) {
      if (!SimMarket::is_open(order) || market_->now() < order.accepted_at_ + listing_lag_) {
        continue;
      }
      out.rows_.push_back({snapshot(order), order.id_, atl::Decimal64(SimMarket::remaining(order), 0)});
    }
    return out;
  }

  /**
   * @brief The order history in a date range.
   * @param account The account.
   * @param query The range.
   * @return The orders placed on those dates.
   */
  atl::Result<atl::OrderPage> history(const PaperAccount& account, const PaperHistoryQuery& query) {
    (void)account;
    atl::OrderPage page;
    page.received_at_ = market_->now();
    for (const auto& order : market_->orders()) {
      const auto civil = civil_from_days(utc_day(order.ordered_at_));
      const atl::Date day{static_cast<int>(civil.year_), civil.month_, civil.day_};
      if ((query.from_ && day < *query.from_) || (query.to_ && *query.to_ < day)) {
        continue;
      }
      page.orders_.push_back(snapshot(order));
    }
    return page;
  }

  /**
   * @brief An instrument's reference data.
   * @param id The instrument.
   * @return The instrument.
   */
  atl::Result<atl::Instrument> instrument(const atl::InstrumentId& id) {
    atl::Instrument instrument;
    instrument.id_ = id;
    instrument.name_ = id.symbol_;
    instrument.type_ = atl::SecurityType::STOCK;
    instrument.currency_ = atl::Currency{"KRW"};
    instrument.lot_size_ = atl::Decimal64(1, 0);
    instrument.price_rule_ = krx_equity_rule();
    return instrument;
  }

  /**
   * @brief Thirty days of the calendar from a date (or today): open where any symbol has a session that day.
   * @param from The first date.
   * @return The calendar.
   */
  atl::Result<atl::TradingCalendar> calendar(std::optional<atl::Date> from) {
    atl::TradingCalendar calendar;
    calendar.country_ = atl::Country::KR;
    calendar.received_at_ = market_->now();
    auto start = utc_day(market_->now());
    if (from) {
      start = days_from_civil(from->year_, from->month_, from->day_);
    }
    for (std::int64_t k = 0; k < 30; ++k) {
      const auto civil = civil_from_days(start + k);
      const atl::Date day{static_cast<int>(civil.year_), civil.month_, civil.day_};
      calendar.days_.push_back(atl::TradingDay{day, market_->trades_on(open_of(start + k))});
    }
    return calendar;
  }

  /**
   * @brief The day's price band.
   * @param id The instrument.
   * @return The band, or `no_band` while the symbol has no previous close.
   */
  atl::Result<atl::PriceLimits> price_limits(const atl::InstrumentId& id) {
    const auto b = market_->band(id.symbol_, market_->now());
    if (!b) {
      return refusal("no_band", "no previous close yet");
    }
    atl::PriceLimits limits;
    limits.currency_ = atl::Currency{"KRW"};
    limits.upper_ = decimal_of(std::floor(b->first));
    limits.lower_ = decimal_of(std::ceil(b->second));
    limits.exchange_time_ = market_->now();
    return limits;
  }

private:
  /**
   * @brief An order as the library's snapshot.
   * @param order The order.
   * @return The snapshot.
   */
  atl::OrderSnapshot snapshot(const PaperOrder& order) const {
    atl::OrderSnapshot s;
    s.order_id_ = order.id_;
    s.instrument_ = order.request_.instrument_;
    s.side_ = order.request_.side_;
    s.type_ = order.request_.type_;
    s.time_in_force_ = order.request_.time_in_force_;
    s.session_ = atl::OrderSession::REGULAR;
    s.venue_ = order.request_.venue_;
    s.status_ = order.status_;
    s.price_ = order.request_.price_;
    s.quantity_ = atl::Decimal64(order.quantity_, 0);
    s.currency_ = atl::Currency{"KRW"};
    s.ordered_at_ = order.ordered_at_;
    if (order.status_ == atl::OrderStatus::CANCELED) {
      s.canceled_at_ = order.done_at_;
    }
    s.execution_.filled_quantity_ = atl::Decimal64(order.filled_, 0);
    if (order.filled_ != 0) {
      s.execution_.average_filled_price_ =
        decimal_of(std::round(order.filled_value_ / static_cast<double>(order.filled_)));
      s.execution_.filled_amount_ = atl::Money{decimal_of(std::round(order.filled_value_)), atl::Currency{"KRW"}};
      s.execution_.commission_ = atl::Money{decimal_of(order.commission_), atl::Currency{"KRW"}};
      s.execution_.tax_ = atl::Money{decimal_of(order.tax_), atl::Currency{"KRW"}};
      s.execution_.filled_at_ = order.done_at_;
    }
    return s;
  }

  std::shared_ptr<SimMarket> market_;
};

} // namespace paper
} // namespace cmdatl

namespace algotradelib {

/** @brief The paper broker's traits: what the session reads of it. */
template <>
struct BrokerTraits<cmdatl::paper::PaperClient> {
  /** @brief The client. */
  using Client = cmdatl::paper::PaperClient;
  /** @brief Its configuration. */
  using Config = cmdatl::paper::PaperConfig;
  /** @brief Its account. */
  using Account = cmdatl::paper::PaperAccount;
  /** @brief Its tag. */
  using BrokerTag = cmdatl::paper::Broker;
  /** @brief A placement's acknowledgement. */
  using PlaceAck = cmdatl::paper::PaperAck;
  /** @brief A cancellation or an amendment. */
  using Amendment = cmdatl::paper::PaperAmendment;
  /** @brief An order-history query. */
  using HistoryQuery = cmdatl::paper::PaperHistoryQuery;
  /** @brief The working-order listing. */
  using WorkingOrdersResult = cmdatl::paper::PaperOpenOrders;

  /** @brief Its name. */
  static constexpr std::string_view name = cmdatl::paper::Broker::name;
  /** @brief Its calls block. */
  static constexpr bool blocking = true;

  /** @brief No account listing. */
  static constexpr bool has_account_discovery = false;
  /** @brief No idempotency key. */
  static constexpr bool has_client_order_id = false;
  /** @brief No trigger price: no stop-limit orders. */
  static constexpr bool has_trigger_price = false;
  /** @brief It trades. */
  static constexpr bool data_only = false;
  /** @brief It lists working orders. */
  static constexpr bool has_working_orders = true;
  /** @brief It reports the amendable quantity. */
  static constexpr bool has_amendable_quantity = true;
  /** @brief Its listing shows open orders only. */
  static constexpr bool working_orders_state_terminal = false;
  /** @brief No order inquiry by id. */
  static constexpr bool has_order_by_id = false;
  /** @brief No partial cancellation. */
  static constexpr bool has_partial_cancel = false;
  /** @brief The history takes no range requirement. */
  static constexpr bool history_requires_range = false;
  /** @brief No history cursor. */
  static constexpr bool has_history_cursor = false;
  /** @brief No history limit. */
  static constexpr bool has_history_limit = false;
  /** @brief No history symbol filter. */
  static constexpr bool has_history_symbol_filter = false;

  /** @brief It reports buying power. */
  static constexpr bool has_buying_power = true;
  /** @brief Not as a quantity. */
  static constexpr bool has_buying_power_quantity = false;
  /** @brief Sellable shares come from holdings. */
  static constexpr SellableSource sellable_source = SellableSource::HOLDINGS;
  /** @brief No commission schedule. */
  static constexpr bool has_commission_schedule = false;
  /** @brief No depth. */
  static constexpr bool has_depth = false;
  /** @brief It reports the price band. */
  static constexpr bool has_price_limits = true;
  /** @brief No trade prints. */
  static constexpr bool has_trade_prints = false;
  /** @brief No batch prices. */
  static constexpr bool has_batch_prices = false;
  /** @brief It reports reference data. */
  static constexpr bool has_reference_data = true;
  /** @brief Not in batches. */
  static constexpr bool has_batch_reference_data = false;
  /** @brief It reports a calendar. */
  static constexpr bool has_calendar = true;
  /** @brief Not over a range. */
  static constexpr bool has_calendar_range = false;
  /** @brief Without sessions. */
  static constexpr bool has_calendar_sessions = false;

  /**
   * @brief Whether it answers bars of an interval: none in this version.
   * @param interval The interval.
   * @return False.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    (void)interval;
    return false;
  }

  /**
   * @brief Refuses every bar query, before the market.
   * @param query The query.
   * @return The refusal.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    (void)query;
    return cmdatl::paper::refusal("bars_unsupported", "the paper broker answers no bar query in this version", false);
  }

  /**
   * @brief Accepts the Korean calendar only.
   * @param country The country.
   * @param from The first date.
   * @return The refusal, or nothing.
   */
  static std::optional<BrokerError> validate_calendar(Country country, std::optional<Date> from) {
    (void)from;
    if (country == Country::KR) {
      return std::nullopt;
    }
    return cmdatl::paper::refusal("unsupported_market", "the paper broker has a Korean calendar only", false);
  }

  /**
   * @brief An order reference from the market's id.
   * @param id The id.
   * @return The reference.
   */
  static OrderRef<cmdatl::paper::Broker> make_ref(std::string id) {
    OrderRef<cmdatl::paper::Broker> ref;
    ref.primary_ = std::move(id);
    return ref;
  }

  /**
   * @brief Refuses an idempotency key, before the market.
   * @param order The order.
   * @return The refusal, or nothing.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    if (order.client_order_id_) {
      return cmdatl::paper::refusal("idempotency_key_unsupported", "no client order id", false);
    }
    return std::nullopt;
  }

  /**
   * @brief A placement's acknowledgement as the library's.
   * @param raw The acknowledgement.
   * @return The placed order.
   */
  static Result<PlacedOrder<cmdatl::paper::Broker>> normalize_place(Result<PlaceAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<cmdatl::paper::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<cmdatl::paper::Broker> placed;
    placed.ack_.order_id_ = raw.value().id_;
    placed.ack_.accepted_at_ = raw.value().at_;
    placed.ref_ = make_ref(raw.value().id_);
    return Result<PlacedOrder<cmdatl::paper::Broker>>(std::move(placed));
  }

  /**
   * @brief The listing as the library's.
   * @param raw The listing.
   * @return The working orders.
   */
  static Result<WorkingOrders<cmdatl::paper::Broker>> normalize_working_orders(Result<WorkingOrdersResult> raw) {
    if (!raw) {
      return Result<WorkingOrders<cmdatl::paper::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<cmdatl::paper::Broker> orders;
    orders.received_at_ = raw.value().read_at_;
    for (auto& row : raw.value().rows_) {
      WorkingOrder<cmdatl::paper::Broker> working;
      working.order_ = std::move(row.order_);
      working.ref_ = make_ref(row.id_);
      working.amendable_quantity_ = row.open_quantity_;
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<cmdatl::paper::Broker>>(std::move(orders));
  }

  /**
   * @brief An amendment by the order's id.
   * @param ref The order.
   * @param instruction The amendment.
   * @return The amendment.
   */
  static Result<Amendment> to_replacement(
    const OrderRef<cmdatl::paper::Broker>& ref, const AmendInstruction& instruction
  ) {
    Amendment amendment;
    amendment.id_ = ref.primary_;
    amendment.quantity_ = instruction.quantity_;
    amendment.price_ = instruction.shape_.price_;
    return Result<Amendment>(std::move(amendment));
  }

  /**
   * @brief A cancellation by the order's id: the whole order only.
   * @param ref The order.
   * @param instruction The cancellation.
   * @return The cancellation, or a refusal before the market.
   */
  static Result<Amendment> to_cancellation(
    const OrderRef<cmdatl::paper::Broker>& ref, const AmendInstruction& instruction
  ) {
    if (!instruction.all_remaining_) {
      return Result<Amendment>(cmdatl::paper::refusal("partial_cancel_unsupported", "cancel the whole order", false));
    }
    return to_replacement(ref, instruction);
  }

  /**
   * @brief An order-history query: a date range only.
   * @param account The account.
   * @param query The query.
   * @return The query, or a refusal before the market.
   */
  static Result<HistoryQuery> to_history_query(const Account& account, const OrderHistoryQuery& query) {
    (void)account;
    if (query.symbol_ || query.cursor_ || query.limit_) {
      return Result<HistoryQuery>(cmdatl::paper::refusal("history_filter_unsupported", "date range only", false));
    }
    return Result<HistoryQuery>(HistoryQuery{query.from_, query.to_});
  }

  /**
   * @brief No rate limit.
   * @param client The client.
   * @return Null.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    (void)client;
    return nullptr;
  }

  /**
   * @brief Bars.
   * @param c The client.
   * @param q The query.
   * @return The answer.
   */
  static Result<BarSeries> bars(Client& c, BarQuery q) {
    return c.bars(q);
  }
  /**
   * @brief The quote.
   * @param c The client.
   * @param i The instrument.
   * @return The answer.
   */
  static Result<QuoteSnapshot> quote(Client& c, InstrumentId i) {
    return c.quote(i);
  }
  /**
   * @brief The holdings.
   * @param c The client.
   * @param a The account.
   * @return The answer.
   */
  static Result<HoldingsSnapshot> holdings(Client& c, const Account& a) {
    return c.holdings(a);
  }
  /**
   * @brief The buying power.
   * @param c The client.
   * @param a The account.
   * @param cur The currency.
   * @return The answer.
   */
  static Result<BuyingPower> buying_power(Client& c, const Account& a, Currency cur) {
    return c.buying_power(a, cur);
  }
  /**
   * @brief A placement: the bare client places nothing, only the logged client does (desk.hpp).
   * @param c The client.
   * @param a The account.
   * @param o The order.
   * @return The refusal.
   */
  static Result<PlaceAck> place_order(Client& c, const Account& a, OrderRequest o) {
    (void)c;
    (void)a;
    (void)o;
    return cmdatl::paper::refusal("unlogged", "the paper market takes orders through its log only", false);
  }
  /**
   * @brief An amendment.
   * @param c The client.
   * @param a The account.
   * @param m The amendment.
   * @return The answer.
   */
  static Result<PlaceAck> replace_order(Client& c, const Account& a, Amendment m) {
    return c.replace(a, m);
  }
  /**
   * @brief A cancellation: the bare client cancels nothing, only the logged client does (desk.hpp).
   * @param c The client.
   * @param a The account.
   * @param m The cancellation.
   * @return The refusal.
   */
  static Result<PlaceAck> cancel_order(Client& c, const Account& a, Amendment m) {
    (void)c;
    (void)a;
    (void)m;
    return cmdatl::paper::refusal("unlogged", "the paper market takes cancellations through its log only", false);
  }
  /**
   * @brief The listing.
   * @param c The client.
   * @param a The account.
   * @return The answer.
   */
  static Result<WorkingOrdersResult> working_orders(Client& c, const Account& a) {
    return c.open_orders(a);
  }
  /**
   * @brief The history.
   * @param c The client.
   * @param a The account.
   * @param q The query.
   * @return The answer.
   */
  static Result<OrderPage> order_history(Client& c, const Account& a, HistoryQuery q) {
    return c.history(a, q);
  }
  /**
   * @brief Reference data.
   * @param c The client.
   * @param i The instrument.
   * @return The answer.
   */
  static Result<Instrument> instrument(Client& c, InstrumentId i) {
    return c.instrument(i);
  }
  /**
   * @brief The calendar.
   * @param c The client.
   * @param country The country.
   * @param from The first date.
   * @return The answer.
   */
  static Result<TradingCalendar> calendar(Client& c, Country country, std::optional<Date> from) {
    (void)country;
    return c.calendar(from);
  }
  /**
   * @brief The band.
   * @param c The client.
   * @param i The instrument.
   * @return The answer.
   */
  static Result<PriceLimits> price_limits(Client& c, InstrumentId i) {
    return c.price_limits(i);
  }
};

} // namespace algotradelib
