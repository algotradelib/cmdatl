#pragma once

// The placement boundary runs trade through: a session that forwards every call to the broker's session and, before
// forwarding a placement, checks the operator's ceilings once more, as the last thing between the runner and the broker.
//
// The runner's own guard stays on; this repeats what the operator's file decides, at the final boundary, because the
// library's guard admits an order closing toward flat outside the permitted phases, and a broker (the mock among them)
// accepts orders at any instant. A cancellation is checked only for the lock, so withdrawals stay available while
// disarmed or outside a permitted phase.
// It counts what it sends, so a caller can tell a call that sent nothing from one whose outcome may be at the broker.

#include "../system/lock.hpp"
#include "../system/process.hpp"
#include "./operator.hpp"

#include <algotradelib/broker>
#include <algotradelib/strategy>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;

/** @brief What the operator's file permits at a placement, fixed for the run's life. */
struct PlacementPolicy {
  /** @brief The operator's armed file: without it nothing is placed. */
  std::string armed_;
  /** @brief The run's lock: once it is no longer at its path, nothing is placed or cancelled. */
  const OwnerLock* lock_{nullptr};
  /** @brief Whether only limit orders may be placed. */
  bool limit_only_{true};
  /** @brief How far a limit may sit from the last price, as a fraction. */
  double collar_{0.05};
  /** @brief The phases in which anything may be placed. */
  std::vector<atl::strategy::MarketPhase> phases_;
  /** @brief The market's session window, which dates and phases are computed in. */
  atl::strategy::SessionWindow window_{atl::strategy::SessionWindow::krx()};
  /** @brief The calendar's country. */
  atl::Country country_{atl::Country::KR};
};

/** @brief One placement the boundary saw: forwarded and accepted, refused by the broker, or refused here. */
struct PlacementEvent {
  /** @brief `placed`, or `refused`. */
  std::string kind_;
  /** @brief When, on the market's clock. */
  std::chrono::system_clock::time_point at_;
  /** @brief The order as the runner built it. */
  atl::OrderRequest order_;
  /** @brief The broker's order id, for a placed order. */
  std::string order_id_;
  /** @brief Why it was refused: one of this boundary's reasons, or the broker's code. */
  std::string reason_;
};

/**
 * @brief The policy's own refusal of a placement, or nothing: the reason, in the order checked.
 * @param policy The policy.
 * @param closed Whether the run has stopped placing altogether.
 * @param order The order.
 * @param mark The last price, when one could be read.
 * @param phase The market's phase now.
 * @return `closed`, `not_armed`, `lock_lost`, `order_type`, `no_price`, `collar` or `phase`, or nothing.
 */
inline std::optional<std::string> placement_refusal(
  const PlacementPolicy& policy, bool closed, const atl::OrderRequest& order, std::optional<atl::Decimal64> mark,
  atl::strategy::MarketPhase phase
) {
  if (closed) {
    return std::string("closed");
  }
  std::error_code ec;
  if (!std::filesystem::exists(policy.armed_, ec)) {
    return std::string("not_armed");
  }
  if (policy.lock_ == nullptr || !policy.lock_->still_at_path()) {
    return std::string("lock_lost");
  }
  if (policy.limit_only_ && (order.type_ != atl::OrderType::LIMIT || !order.price_)) {
    return std::string("order_type");
  }
  if (!mark || !atl::decimal_less(atl::Decimal64::parse("0"), *mark)) {
    return std::string("no_price");
  }
  if (order.price_ && outside_collar(std::stod(order.price_->str()), std::stod(mark->str()), policy.collar_)) {
    return std::string("collar");
  }
  bool permitted = false;
  for (const auto allowed : policy.phases_) {
    permitted = permitted || allowed == phase;
  }
  if (!permitted) {
    return std::string("phase");
  }
  return std::nullopt;
}

/**
 * @brief A session that forwards to another and checks the operator's ceilings before every placement, and the lock
 *   before every cancellation.
 *
 * It exposes the inner session's `Caps`, `BrokerTag` and `Traits` and its call signatures, so the library's binding,
 * executor and source take it as they take the inner session.
 * @tparam Inner The broker's session type, such as `BlockingSession<mock::MockClient>`.
 * @tparam Market The session the boundary's own phase and price reads go to: the inner one, or one that cannot trade
 *   (a KIS demo account reads market data with the real login, since its host refuses the calendar).
 */
template <typename Inner, typename Market = Inner>
class GuardedSession {
public:
  /** @brief The broker's capabilities. */
  using Caps = typename Inner::Caps;
  /** @brief The broker's tag. */
  using BrokerTag = typename Inner::BrokerTag;
  /** @brief The broker's traits, so the executor keeps its route selection. */
  using Traits = typename Inner::Traits;
  /** @brief An order reference. */
  using Ref = typename Inner::Ref;

  /**
   * @brief Builds the boundary.
   * @param inner The broker's session; borrowed, and must outlive this.
   * @param policy The operator's ceilings.
   * @param now The market's clock.
   * @param observer Told of every placement, forwarded or refused; may be empty.
   */
  GuardedSession(
    Inner& inner, PlacementPolicy policy, std::function<std::chrono::system_clock::time_point()> now,
    std::function<void(const PlacementEvent&)> observer
  )
  : inner_(&inner), market_(&inner), policy_(std::move(policy)), now_(std::move(now)), observer_(std::move(observer)) {}

  /**
   * @brief Builds the boundary with its phase and price reads going to another session.
   * @param inner The broker's session, which places and cancels; borrowed, and must outlive this.
   * @param market The session read for the calendar and the last price; borrowed, and must outlive this.
   * @param policy The operator's ceilings.
   * @param now The market's clock.
   * @param observer Told of every placement, forwarded or refused; may be empty.
   */
  GuardedSession(
    Inner& inner, Market& market, PlacementPolicy policy, std::function<std::chrono::system_clock::time_point()> now,
    std::function<void(const PlacementEvent&)> observer
  )
  : inner_(&inner),
    market_(&market),
    policy_(std::move(policy)),
    now_(std::move(now)),
    observer_(std::move(observer)) {}

  /**
   * @brief Stops every later placement: called when the loop has returned, so nothing the closing reads trigger sends.
   */
  void close() noexcept {
    closed_ = true;
  }

  /**
   * @brief Names the run's latched stop: once it says a stop was asked for (a disarm, a lost lock, a closed output, the
   *   end), every later placement is refused here, even one the runner sends before it takes the stop up.
   * @param stopping Whether a stop is latched.
   */
  void set_stopping(std::function<bool()> stopping) {
    stopping_ = std::move(stopping);
  }

  /**
   * @brief Whether the order ids this boundary cancelled include one.
   * @param order_id The broker's order id.
   * @return Whether it cancelled it.
   */
  bool cancelled(const std::string& order_id) const {
    return cancelled_.count(order_id) != 0;
  }

  /**
   * @brief How many placements and cancellations were sent towards the broker: each counted before it is forwarded, and
   *   given back only for an answer that says it did not reach the broker; a call that throws stays counted.
   * @return The count.
   */
  std::uint64_t sends() const noexcept {
    return sends_;
  }

  /**
   * @brief The market's phase now, from the calendar read through the market session: `UNKNOWN` when it cannot be read.
   * @return The phase.
   */
  atl::strategy::MarketPhase phase_now() {
    const auto now = now_();
    const auto calendar = market_->calendar(policy_.country_, std::nullopt);
    if (!calendar) {
      return atl::strategy::MarketPhase::UNKNOWN;
    }
    return policy_.window_.phase(now, calendar.value().is_trading_day(policy_.window_.local_date(now)));
  }

  /**
   * @brief Places an order after the policy's checks; a refusal here reaches no broker.
   * @param order The order.
   * @return The broker's answer, or a local refusal.
   */
  auto place(atl::OrderRequest order) {
    using Answer = decltype(inner_->place(order));
    std::optional<atl::Decimal64> mark;
    if (!closed_) {
      const auto quote = market_->quote(order.instrument_);
      if (quote) {
        mark = quote.value().last_price_;
      }
    }
    const auto phase = closed_ ? atl::strategy::MarketPhase::UNKNOWN : phase_now();
    // A stop noted since the last step refuses at once: the guard withdraws at the next step, and nothing new goes out
    // meanwhile.
    const auto reason = interrupted() ?
      std::optional<std::string>("signal") :
      stopping_ && stopping_() ?
      std::optional<std::string>("stopping") :
      placement_refusal(policy_, closed_, order, mark, phase);
    if (reason) {
      tell("refused", order, "", *reason);
      atl::BrokerError error;
      error.category_ = atl::ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = *reason;
      error.message_ = "refused by the operator's limits before sending: " + *reason;
      return Answer(std::move(error));
    }
    ++sends_;
    auto answer = inner_->place(order);
    if (answer) {
      tell("placed", order, answer.value().ref_.primary_, "");
    } else {
      give_back(answer.error());
      tell("refused", order, "", answer.error().broker_code_.empty() ? "broker" : answer.error().broker_code_);
    }
    return answer;
  }

  /**
   * @brief Cancels an order once the lock is checked, whether armed or not and in any phase; an accepted cancellation
   *   is remembered.
   * @param ref The order.
   * @param instruction What to cancel.
   * @return The broker's answer, or the refusal `lock_lost`, which reaches no broker.
   */
  auto cancel(const Ref& ref, const atl::AmendInstruction& instruction) {
    using Answer = decltype(inner_->cancel(ref, instruction));
    if (policy_.lock_ == nullptr || !policy_.lock_->still_at_path()) {
      atl::BrokerError error;
      error.category_ = atl::ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "lock_lost";
      error.message_ = "refused before sending: the lock file was replaced";
      return Answer(std::move(error));
    }
    ++sends_;
    auto answer = inner_->cancel(ref, instruction);
    if (answer) {
      cancelled_.insert(ref.primary_);
    } else {
      give_back(answer.error());
    }
    return answer;
  }

  /** @brief Forwarded. @param query The query. @return The answer. */
  auto bars(atl::BarQuery query) {
    return inner_->bars(std::move(query));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto quote(atl::InstrumentId instrument) {
    return inner_->quote(std::move(instrument));
  }
  /** @brief Forwarded. @param subject The instrument. @return The answer. */
  auto instrument(atl::InstrumentId subject) {
    return inner_->instrument(std::move(subject));
  }
  /** @brief Forwarded. @param country The country. @param from The first date. @return The answer. */
  auto calendar(atl::Country country, std::optional<atl::Date> from = std::nullopt) {
    return inner_->calendar(country, std::move(from));
  }
  /** @brief Forwarded. @return The answer. */
  auto holdings() {
    return inner_->holdings();
  }
  /** @brief Forwarded. @param currency The currency. @return The answer. */
  auto buying_power(atl::Currency currency) {
    return inner_->buying_power(std::move(currency));
  }
  /** @brief Forwarded. @return The answer. */
  auto working_orders() {
    return inner_->working_orders();
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto order_history(const atl::OrderHistoryQuery& query) {
    return inner_->order_history(query);
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto sellable_quantity(atl::InstrumentId instrument) {
    return inner_->sellable_quantity(std::move(instrument));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto price_limits(atl::InstrumentId instrument) {
    return inner_->price_limits(std::move(instrument));
  }

private:
  /**
   * @brief Takes a send back off the count when its answer says it did not reach the broker.
   * @param error The answer.
   */
  void give_back(const atl::BrokerError& error) noexcept {
    if (!error.reached_broker_) {
      --sends_;
    }
  }

  void tell(const char* kind, const atl::OrderRequest& order, std::string order_id, std::string reason) {
    if (!observer_) {
      return;
    }
    PlacementEvent event;
    event.kind_ = kind;
    event.at_ = now_();
    event.order_ = order;
    event.order_id_ = std::move(order_id);
    event.reason_ = std::move(reason);
    observer_(event);
  }

  Inner* inner_;
  Market* market_;
  PlacementPolicy policy_;
  std::function<std::chrono::system_clock::time_point()> now_;
  std::function<void(const PlacementEvent&)> observer_;
  std::set<std::string> cancelled_;
  std::function<bool()> stopping_;
  std::uint64_t sends_{0};
  bool closed_{false};
};

} // namespace driver
} // namespace cmdatl
