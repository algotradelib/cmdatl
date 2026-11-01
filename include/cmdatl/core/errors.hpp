#pragma once

#include "./reply.hpp"

#include <string>
#include <vector>

namespace cmdatl {

/** @brief One error code: what it means for the caller. */
struct ErrorSpec {
  /** @brief The code. */
  std::string code_;
  /** @brief The exit code it comes with. */
  Exit exit_;
  /** @brief Whether the same call may succeed later unchanged. */
  bool retry_;
  /** @brief What to do. */
  std::string next_;
};

/** @brief What a run whose outcome is unknown tells its caller to do: the operator decides whether another starts. */
inline constexpr const char* RUN_END =
  "report run_exit and run_error to the operator, and start another run only when asked";

/**
 * @brief The `next` of a run's own reply when its outcome is unknown (the driver's), for any code.
 * @return The text.
 */
inline std::string run_unknown_next() {
  return std::string("the run's result is unknown; its market ended with its process and no account was affected: ") +
    RUN_END;
}

/**
 * @brief Every error code cmdatl answers with.
 * @return The catalog.
 */
inline const std::vector<ErrorSpec>& error_catalog() {
  static const std::vector<ErrorSpec> catalog{
    {"usage.unknown_command", Exit::USAGE, false, "cmdatl --help lists the commands"},
    {"usage.missing_argument", Exit::USAGE, false, "cmdatl COMMAND --help shows its arguments"},
    {"usage.bad_option", Exit::USAGE, false, "cmdatl COMMAND --help shows its options"},
    {"usage.bad_format", Exit::USAGE, false, "--format is json, jsonl or text"},
    {"usage.no_such_field", Exit::USAGE, false, "cmdatl schema COMMAND lists each command's fields"},
    {"unavailable.not_yet", Exit::FAILED, false, "this command is planned but not built in this version"},
    {"internal.error", Exit::FAILED, false, "a defect in cmdatl: report it with the command line"},
    {"unknown.internal", Exit::UNKNOWN, false, "a defect in cmdatl after it may have sent: reconcile, then report it"},
    {"unavailable.platform", Exit::FAILED, false, "this command does not run on this platform yet"},
    {"interrupted", Exit::INTERRUPTED, true,
      "SIGINT or SIGTERM stopped the command before anything could be sent: run it again"},
    {"atlc.missing", Exit::FAILED, false, "install atlc, or name it with CMDATL_ATLC; cmdatl env shows what was found"},
    {"atlc.unsupported", Exit::FAILED, false, "install a newer atlc"},
    {"atlc.failed", Exit::FAILED, false, "atlc ended unexpectedly: read stderr, and check atlc with cmdatl env"},
    {"program.invalid", Exit::PROGRAM, false, "fix the program (atlc check FILE.atls checks it alone)"},
    {"spec.invalid", Exit::USAGE, false, "fix the run spec"},
    {"build.failed", Exit::FAILED, false, "atlc could not build the strategy: read the build log stderr names"},
    {"replay.failed", Exit::FAILED, false, "the replay failed: check the data and the parameters' bounds"},
    {"strategy.name", Exit::USAGE, false, "a name is a letter, then letters, digits, - or _ (at most 64)"},
    {"strategy.template", Exit::USAGE, false, "cmdatl strategy new --help lists the templates"},
    {"strategy.exists", Exit::FAILED, false, "choose another NAME or --dir, or --force to replace regular files"},
    {"file.unwritable", Exit::FAILED, false, "check the directory and its permissions"},
    {"usage.bad_value", Exit::USAGE, false, "cmdatl COMMAND --help shows what each argument takes"},
    {"config.missing", Exit::FAILED, false, "the operator sets CMDATL_CONFIG to their file"},
    {"config.invalid", Exit::FAILED, false,
      "the operator fixes their file at the line and column given; cmdatl env shows it"},
    {"unsupported.broker", Exit::FAILED, false, "this version trades the paper broker only"},
    {"state.unavailable", Exit::FAILED, false,
      "the operator sets XDG_STATE_HOME or HOME (another state directory is another paper account)"},
    {"busy.locked", Exit::FAILED, true, "another command holds the account: run it again when it is free"},
    {"unknown.lock_lost", Exit::UNKNOWN, false,
      "the account's lock file was replaced: stop and report to the operator, who finds what replaced it"},
    {"unknown.journal_missing", Exit::UNKNOWN, false,
      "stop and report to the operator, who restores the journal, or on paper keeps the account's directory for inspection and moves it aside, abandoning that paper market"},
    {"unknown.journal_damaged", Exit::UNKNOWN, false,
      "stop and report to the operator, who restores the journal, or on paper keeps the account's directory for inspection and moves it aside, abandoning that paper market"},
    {"journal.unopened", Exit::FAILED, false, "check the account's directory and its permissions"},
    {"paper.data", Exit::FAILED, false, "the operator fixes the paper bars file their file names"},
    {"unknown.paper_state", Exit::UNKNOWN, false,
      "stop and report to the operator, who restores paper.jsonl or moves the account's directory aside, abandoning that paper account"},
    {"quote.no_data", Exit::FAILED, false, "quote a symbol the paper bars have; cmdatl env shows the bars file"},
    {"clock.backwards", Exit::FAILED, false, "the clock only moves forward: cmdatl clock shows where it stands"},
    {"clock.past_data", Exit::FAILED, false, "the paper data ends there: cmdatl clock shows where"},
    {"unknown.clock_advance", Exit::UNKNOWN, false,
      "cmdatl clock shows where the clock stands; do not repeat the advance before reading it"},
    {"refused.symbol_not_allowed", Exit::REFUSED, false, "cmdatl env shows the operator's limits.symbols"},
    {"refused.position_limit", Exit::REFUSED, false, "cmdatl env shows the operator's limits.position"},
    {"refused.not_armed", Exit::REFUSED, false,
      "the armed file cmdatl env names is missing: only the operator creates it; ask the operator"},
    {"refused.daily_order_limit", Exit::REFUSED, false,
      "cmdatl env shows limits.daily_orders: more orders today are the operator's to allow; ask the operator"},
    {"refused.order_value", Exit::REFUSED, false,
      "cmdatl env shows limits.order_value: a larger order is the operator's to allow; ask the operator"},
    {"refused.no_plan", Exit::REFUSED, false, "cmdatl order plan gives a plan id"},
    {"refused.plan_used", Exit::REFUSED, false, "a plan sends once: cmdatl order plan again"},
    {"refused.stale_plan", Exit::REFUSED, false, "the account moved or the plan lapsed: cmdatl order plan again"},
    {"refused.phase", Exit::REFUSED, true,
      "cmdatl env shows limits.phases; on paper, when the phase is the only gate cmdatl status names, cmdatl clock advance --next-open"},
    {"refused.collar", Exit::REFUSED, false, "cmdatl quote shows the last price; cmdatl env shows limits.collar"},
    {"refused.off_tick", Exit::REFUSED, false, "a limit on the market's tick grid"},
    {"refused.no_price", Exit::REFUSED, true, "cmdatl quote shows whether the symbol has a last price"},
    {"refused.reference_unread", Exit::REFUSED, true, "run the command again"},
    {"refused.order_list_unread", Exit::REFUSED, true, "run the command again"},
    {"refused.holdings_unread", Exit::REFUSED, true, "run the command again"},
    {"refused.awaiting_confirmation", Exit::REFUSED, true,
      "holdings have not shown an order yet: place nothing until cmdatl order reconcile SYMBOL shows the symbol clean"},
    {"refused.unjournaled_order", Exit::REFUSED, false,
      "an order no cmdatl journal line names rests on the symbol: the operator withdraws it where it was placed"},
    {"refused.guard", Exit::REFUSED, false, "a risk limit refused it: cmdatl env shows the limits"},
    {"refused.not_reached", Exit::REFUSED, true,
      "the paper client refused before writing the request (the armed file, the input rules, the lock, the log): read the message"},
    {"refused.withheld", Exit::REFUSED, true,
      "the runner withheld the plan (unknown or stale book, or a hold): cmdatl order reconcile SYMBOL, then plan again"},
    {"refused.journal_unwritable", Exit::REFUSED, false,
      "nothing reached the market; the next cmdatl account shows whether the unsent request was journaled"},
    {"refused.not_approved", Exit::REFUSED, false,
      "the operator writes the approval file beside the operator's file, one SYMBOL JOURNAL_ID line per owed order"},
    {"refused.nothing_owed", Exit::REFUSED, false, "cmdatl account shows what is owed"},
    {"refused.named", Exit::REFUSED, true,
      "the broker named the order: cmdatl order reconcile SYMBOL records it from holdings"},
    {"refused.awaiting", Exit::REFUSED, true,
      "an order is owed or resting: cmdatl clock advance on paper, then cmdatl order reconcile again"},
    {"refused.holding_moved", Exit::REFUSED, true, "holdings changed between two reads: cmdatl order reconcile again"},
    {"unknown.unaccounted", Exit::UNKNOWN, false,
      "report to the operator, who reads the order list and holdings and runs cmdatl order release SYMBOL"},
    {"unknown.unresolved", Exit::UNKNOWN, false,
      "cmdatl order reconcile SYMBOL first; on paper the listing settles it after cmdatl clock advance --minutes 3"},
    {"unknown.unconfirmed", Exit::UNKNOWN, false,
      "cmdatl order reconcile SYMBOL first; then the operator reads the order list and holdings"},
    {"unknown.indeterminate", Exit::UNKNOWN, false,
      "cmdatl order reconcile SYMBOL first; cmdatl order list shows whether the order rests"},
    {"unknown.withdrawal", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first; an order may have filled"},
    {"unknown.mixed", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first"},
    {"unknown.unmapped", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first, then report the defect"},
    {"unknown.journal_write", Exit::UNKNOWN, false,
      "cmdatl order reconcile SYMBOL first; cmdatl account shows what the journal holds"},
    {"unknown.broker_refused", Exit::UNKNOWN, false,
      "the market refused it and nothing rests: cmdatl order reconcile SYMBOL shows nothing owed; do not place again in the same phase, report it"},
    {"program.live_unsupported", Exit::PROGRAM, false,
      "a run takes programs without on_fill or session legs, warming up on at most 10,000 bars; cmdatl strategy backtest replays the rest"},
    {"spec.no_instruments", Exit::USAGE, false,
      "name the symbols in the run spec's [instruments], as the data names them"},
    {"refused.order_type", Exit::REFUSED, false,
      "set [orders] type = \"limit\" and an offset within the operator's collar"},
    {"unsupported.interval", Exit::FAILED, false, "a run trades daily bars: give the run spec daily data"},
    {"run.unknown", Exit::FAILED, false, "cmdatl run start starts a run; its reply names the run's id"},
    {"run.failed", Exit::FAILED, true, "nothing was started or sent: run cmdatl run start again"},
    {"run.reference_unreadable", Exit::FAILED, true, "nothing was sent: run cmdatl run start again"},
    {"cache.unavailable", Exit::FAILED, false, "set XDG_CACHE_HOME or HOME to a writable absolute directory"},
    {"unknown.run_start", Exit::UNKNOWN, false, "the run may be trading: cmdatl run status RUN; do not start it again"},
    {"unknown.run_ended", Exit::UNKNOWN, false,
      std::string("the run ended without a reply; its market ended with it and no account was affected: ") + RUN_END},
    {"unknown.owed_at_end", Exit::UNKNOWN, false,
      std::string("the run's result is unknown; its market ended with its process and no account was affected: ") +
        RUN_END},
    {"unknown.recovery_halted", Exit::UNKNOWN, false,
      std::string("the run's journal halted recovery and nothing was started: ") + RUN_END},
    {"unknown.closing_incomplete", Exit::UNKNOWN, false,
      std::string("the run could not confirm its withdrawal; its market ended with it and no account was affected: ") +
        RUN_END},
    {"unknown.loop_abandoned", Exit::UNKNOWN, false,
      std::string(
        "the run gave up after repeated failed reads; its market ended with it and no account was affected: "
      ) +
        RUN_END},
    {"unknown.local_failure", Exit::UNKNOWN, false,
      std::string("a journal write failed during the run; its market ended with it and no account was affected: ") +
        RUN_END},
    {"unknown.owed_order", Exit::UNKNOWN, false,
      std::string("the run stopped placing on an order nobody could account for; its market ended with it: ") +
        RUN_END},
    {"unknown.market_halted", Exit::UNKNOWN, false,
      std::string(
        "the mock market could not cost or record a fill; its market ended with it and no account was affected: "
      ) +
        RUN_END},
    {"unknown.run_stopping", Exit::UNKNOWN, false, "the run stops placing: cmdatl run status RUN shows when it ended"},
    {"unknown.run_stop", Exit::UNKNOWN, false,
      "it may still be trading: cmdatl run status RUN; the operator can remove the armed file, which stops every run"},
    {"refused.stop_file_full", Exit::REFUSED, false,
      "the stop was not written and the run still trades: the stop file is full of other runs' lines; wait for them to end, or ask the operator, who can remove the armed file, which stops every run"},
    {"run.log_replaced", Exit::FAILED, false,
      "the run's event log was replaced or cut while it was read: cmdatl run status RUN"},
    {"run.log_unreadable", Exit::FAILED, false,
      "the run started and its event log is missing or not a file that can be read: cmdatl run status RUN"},
  };
  return catalog;
}

/**
 * @brief Every code `run start` may answer, its own and the run's driver's, which it relays: the table declares each
 *   on `run start`, and `run status` reports the driver's as `run_error`.
 */
inline const std::vector<std::string> RUN_CODES{
  "usage.bad_value",
  "spec.invalid",
  "spec.no_instruments",
  "program.invalid",
  "program.live_unsupported",
  "unsupported.interval",
  "config.missing",
  "config.invalid",
  "unsupported.broker",
  "refused.symbol_not_allowed",
  "refused.order_type",
  "refused.collar",
  "refused.not_armed",
  "busy.locked",
  "file.unwritable",
  "atlc.missing",
  "atlc.unsupported",
  "atlc.failed",
  "build.failed",
  "cache.unavailable",
  "run.failed",
  "run.reference_unreadable",
  "interrupted",
  "unknown.lock_lost",
  "unknown.journal_damaged",
  "unknown.recovery_halted",
  "unknown.closing_incomplete",
  "unknown.loop_abandoned",
  "unknown.local_failure",
  "unknown.owed_order",
  "unknown.owed_at_end",
  "unknown.market_halted",
  "unknown.run_start",
  "unknown.run_ended",
  "unknown.internal",
  "internal.error",
};

} // namespace cmdatl
