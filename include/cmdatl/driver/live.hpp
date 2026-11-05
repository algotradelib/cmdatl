#pragma once

// A run's live wiring: the library's runner, journal, recovery, session source and polling loop composed in the order
// the runner requires, then a finalization stage. cmdatl's own code for cmdatl's behaviour, on the library's public
// API: it takes the run's journal as given (one writer per process), states the buying-power currencies, refuses a
// program with `on_fill` before recovery, calls back once just before the loop starts, and after the loop reads the
// broker until the runner has confirmed every order it sent, since a closing pass only sends its cancellations.

#include <algotradelib/live>
#include <algotradelib/mock>
#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <chrono>
#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;

/** @brief How the wiring ended: the loop's own endings, and the refusals before it started. */
enum class Outcome {
  /** @brief The loop ended, and any closing pass it made got every instruction out. */
  CLEAN,
  /** @brief A stop that closes ran and `closing_complete()` is false. */
  CLOSING_INCOMPLETE,
  /** @brief Recovery halted the runner; nothing was started. */
  RECOVERY_HALTED,
  /** @brief An instrument's reference data could not be read; nothing was started. */
  REFERENCE_UNREADABLE,
  /** @brief The program defines `on_fill`, which a live runner never calls; nothing was started. */
  ON_FILL_UNSUPPORTED,
  /** @brief The loop gave up after repeated poll failures. */
  LOOP_ABANDONED,
  /** @brief Local machinery failed during the run (usually a journal write) and the runner halted itself. */
  LOCAL_FAILURE,
  /** @brief The loop stopped placing by itself: an order stayed owed too long with nobody to account for it. */
  OWED_ORDER,
  /** @brief The callback before the loop asked not to start (its start line could not be written). */
  NOT_STARTED,
  /** @brief After the loop, the finalization stage could not show that nothing rests and nothing is owed. */
  OWED_AT_END,
};

/**
 * @brief The outcome as a word, for the run's summary.
 * @param outcome The outcome.
 * @return The word.
 */
inline const char* outcome_name(Outcome outcome) {
  switch (outcome) {
    case Outcome::CLEAN:
      return "clean";
    case Outcome::CLOSING_INCOMPLETE:
      return "closing_incomplete";
    case Outcome::RECOVERY_HALTED:
      return "recovery_halted";
    case Outcome::REFERENCE_UNREADABLE:
      return "reference_unreadable";
    case Outcome::ON_FILL_UNSUPPORTED:
      return "on_fill_unsupported";
    case Outcome::LOOP_ABANDONED:
      return "loop_abandoned";
    case Outcome::LOCAL_FAILURE:
      return "local_failure";
    case Outcome::OWED_ORDER:
      return "owed_order";
    case Outcome::NOT_STARTED:
      return "not_started";
    case Outcome::OWED_AT_END:
      return "owed_at_end";
  }
  return "unmapped";
}

/** @brief How many times the finalization stage reads the broker before it gives up. */
inline constexpr int FINAL_ROUNDS = 10;

/** @brief How far the market moves between two finalization reads. */
inline constexpr std::chrono::minutes FINAL_STEP{1};

/** @brief What the wiring is told: fixed by the driver from the operator's file and the market, never by a run's arguments. */
/** @brief One value of the program's `data {}`, published at `at_`, as the run's mock vendor releases it. */
struct DataPoint {
  /** @brief When it was published: the loop receives it at its first data poll at or after this. */
  std::chrono::system_clock::time_point at_;
  /** @brief The value's index in the program's `data {}`. */
  std::size_t name_{0};
  /** @brief The instrument it is for; empty for every instrument. */
  std::optional<atl::InstrumentId> instrument_;
  /** @brief The value; NaN is undefined. */
  double value_{0};
};

struct Wiring {
  /** @brief The run's instruments. */
  std::vector<atl::InstrumentId> universe_;
  /** @brief The limits the guard enforces (the rails' `risk_limits` for the run's symbols). */
  atl::strategy::RiskLimits limits_;
  /** @brief How often each kind is polled; every cadence the run needs is stated. */
  atl::runner::PollSchedule schedule_;
  /** @brief How many daily bars each bar read asks for: the first read is the history. */
  int bar_count_{1};
  /** @brief How old holdings and the listing may be when a decision is made. */
  std::chrono::nanoseconds maximum_portfolio_age_{std::chrono::seconds(30)};
  /** @brief How old a price band may be before the loop stops clamping to it; none, for a broker without bands. */
  std::optional<std::chrono::nanoseconds> maximum_price_limits_age_;
  /** @brief How long a closing stop waits for the listing and holdings to account for the run's own orders. */
  std::chrono::nanoseconds closing_wait_{std::chrono::seconds(60)};
  /** @brief The market's session window, which the loop derives dates and phases in. */
  atl::strategy::SessionWindow window_{atl::strategy::SessionWindow::krx()};
  /** @brief How long an order may stay owed with nobody able to account for it; the loop's own default, stated. */
  std::chrono::nanoseconds owed_order_limit_{std::chrono::minutes(10)};
  /** @brief The venue an order is sent to. */
  atl::Venue default_route_{atl::Venue::UNSPECIFIED};
  /** @brief The program's data values, read by the loop's data poll (`schedule_.data_`); empty without `data {}`. */
  std::vector<DataPoint> data_;
};

/** @brief How the wiring ended, and the loop's report when the loop ran. */
struct Traded {
  /** @brief The outcome. */
  Outcome outcome_{Outcome::CLEAN};
  /** @brief The loop's report, when it ran. */
  std::optional<atl::runner::LoopReport> report_;
  /** @brief What went wrong, in the library's words, when anything did. */
  std::string detail_;
  /** @brief How many finalization rounds ran. */
  int final_rounds_{0};
  /** @brief Targets the strategy did not submit because it could not price them (atlc's `pricing_refusals()`). */
  std::size_t pricing_refused_{0};
};

// Whether a strategy counts the targets it could not price, as atlc's generated strategies do.
template <typename Strategy, typename = void>
struct counts_pricing_refusals : std::false_type {};

template <typename Strategy>
struct counts_pricing_refusals<
  Strategy, std::void_t<decltype(std::declval<const Strategy&>().pricing_refusals().total())>
> : std::true_type {};

/**
 * @brief Trades a strategy on a session through its journal, in the order the runner requires: bind through the
 *   journal; refuse `on_fill`; recover; read each instrument's reference data; build the source on the journal with the
 *   bar query and the buying-power currencies; build and configure the loop; call `on_ready`; run; then finalize:
 *   close the session to placements and read holdings and the order list, one market minute apart, until the runner
 *   owes nothing, `nothing_rests` holds and no local failure stands; map the ending.
 *
 * **This places orders**, through the session (the run's placement boundary).
 * Exceptions from the clock (the bound, a halted market) and the session propagate: the caller treats them as an
 * unknown outcome.
 * @tparam Strategy The strategy type.
 * @tparam Session The session type, with `close()` stopping later placements.
 * @tparam Clock The clock: `now()`, `monotonic_now()` and `wait_for(duration)`.
 * @tparam Ready A callable returning `bool`: whether to start the loop (false ends with `NOT_STARTED`, nothing sent).
 * @tparam Rests A callable returning `bool`: whether the broker's own list shows no working order.
 * @tparam Closed A callable, called when the session has been closed to placements.
 * @param session The session.
 * @param journal The run's journal, already opened as the process's one writer.
 * @param strategy The strategy, configured.
 * @param wiring The settings.
 * @param stop The stop source the loop reads.
 * @param clock The clock, shared by the runner and the loop.
 * @param on_ready Called once, after recovery and reference data, just before the loop.
 * @param nothing_rests Whether the broker's own list shows no working order, read in the finalization stage.
 * @param on_closed Called once, right after the session is closed to placements.
 * @param err Where the recovery and the loop's report are printed for the operator.
 * @return How it ended.
 */
template <typename Strategy, typename Session, typename Clock, typename Ready, typename Rests, typename Closed>
Traded trade_on_rails(
  Session& session, atl::runner::FileJournal& journal, Strategy strategy, const Wiring& wiring,
  atl::runner::StopSource& stop, Clock clock, Ready&& on_ready, Rests&& nothing_rests, Closed&& on_closed,
  std::ostream& err
) {
  Traded traded;
  // One clock for the loop and the journal: lines are stamped, and recovery's sweep asks the broker about, the market's
  // times.
  atl::runner::ClockReference<Clock> shared_clock(clock);
  // Through the journal: without it an order in flight when the process dies leaves no trace.
  auto bound = atl::live::bind_journaled(
    session, journal, std::move(strategy), wiring.limits_, atl::live::default_series_capacity, wiring.default_route_,
    shared_clock
  );
  // A live runner never calls on_fill: such a program would silently skip its own logic. Refused before recovery,
  // since binding sent nothing.
  if constexpr (decltype(bound)::on_fill_never_fires) {
    traded.outcome_ = Outcome::ON_FILL_UNSUPPORTED;
    traded.detail_ = "the program defines on_fill, which a live runner never calls";
    return traded;
  }
  // Whatever a previous process left owed is withheld until the broker's lists resolve it.
  const auto recovery = atl::runner::recover_runner(bound, journal);
  err << "recovery: " << bound.obligations().size() << " placement(s) owed after restore\n";
  if (const auto failure = bound.local_failure()) {
    traded.outcome_ = Outcome::RECOVERY_HALTED;
    traded.detail_ = *failure;
    err << "HALTED: " << *failure << '\n';
    return traded;
  }
  (void)recovery;
  // Lot size and price grid, once: no order can be priced without them, and the loop does not poll them.
  for (const auto& instrument : wiring.universe_) {
    auto reference = session.instrument(instrument);
    if (!reference) {
      traded.outcome_ = Outcome::REFERENCE_UNREADABLE;
      traded.detail_ = "reference data for " + instrument.symbol_ + ": " + reference.error().message_;
      return traded;
    }
    bound.context().set_instrument(std::move(reference.value()));
  }
  // The journal resolves recovered instruments; the currencies let the guard read buying power.
  atl::live::SessionSource<Session, atl::runner::FileJournal> source(session, wiring.universe_);
  source.set_journal(journal);
  source.set_bar_query(atl::BarInterval::ONE_DAY, wiring.bar_count_);
  source.set_buying_power_currencies({atl::Currency::krw()});
  atl::runner::PollingLoop<decltype(bound), decltype(source), atl::runner::ClockReference<Clock>> loop(
    bound, source, wiring.schedule_, shared_clock
  );
  // Every age stated, and the market's own window. No control file: the driver stops on a signal.
  loop.set_maximum_portfolio_age(wiring.maximum_portfolio_age_);
  loop.set_maximum_price_limits_age(wiring.maximum_price_limits_age_);
  loop.set_closing_wait(wiring.closing_wait_);
  loop.set_owed_order_limit(wiring.owed_order_limit_);
  loop.set_session_window(wiring.window_);
  if constexpr (decltype(bound)::takes_records) {
    // The run's mock vendor: each value released at the first data poll at or after its time, stamped by the loop.
    using Record = typename decltype(bound)::DataRecord;
    std::vector<typename atl::mock::DataFeed<Record>::Entry> entries;
    for (const auto& point : wiring.data_) {
      Record record;
      record.name_ = point.name_;
      record.instrument_ = point.instrument_;
      record.value_ = point.value_;
      entries.emplace_back(point.at_, record);
    }
    loop.set_data_poll(atl::mock::DataFeed<Record>(std::move(entries)));
  }
  if (!on_ready()) {
    traded.outcome_ = Outcome::NOT_STARTED;
    traded.detail_ = "the run did not start";
    return traded;
  }
  traded.report_ = loop.run(stop, 0);
  const auto& report = *traded.report_;
  if constexpr (counts_pricing_refusals<std::decay_t<decltype(bound.strategy())>>::value) {
    traded.pricing_refused_ = static_cast<std::size_t>(bound.strategy().pricing_refusals().total());
  }
  err
    << "loop: " << report.iterations_ << " iteration(s), " << report.blocked_ << " blocked, "
    << report.orders_guard_refused_ << " refused by the guard, " << report.orders_binding_refused_
    << " refused before sending, final gate "
    << (report.final_gate_ ? atl::strategy::halt_reason_name(*report.final_gate_) : std::string_view("none")) << '\n';
  // A failed journal write is the most urgent: what the runner was sending is unknown.
  if (const auto failure = bound.local_failure()) {
    traded.outcome_ = Outcome::LOCAL_FAILURE;
    traded.detail_ = *failure;
    err << "LOCAL FAILURE: " << *failure << '\n';
    return traded;
  }
  if (report.abandoned_) {
    traded.outcome_ = Outcome::LOOP_ABANDONED;
    return traded;
  }
  const bool closing = report.stopped_by_ && atl::runner::stop_intent_closes(*report.stopped_by_);
  if (closing && !report.closing_complete()) {
    traded.outcome_ = Outcome::CLOSING_INCOMPLETE;
    return traded;
  }
  if (report.owed_stop_) {
    traded.outcome_ = Outcome::OWED_ORDER;
    return traded;
  }
  // Finalization: a closing pass sends its cancellations and confirms nothing, so the journal still owes a withdrawn
  // order until the history is read. Nothing is placed from here on.
  session.close();
  on_closed();
  const auto settled = [&]() {
    return bound.obligations().empty() && nothing_rests() && !bound.local_failure();
  };
  while (!settled() && traded.final_rounds_ < FINAL_ROUNDS && !bound.local_failure()) {
    ++traded.final_rounds_;
    shared_clock.wait_for(FINAL_STEP);
    (void)source.poll(atl::runner::PollKind::HOLDINGS, bound);
    (void)source.poll(atl::runner::PollKind::WORKING_ORDERS, bound);
  }
  err << "finalization: " << traded.final_rounds_ << " round(s), " << bound.obligations().size() << " owed\n";
  if (const auto failure = bound.local_failure()) {
    traded.outcome_ = Outcome::LOCAL_FAILURE;
    traded.detail_ = *failure;
    err << "LOCAL FAILURE: " << *failure << '\n';
    return traded;
  }
  if (!settled()) {
    traded.outcome_ = Outcome::OWED_AT_END;
    traded.detail_ = std::to_string(bound.obligations().size()) + " order(s) owed, " +
      (nothing_rests() ? "none resting" : "some resting") + " after " + std::to_string(traded.final_rounds_) +
      " finalization round(s)";
    return traded;
  }
  traded.outcome_ = Outcome::CLEAN;
  return traded;
}

} // namespace driver
} // namespace cmdatl
