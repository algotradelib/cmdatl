#pragma once

// once(): one target, one decision, one call, on an account whose lock the caller holds and whose journal the caller
// opened; and release(), the operator's settlement of what the journal owes.
//
// The rules:
//   1. Nothing places unless the armed file exists; a withdrawal needs none, since it adds no exposure.
//   2. The caller holds the account's owner lock for the whole command; once() checks it is still the file at its
//      path before it writes.
//   3. `recover_runner` runs before any read; damage the journal shows is an unknown.
//   4. Nothing is submitted unless the working-order listing was read in this call.
//   5. The runner is armed in one place (`arm`): a known portfolio, the observed instant, the portfolio's age bound.
//   6. One order per instrument until it is accounted for: the runner is serialized, an owed order the broker never
//      named (or one held for the operator) is an unknown, and a named one is a wait until holdings show it.
//   7. Targets only, limit orders only, the limit inside a collar around the last price and on the tick grid.
//   8. A cancel and a placement never go out in one call: with an order working, the call finds the target met by it
//      or withdraws it, and the placement waits for a call that sees it gone.
//   9. Unknown is never reported as anything else: the reply after the send is mapped by whether the desk counted a
//      send in the call (`map_report`), and anything unmapped or thrown after a send is an unknown.
//  10. Only in a permitted phase, except a withdrawal.
//
// The options a caller turns on: `dry_run_` (a plan, and how reconcile runs), `withdraw_only_` (a cancel),
// `refuse_unjournaled_orders_` (an order no journal line names refuses the call).

#include "../account/operator.hpp"
#include "../core/reply.hpp"
#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "../system/syscalls.hpp"

#include <algotradelib/live>
#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace sending {

namespace atl = algotradelib;

/** @brief What a call ends with; the numbers are the exit codes. */
enum class OnceExit : int {
  /** @brief Done: placed, withdrawn as asked, planned, or nothing needed. */
  DONE = 0,
  /** @brief Refused: no `place` or `cancel` input reached the paper log in this call. */
  REFUSED = 3,
  /** @brief Unknown: something may be at the broker, or the journal cannot say. */
  UNKNOWN = 4,
};

/** @brief One reason once() or release() can answer with, and its exit. */
struct OnceReason {
  /** @brief The reason, such as `collar`. */
  const char* reason_;
  /** @brief Its exit. */
  OnceExit exit_;
};

/**
 * @brief Every reason once() and release() can answer with, and its exit; each becomes the error code
 *   `refused.<reason>` (3) or `unknown.<reason>` (4), or `reason` on exit 0.
 */
inline const std::vector<OnceReason> ONCE_REASONS{
  {"plan", OnceExit::DONE},
  {"met", OnceExit::DONE},
  {"placed", OnceExit::DONE},
  {"withdrawal_sent", OnceExit::DONE},
  {"nothing_working", OnceExit::DONE},
  {"released", OnceExit::DONE},
  {"not_armed", OnceExit::REFUSED},
  {"reference_unread", OnceExit::REFUSED},
  {"order_list_unread", OnceExit::REFUSED},
  {"holdings_unread", OnceExit::REFUSED},
  {"phase", OnceExit::REFUSED},
  {"awaiting_confirmation", OnceExit::REFUSED},
  {"no_price", OnceExit::REFUSED},
  {"collar", OnceExit::REFUSED},
  {"off_tick", OnceExit::REFUSED},
  {"unjournaled_order", OnceExit::REFUSED},
  {"guard", OnceExit::REFUSED},
  {"not_reached", OnceExit::REFUSED},
  {"withheld", OnceExit::REFUSED},
  {"journal_unwritable", OnceExit::REFUSED},
  {"not_approved", OnceExit::REFUSED},
  {"nothing_owed", OnceExit::REFUSED},
  {"named", OnceExit::REFUSED},
  {"lock_lost", OnceExit::UNKNOWN},
  {"journal_damaged", OnceExit::UNKNOWN},
  {"unaccounted", OnceExit::UNKNOWN},
  {"unresolved", OnceExit::UNKNOWN},
  {"unconfirmed", OnceExit::UNKNOWN},
  {"indeterminate", OnceExit::UNKNOWN},
  {"withdrawal", OnceExit::UNKNOWN},
  {"mixed", OnceExit::UNKNOWN},
  {"unmapped", OnceExit::UNKNOWN},
  {"journal_write", OnceExit::UNKNOWN},
  {"broker_refused", OnceExit::UNKNOWN},
  {"internal", OnceExit::UNKNOWN},
};

/** @brief What to move to. */
struct OnceRequest {
  /** @brief The instrument. */
  atl::InstrumentId instrument_;
  /** @brief The position to hold, whole shares, never below zero. */
  atl::Decimal64 target_quantity_;
  /** @brief The limit every derived order carries; absent for a withdrawal and for reconcile, which never submit. */
  std::optional<atl::Decimal64> limit_;
};

/** @brief How a call runs. */
struct OnceConfig {
  /** @brief The operator's armed file. */
  std::string armed_file_;
  /** @brief The risk limits the runner enforces. */
  atl::strategy::RiskLimits limits_;
  /** @brief How far the limit may sit from the last price, as a fraction. */
  double collar_{0.05};
  /** @brief How long an owed order may wait for holdings before it is an unknown rather than a wait. */
  std::chrono::nanoseconds confirmation_timeout_{std::chrono::minutes(10)};
  /** @brief How old holdings and the listing may be when the decision is made. */
  std::chrono::nanoseconds maximum_portfolio_age_{std::chrono::seconds(30)};
  /** @brief Plan only: every read and check, then report what would be sent, sending nothing. */
  bool dry_run_{false};
  /** @brief A cancel: never submit; withdraw every listed order on the instrument, in any phase and unarmed. */
  bool withdraw_only_{false};
  /** @brief Refuse when the listing holds an order on the instrument that no journal line names. */
  bool refuse_unjournaled_orders_{false};
};

/** @brief The order a call placed, as the report and the reserved row name it. */
struct PlacedOrder {
  /** @brief The broker's id, or empty when no row names it yet. */
  std::string order_id_;
  /** @brief `buy` or `sell`. */
  std::string side_;
  /** @brief The quantity. */
  std::string quantity_;
  /** @brief The limit. */
  std::string limit_;
};

/** @brief What a call answered. */
struct OnceResult {
  /** @brief The exit. */
  OnceExit exit_{OnceExit::REFUSED};
  /** @brief A reason of `ONCE_REASONS`. */
  std::string reason_;
  /** @brief What happened, for a person. */
  std::string detail_;
  /** @brief The phase the decision was made in, when one was read. */
  std::string phase_;
  /** @brief For a plan: `withdraw`, `buy` or `sell`. */
  std::string plan_kind_;
  /** @brief The broker's ids of the orders a withdrawal was sent for. */
  std::vector<std::string> withdrawn_;
  /** @brief The order placed. */
  std::optional<PlacedOrder> order_;
  /** @brief For release: the journal ids released. */
  std::vector<std::string> released_;
  /** @brief For release: whether the approval could not be renamed after its use. */
  bool approval_kept_{false};
};

/** @brief The strategy a call binds: none; the decision is the caller's target, submitted directly. */
struct NoStrategy {
  /**
   * @brief Never called.
   * @tparam Ctx The context.
   * @param ctx The context.
   * @param event The bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const atl::strategy::BarEvent& event) {
    (void)ctx;
    (void)event;
  }
};

namespace once_detail {

/**
 * @brief An instant as nanoseconds since 1970.
 * @param t The instant.
 * @return The count.
 */
inline std::int64_t nanos(std::chrono::system_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

/**
 * @brief Whether `a < b`, for decimals of any scale.
 * @param a One.
 * @param b The other.
 * @return Whether it is less.
 */
inline bool less(const atl::Decimal64& a, const atl::Decimal64& b) {
  return atl::decimal_less(a, b);
}

/**
 * @brief A decimal as a double, for the collar's fraction and the netting of whole shares.
 * @param value The decimal.
 * @return The double.
 */
inline double number(const atl::Decimal64& value) {
  return std::stod(value.str());
}

/**
 * @brief A broker error as one line, its code first.
 * @param error The error.
 * @return The line.
 */
inline std::string described(const atl::BrokerError& error) {
  return (error.broker_code_.empty() ? std::string() : "[" + error.broker_code_ + "] ") + error.message_;
}

/**
 * @brief Rule 5: what a runner driven without a loop must be told before it may plan.
 * @tparam Runner The runner.
 * @param runner The runner.
 * @param now The decision's instant.
 * @param age The portfolio's age bound.
 */
template <typename Runner>
void arm(Runner& runner, std::chrono::system_clock::time_point now, std::chrono::nanoseconds age) {
  runner.require_known_portfolio(true);
  runner.set_observed_at(now);
  runner.set_maximum_portfolio_age(age);
}

/**
 * @brief Whether a poll answered.
 * @param outcome The poll's outcome.
 * @return Whether it was read.
 */
inline bool answered(atl::runner::PollOutcome outcome) {
  return outcome == atl::runner::PollOutcome::UPDATED || outcome == atl::runner::PollOutcome::UNCHANGED;
}

/**
 * @brief The orders the journal owes on an instrument, as `journal id (order id or unnamed, state)`.
 * @tparam Runner The runner.
 * @param runner The runner.
 * @param id The instrument.
 * @return The text.
 */
template <typename Runner>
std::string owed_text(const Runner& runner, const atl::InstrumentId& id) {
  std::string text;
  for (const auto& owed : runner.obligations(id)) {
    if (!text.empty()) {
      text += ", ";
    }
    text += owed.journal_id_ + " (" + (owed.order_id_.empty() ? std::string("unnamed") : owed.order_id_) + ", " +
      std::string(atl::runner::obligation_state_name(owed.state_)) + ")";
  }
  return text;
}

/**
 * @brief Whether the journal owes an order on an instrument whose outcome is unknown, or that only an operator may
 *   settle.
 * @tparam Runner The runner.
 * @param runner The runner.
 * @param id The instrument.
 * @return Whether it does.
 */
template <typename Runner>
bool owes_unknown(const Runner& runner, const atl::InstrumentId& id) {
  const auto state = runner.obligation_state(id);
  return state == atl::runner::ObligationState::UNNAMED || state == atl::runner::ObligationState::OPERATOR;
}

/** @brief The decision clock as the journal's clock, so its stamps and the broker's rows share one clock. */
struct DecisionClock {
  /** @brief The clock. */
  std::function<std::chrono::system_clock::time_point()> now_;

  /**
   * @brief The instant.
   * @return It.
   */
  std::chrono::system_clock::time_point now() const {
    return now_();
  }
};

/** @brief The bound runner of a session, journaling to a file on the decision clock. */
template <typename Session>
using Bound = atl::runner::Runner<
  NoStrategy, typename Session::Caps, typename Session::BrokerTag,
  atl::runner::JournalingExecutor<atl::live::SessionExecutor<Session>, atl::runner::FileJournal, DecisionClock>
>;

/**
 * @brief Binds a runner to a session and a journal: journal ids prefixed by the wall clock's nanoseconds, unique
 *   across processes even when the paper clock has not moved.
 * @tparam Session The session.
 * @param session The session.
 * @param journal The journal, open; it must outlive the runner.
 * @param limits The risk limits.
 * @param now The decision clock.
 * @return The runner.
 */
template <typename Session>
Bound<Session> bind(
  Session& session, atl::runner::FileJournal& journal, const atl::strategy::RiskLimits& limits,
  const std::function<std::chrono::system_clock::time_point()>& now
) {
  atl::runner::JournalingExecutor<atl::live::SessionExecutor<Session>, atl::runner::FileJournal, DecisionClock>
    executor(atl::live::SessionExecutor<Session>(session, atl::Venue::UNSPECIFIED), journal, DecisionClock{now});
  executor.set_id_prefix(std::to_string(nanos(std::chrono::system_clock::now())));
  return Bound<Session>(NoStrategy{}, limits, atl::live::default_series_capacity, std::move(executor));
}

/**
 * @brief Whether recovery left the runner able to send: every line read, nothing unattributable, not halted.
 * @tparam Runner The runner.
 * @param recovery The recovery's report.
 * @param runner The runner.
 * @return Whether it did.
 */
template <typename Runner>
bool recovered(const atl::runner::RecoveryReport& recovery, const Runner& runner) {
  return !(
    recovery.truncated_ || recovery.unattributable_ || recovery.unreadable_lines_ > 0 || recovery.unreadable_file_ ||
    runner.guard().halted()
  );
}

} // namespace once_detail

/** @brief The reply's outcome as `map_report` decides it. */
struct Mapped {
  /** @brief The exit. */
  OnceExit exit_{OnceExit::UNKNOWN};
  /** @brief The reason. */
  std::string reason_;
  /** @brief What happened. */
  std::string detail_;
};

/**
 * @brief The reply after `settle_closing`, by what reached the market: the first rule that applies.
 *
 * 1. a send in transit: `indeterminate` (4);
 * 2. a journal write failed during the settle after an input was logged: `journal_write` (4);
 * 3. a journal write failed and no input was logged: `journal_unwritable` (3);
 * 4. a withdrawal: only withdrawals sent, `withdrawal_sent` (0); nothing sent, `not_reached` (3); else `withdrawal` (4);
 * 5. a placement: only placed, `placed` (0), else `mixed` (4);
 * 6. nothing at all: `met` (0);
 * 7. nothing logged and nothing sent: `guard`, `not_reached` (the binding refused before its line, with its text) or
 *    `withheld` (3);
 * 8. something logged and the market refused it: `broker_refused` (4), with the market's text;
 * 9. anything else: `unmapped` (4).
 *
 * @tparam Report The runner's report type.
 * @param report The report.
 * @param logged How many paper inputs this call logged.
 * @param failed Whether the runner's local failure became set during the settle.
 * @param withdrawals How many withdrawals the call asked for (0 when it was not a withdrawal).
 * @param binding_refusal Why the binding refused the call's last request, or empty.
 * @param broker_refusal Why the market refused the call's last refused request, or empty.
 * @return The outcome.
 */
template <typename Report>
Mapped map_report(
  const Report& report, std::uint64_t logged, bool failed, std::size_t withdrawals, const std::string& binding_refusal,
  const std::string& broker_refusal
) {
  if (!report.indeterminate_.empty() || !report.indeterminate_cancel_.empty()) {
    return {OnceExit::UNKNOWN, "indeterminate", "a send failed in transit; read the order list before anything else"};
  }
  if (failed && logged > 0) {
    return {
      OnceExit::UNKNOWN, "journal_write",
      "the request reached the paper log but its journal answer could not be written whole"
    };
  }
  if (failed) {
    return {
      OnceExit::REFUSED, "journal_unwritable",
      "the journal could not be written, so nothing reached the market; the journal may now hold the request's PENDING "
      "line whole (owed until order release), a fragment of it (the journal is then damaged), or nothing (plan again); "
      "the next cmdatl account shows which"
    };
  }
  const bool cancels_quiet = report.unresolved_cancel_.empty() && report.unavailable_cancel_.empty() &&
    report.refused_cancel_.empty() && report.rejected_cancel_.empty();
  const bool places_quiet = report.rejected_.empty() && report.refused_.empty() && report.withheld_.empty() &&
    report.withheld_plans_.empty() && report.unavailable_.empty();
  if (withdrawals > 0) {
    if (report.would_place_.empty() && places_quiet && cancels_quiet && report.would_cancel_.size() == withdrawals) {
      return {OnceExit::DONE, "withdrawal_sent", "sent, not confirmed: cmdatl order list shows when they are gone"};
    }
    if (logged == 0) {
      return {
        OnceExit::REFUSED, "not_reached",
        "the withdrawal was refused before it reached the market" +
          (binding_refusal.empty() ? std::string() : ": " + binding_refusal)
      };
    }
    return {OnceExit::UNKNOWN, "withdrawal", "a withdrawal was not taken as asked; an order may have filled"};
  }
  if (!report.would_place_.empty()) {
    if (report.would_cancel_.empty() && places_quiet && cancels_quiet) {
      return {OnceExit::DONE, "placed", "the order was accepted by the market"};
    }
    return {OnceExit::UNKNOWN, "mixed", "an order was placed and something else in the plan was not"};
  }
  if (report.quiet()) {
    return {OnceExit::DONE, "met", "nothing needed"};
  }
  const bool nothing_sent = report.would_cancel_.empty() && cancels_quiet;
  if (logged == 0 && nothing_sent) {
    if (!report.refused_.empty()) {
      return {
        OnceExit::REFUSED, "guard", std::string(atl::strategy::guard_reason_name(report.refused_.front().reason_))
      };
    }
    if (!report.rejected_.empty() && report.rejected_.front().reason_ == atl::strategy::RejectReason::BINDING_REFUSED) {
      return {
        OnceExit::REFUSED, "not_reached",
        "the paper client refused the request before writing it" +
          (binding_refusal.empty() ? std::string() : ": " + binding_refusal)
      };
    }
    if (!report.withheld_.empty() || !report.withheld_plans_.empty() || !report.unavailable_.empty()) {
      return {OnceExit::REFUSED, "withheld", "the runner withheld the plan (unknown or stale book, or a hold)"};
    }
  }
  if (logged > 0) {
    for (const auto& rejected : report.rejected_) {
      if (rejected.reason_ == atl::strategy::RejectReason::BROKER_REFUSED) {
        return {
          OnceExit::UNKNOWN, "broker_refused",
          "the request reached the paper market, which refused it" +
            (broker_refusal.empty() ? std::string() : " (" + broker_refusal + ")") + "; nothing rests"
        };
      }
    }
  }
  return {OnceExit::UNKNOWN, "unmapped", "the report carried a result this call does not map"};
}

/**
 * @brief Plans or places one target on an account, under the rules at the top of this file.
 *
 * Only a caller that holds the account's lock and has opened its journal can call it; the journal must outlive the
 * call, and the desk's clock dates the decision and the journal's lines.
 *
 * @tparam Desk The desk (`account/account.hpp`): `session()`, `market_session()`, `now()` and `sends()`.
 * @param desk The desk.
 * @param journal The account's journal, opened by the caller.
 * @param lock The account's lock, held.
 * @param request The target.
 * @param config How to run.
 * @return The outcome.
 * @throws std::logic_error On a defect: a target below zero, or a placement without a limit.
 */
template <typename Desk>
OnceResult once(
  Desk& desk, atl::runner::FileJournal& journal, const OwnerLock& lock, const OnceRequest& request,
  const OnceConfig& config
) {
  using atl::runner::PollKind;
  using Session = typename Desk::Session;
  using MarketSession = typename Desk::MarketSession;
  if (request.target_quantity_.coefficient() < 0) {
    throw std::logic_error("once() takes no target below zero");
  }
  auto& session = desk.session();
  const std::function<std::chrono::system_clock::time_point()> now = [&desk] {
    return desk.now();
  };
  OnceResult result;
  const auto refuse = [&result](std::string reason, std::string detail = {}) {
    result.exit_ = OnceExit::REFUSED;
    result.reason_ = std::move(reason);
    result.detail_ = std::move(detail);
    return result;
  };
  const auto unknown = [&result](std::string reason, std::string detail = {}) {
    result.exit_ = OnceExit::UNKNOWN;
    result.reason_ = std::move(reason);
    result.detail_ = std::move(detail);
    return result;
  };
  const auto done = [&result](std::string reason, std::string detail = {}) {
    result.exit_ = OnceExit::DONE;
    result.reason_ = std::move(reason);
    result.detail_ = std::move(detail);
    return result;
  };
  const auto armed = [&config] {
    std::error_code ec;
    return !config.armed_file_.empty() && std::filesystem::exists(config.armed_file_, ec);
  };

  // 1. Armed, unless a plan or a withdrawal.
  if (!config.dry_run_ && !config.withdraw_only_ && !armed()) {
    return refuse("not_armed", "the armed file does not exist: " + config.armed_file_);
  }
  // 2. The caller's lock, still the account's.
  if (!lock.still_at_path()) {
    return unknown("lock_lost", "the account's lock file was replaced; nothing was written");
  }
  if (!journal.open()) {
    return unknown("journal_damaged", "the journal is not open: " + journal.rejection());
  }
  auto bound = once_detail::bind(session, journal, config.limits_, now);

  // 3. Recovery before any read.
  const auto recovery = atl::runner::recover_runner(bound, journal);
  if (!once_detail::recovered(recovery, bound)) {
    return unknown("journal_damaged", recovery.detail_.empty() ? "the journal is damaged" : recovery.detail_);
  }
  const auto unaccounted = [&] {
    return unknown(
      "unaccounted",
      "the journal owes an order on this instrument whose outcome is unknown or held for the operator: " +
        once_detail::owed_text(bound, request.instrument_)
    );
  };
  if (bound.obligation_state(request.instrument_) == atl::runner::ObligationState::OPERATOR) {
    return unaccounted();
  }
  auto reference = session.instrument(request.instrument_);
  if (!reference) {
    if (once_detail::owes_unknown(bound, request.instrument_)) {
      return unaccounted();
    }
    return refuse("reference_unread", once_detail::described(reference.error()));
  }
  const auto rule = reference.value().price_rule_;
  const auto currency = reference.value().currency_;
  bound.context().set_instrument(std::move(reference.value()));

  // 4. The working-order listing, then the rest of the book.
  atl::live::SessionSource<Session, atl::runner::FileJournal, MarketSession> source(
    session, desk.market_session(), {request.instrument_}
  );
  source.set_journal(journal);
  source.set_buying_power_currencies({currency});
  once_detail::arm(bound, now(), config.maximum_portfolio_age_);
  if (!once_detail::answered(source.read_order_list(bound))) {
    if (
      bound.ledger().requires_reconcile(request.instrument_) || once_detail::owes_unknown(bound, request.instrument_)
    ) {
      return unknown("unresolved", "recovery marked this instrument and the listing could not be read");
    }
    return refuse(
      "order_list_unread",
      "the working-order listing could not be read" +
        (source.last_error() ? ": " + once_detail::described(*source.last_error()) : std::string())
    );
  }
  if (bound.ledger().requires_reconcile(request.instrument_)) {
    return unknown(
      "unresolved",
      "the journal left an instruction on this instrument the listing does not resolve yet; " + recovery.detail_
    );
  }
  if (once_detail::owes_unknown(bound, request.instrument_)) {
    return unaccounted();
  }
  if (!once_detail::answered(source.poll(PollKind::HOLDINGS, bound))) {
    return refuse(
      "holdings_unread", source.last_error() ? once_detail::described(*source.last_error()) : std::string()
    );
  }
  if constexpr (Session::Caps::has_buying_power) {
    source.poll(PollKind::BUYING_POWER, bound);
  }
  if constexpr (MarketSession::Caps::has_price_limits) {
    source.poll(PollKind::PRICE_LIMITS, bound);
  }
  source.poll(PollKind::QUOTES, bound);
  if constexpr (MarketSession::Caps::has_calendar) {
    source.poll(PollKind::CALENDAR, bound);
  }
  const auto at = now();
  once_detail::arm(bound, at, config.maximum_portfolio_age_);
  const auto window = atl::strategy::SessionWindow::krx();
  const auto phase = window.phase(at, bound.context().calendar().is_trading_day(window.local_date(at)));
  bound.context().set_phase(phase);
  result.phase_ = std::string(atl::strategy::market_phase_name(phase));
  // 10. Outside a permitted phase, refused before the book is judged; a withdrawal is let through.
  if (
    const auto& permitted = config.limits_.permitted_phases_;
    !config.withdraw_only_ && permitted && std::find(permitted->begin(), permitted->end(), phase) == permitted->end()
  ) {
    return refuse(
      "phase", "the market is " + result.phase_ + "; the operator's limits trade only in a permitted phase"
    );
  }
  const auto& book = bound.context().portfolio();

  // 6. An order still owed on the instrument, after this call's listing, history and holdings.
  bound.refresh_unconfirmed();
  if (const auto failure = bound.local_failure()) {
    return refuse(
      "journal_unwritable",
      "a confirmation could not be written to the journal before anything was sent (" + *failure +
        "); the next cmdatl account shows what the journal holds"
    );
  }
  if (once_detail::owes_unknown(bound, request.instrument_)) {
    return unaccounted();
  }
  double reserved = 0;
  bool listed = false;
  for (const auto& working : book.working_) {
    if (!(working.order_.instrument_ == request.instrument_) || !atl::strategy::order_is_open(working.order_.status_)) {
      continue;
    }
    if (working.reserved_) {
      const double left =
        once_detail::number(working.order_.quantity_) - once_detail::number(working.order_.execution_.filled_quantity_);
      reserved += working.order_.side_ == atl::OrderSide::BUY ? left : -left;
    } else {
      listed = true;
    }
  }
  const auto owed = bound.obligations(request.instrument_);
  bool overdue = false;
  if (!owed.empty()) {
    auto oldest = owed.front().sent_at_;
    for (const auto& o : owed) {
      oldest = std::min(oldest, o.sent_at_);
    }
    overdue = at - oldest > config.confirmation_timeout_;
  }
  const auto waiting = [&](const std::string& detail) {
    if (overdue) {
      return unknown(
        "unconfirmed",
        "holdings have not accounted for an order on this instrument within the confirmation timeout: " +
          once_detail::owed_text(bound, request.instrument_)
      );
    }
    return refuse("awaiting_confirmation", detail + ": " + once_detail::owed_text(bound, request.instrument_));
  };
  if (!owed.empty() && !listed) {
    return waiting("an order on this instrument is not yet accounted for by holdings; call again after they show it");
  }

  // 7. A limit on the grid, inside the collar; a call that never submits carries none.
  std::optional<atl::strategy::Price> price;
  if (request.limit_) {
    const auto last = bound.context().last_price(request.instrument_);
    if (!last) {
      return refuse("no_price", "no last price to check the limit against");
    }
    const double mark = once_detail::number(*last);
    const double limit = once_detail::number(*request.limit_);
    if (outside_collar(limit, mark, config.collar_)) {
      return refuse(
        "collar",
        "limit " + request.limit_->str() + " is outside " + decimal_text(config.collar_) + " of the last price " +
          last->str()
      );
    }
    price = atl::strategy::Price::checked(*request.limit_, rule);
    if (!price) {
      return refuse("off_tick", "limit " + request.limit_->str() + " is not on the tick grid");
    }
  }

  // 8. With an order working on the instrument: met by it, or withdraw it and place on a later call.
  std::vector<decltype(book.working_.front().ref_)> working_refs;
  double committed = 0;
  for (const auto& working : book.working_) {
    if (
      working.reserved_ || !(working.order_.instrument_ == request.instrument_) ||
      !atl::strategy::order_is_open(working.order_.status_)
    ) {
      continue;
    }
    working_refs.push_back(working.ref_);
    const double left =
      once_detail::number(working.order_.quantity_) - once_detail::number(working.order_.execution_.filled_quantity_);
    committed += working.order_.side_ == atl::OrderSide::BUY ? left : -left;
  }
  const auto planned = [&](std::string kind, std::string detail) {
    result.plan_kind_ = std::move(kind);
    return done("plan", std::move(detail));
  };
  if (config.refuse_unjournaled_orders_ && !working_refs.empty()) {
    std::vector<std::string> named;
    for (const auto& line : journal.replay().entries_) {
      if (!line.order_id_.empty()) {
        named.push_back(line.order_id_);
      }
    }
    for (const auto& ref : working_refs) {
      if (std::find(named.begin(), named.end(), ref.primary_) == named.end()) {
        return refuse(
          "unjournaled_order",
          "order " + ref.primary_ + " on " + request.instrument_.symbol_ +
            " is not named by this account's journal; it was placed elsewhere"
        );
      }
    }
  }
  const auto logged_before = desk.sends();
  const bool failed_before = bound.local_failure().has_value();
  if (config.withdraw_only_) {
    if (working_refs.empty()) {
      return done("nothing_working", "the listing shows no order on this instrument");
    }
    if (config.dry_run_) {
      return planned("withdraw", "would withdraw " + std::to_string(working_refs.size()) + " working order(s)");
    }
    for (const auto& ref : working_refs) {
      result.withdrawn_.push_back(ref.primary_);
      bound.context().cancel(ref, "cmdatl");
    }
  } else if (!working_refs.empty()) {
    const auto held = book.net_quantity_of(request.instrument_);
    if (!held) {
      return refuse("holdings_unread", "the holding could not be netted");
    }
    const double target = once_detail::number(request.target_quantity_);
    if (reserved != 0 && once_detail::number(*held) + committed + reserved == target) {
      return waiting(
        "an order on this instrument is partly filled and holdings do not show it yet; call again after they do"
      );
    }
    if (reserved == 0 && once_detail::number(*held) + committed == target) {
      return done("met", "met by the holding and the orders working");
    }
    if (config.dry_run_) {
      return planned(
        "withdraw",
        "would withdraw " + std::to_string(working_refs.size()) +
          " working order(s) first; the placement waits for a later call"
      );
    }
    if (!armed()) {
      return refuse("not_armed", "the armed file was removed during the call");
    }
    for (const auto& ref : working_refs) {
      result.withdrawn_.push_back(ref.primary_);
      bound.context().cancel(ref, "cmdatl");
    }
  } else {
    if (config.dry_run_) {
      const auto held = book.net_quantity_of(request.instrument_);
      if (!held) {
        return refuse("holdings_unread", "the holding could not be netted");
      }
      const double delta = once_detail::number(request.target_quantity_) - once_detail::number(*held);
      if (delta == 0) {
        return done("met", "the holding is the target");
      }
      return planned(
        delta > 0 ? "buy" : "sell",
        std::string("would ") + (delta > 0 ? "buy " : "sell ") +
          std::to_string(static_cast<long long>(std::abs(delta))) +
          (request.limit_ ? " at " + request.limit_->str() + " (limit)" : std::string()) +
          ", subject to the risk limits at send"
      );
    }
    if (!armed()) {
      return refuse("not_armed", "the armed file was removed during the call");
    }
    if (!price) {
      throw std::logic_error("once() submits only with a limit");
    }
    atl::strategy::TargetPosition target;
    target.instrument_ = request.instrument_;
    target.target_quantity_ = request.target_quantity_;
    target.limit_ = *price;
    target.tag_ = "cmdatl";
    bound.context().submit(target);
  }

  // From here something may have been sent: any failure is an unknown.
  try {
    const auto report = bound.settle_closing();
    bound.clear_retained_targets();
    const auto logged = desk.sends() - logged_before;
    const bool failed = !failed_before && bound.local_failure().has_value();
    // Read right after the settle: the executor's error is its last request's, and the runner keeps the market's
    // last refusal.
    const auto& binding = bound.executor().last_error();
    const auto& broker = bound.last_broker_refusal();
    const auto mapped = map_report(
      report, logged, failed, result.withdrawn_.size(), binding ? once_detail::described(*binding) : std::string(),
      broker ? (broker->code_.empty() ? std::string() : "[" + broker->code_ + "] ") + broker->message_ : std::string()
    );
    if (mapped.reason_ == "placed") {
      PlacedOrder order;
      const auto& first = report.would_place_.front();
      order.side_ = first.side_ == atl::OrderSide::BUY ? "buy" : "sell";
      order.quantity_ = first.quantity_.str();
      order.limit_ = first.price_ ? first.price_->str() : std::string();
      for (const auto& working : bound.context().portfolio().working_) {
        if (working.reserved_ && working.order_.instrument_ == request.instrument_) {
          order.order_id_ = working.ref_.primary_;
          break;
        }
      }
      result.order_ = std::move(order);
    }
    if (mapped.exit_ == OnceExit::REFUSED) {
      // Nothing reached the market: no withdrawal was sent.
      result.withdrawn_.clear();
    }
    result.exit_ = mapped.exit_;
    result.reason_ = mapped.reason_;
    result.detail_ = mapped.detail_;
    return result;
  } catch (const std::exception& error) {
    return unknown("internal", error.what());
  }
}

/** @brief Why an approval file was refused, or its lines for one symbol. */
struct Approval {
  /** @brief The journal ids it names for the symbol. */
  std::vector<std::string> ids_;
  /** @brief Why it is refused, or empty. */
  std::string refused_;
};

/**
 * @brief Whether a text is a journal id an approval may name: 1-128 of `[A-Za-z0-9._:~-]`.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_journal_id(std::string_view text) {
  if (text.empty() || text.size() > 128) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
      c == ':' || c == '~' || c == '-';
  });
}

/**
 * @brief Reads an approval's text: each line exactly `SYMBOL JOURNAL_ID` with one space, `\n` or `\r\n` endings, no
 *   blank lines; the ids for one symbol.
 * @param text The text.
 * @param symbol The symbol.
 * @param valid_symbol The symbol grammar.
 * @return The ids, or why the text is refused (naming the line).
 */
inline Approval parse_approval(
  const std::string& text, const std::string& symbol, const std::function<bool(std::string_view)>& valid_symbol
) {
  Approval approval;
  std::size_t start = 0;
  std::size_t number = 0;
  while (start < text.size()) {
    ++number;
    const auto end = text.find('\n', start);
    if (end == std::string::npos) {
      approval.refused_ = "line " + std::to_string(number) + " has no line ending";
      return approval;
    }
    auto line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto space = line.find(' ');
    if (
      space == std::string::npos || line.find(' ', space + 1) != std::string::npos ||
      !valid_symbol(std::string_view(line).substr(0, space)) ||
      !valid_journal_id(std::string_view(line).substr(space + 1))
    ) {
      approval.refused_ = "line " + std::to_string(number) + " is not SYMBOL JOURNAL_ID with one space";
      return approval;
    }
    if (line.compare(0, space, symbol) == 0 && space == symbol.size()) {
      approval.ids_.push_back(line.substr(space + 1));
    }
  }
  if (number == 0) {
    approval.refused_ = "it is empty";
  }
  return approval;
}

/** @brief The largest approval file read, in bytes. */
inline constexpr std::size_t MAX_APPROVAL_BYTES = 64u << 10;

#if !defined(_WIN32)
/**
 * @brief Reads the operator's approval file through one descriptor: a regular file, at most 64 KiB, read to its size.
 * @param path The file.
 * @param text Where its bytes go.
 * @return Why it is refused, or empty.
 */
inline std::string read_approval(const std::string& path, std::string& text) {
  ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (fd.get() < 0) {
    return "the approval file " + path + " cannot be opened: " + std::strerror(errno);
  }
  const auto read = read_whole(fd.get(), MAX_APPROVAL_BYTES, text);
  if (read.status_ != FileRead::Status::OK) {
    return "the approval file " + path + " was not read: " + read.text(MAX_APPROVAL_BYTES);
  }
  return {};
}

/**
 * @brief The operator's settlement of every order the journal owes on one instrument, on the approval file that names
 *   exactly those orders: one confirmation line per order, sending nothing; the approval renamed after its use.
 * @tparam Desk The desk.
 * @param desk The desk.
 * @param journal The account's journal, opened by the caller.
 * @param lock The account's lock, held.
 * @param instrument The instrument.
 * @param reason Why the operator settles it.
 * @param armed_file The armed file.
 * @param approval_file The approval file.
 * @param valid_symbol The symbol grammar, for the approval's lines.
 * @return The outcome.
 */
template <typename Desk>
OnceResult release(
  Desk& desk, atl::runner::FileJournal& journal, const OwnerLock& lock, const atl::InstrumentId& instrument,
  const std::string& reason, const std::string& armed_file, const std::string& approval_file,
  const std::function<bool(std::string_view)>& valid_symbol
) {
  OnceResult result;
  const auto answer = [&result](OnceExit exit, std::string code, std::string detail = {}) {
    result.exit_ = exit;
    result.reason_ = std::move(code);
    result.detail_ = std::move(detail);
    return result;
  };
  std::error_code ec;
  if (armed_file.empty() || !std::filesystem::exists(armed_file, ec)) {
    return answer(OnceExit::REFUSED, "not_armed", "the armed file does not exist: " + armed_file);
  }
  std::string text;
  if (auto refused = read_approval(approval_file, text); !refused.empty()) {
    return answer(OnceExit::REFUSED, "not_approved", refused);
  }
  const auto approval = parse_approval(text, instrument.symbol_, valid_symbol);
  if (!approval.refused_.empty()) {
    return answer(OnceExit::REFUSED, "not_approved", "the approval file's " + approval.refused_);
  }
  if (!lock.still_at_path()) {
    return answer(OnceExit::UNKNOWN, "lock_lost", "the account's lock file was replaced; nothing was written");
  }
  if (!journal.open()) {
    return answer(OnceExit::UNKNOWN, "journal_damaged", "the journal is not open: " + journal.rejection());
  }
  const std::function<std::chrono::system_clock::time_point()> now = [&desk] {
    return desk.now();
  };
  auto bound = once_detail::bind(desk.session(), journal, atl::strategy::RiskLimits{}, now);
  const auto recovery = atl::runner::recover_runner(bound, journal);
  if (!once_detail::recovered(recovery, bound)) {
    return answer(
      OnceExit::UNKNOWN, "journal_damaged", recovery.detail_.empty() ? "the journal is damaged" : recovery.detail_
    );
  }
  const auto owed = bound.obligations(instrument);
  if (owed.empty()) {
    return answer(OnceExit::REFUSED, "nothing_owed", "the journal owes nothing on " + instrument.symbol_);
  }
  for (const auto& o : owed) {
    if (o.state_ == atl::runner::ObligationState::NAMED) {
      return answer(
        OnceExit::REFUSED, "named",
        "order " + o.order_id_ + " is named and waits for holdings: " + once_detail::owed_text(bound, instrument)
      );
    }
  }
  auto approved = approval.ids_;
  std::vector<std::string> wanted;
  for (const auto& o : owed) {
    wanted.push_back(o.journal_id_);
  }
  std::sort(approved.begin(), approved.end());
  std::sort(wanted.begin(), wanted.end());
  if (approved != wanted) {
    return answer(
      OnceExit::REFUSED, "not_approved",
      "the approval file does not name exactly the orders owed: " + once_detail::owed_text(bound, instrument)
    );
  }
  const auto released = once_detail::owed_text(bound, instrument);
  if (!bound.release_obligations(instrument, reason)) {
    return answer(
      OnceExit::UNKNOWN, "journal_write",
      bound.local_failure().value_or("a release could not be written") + "; some confirmations may be written"
    );
  }
  result.released_ = wanted;
  const auto used = approval_file + ".used-" + std::to_string(once_detail::nanos(std::chrono::system_clock::now()));
  if (sys::rename(approval_file.c_str(), used.c_str()) != 0) {
    result.approval_kept_ = true;
  }
  return answer(OnceExit::DONE, "released", released);
}
#endif

} // namespace sending
} // namespace cmdatl
