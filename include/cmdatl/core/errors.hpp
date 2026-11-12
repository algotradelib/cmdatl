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
  "report the reply's exit and error to the operator, and start another run only when asked";

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
    {"spec.invalid", Exit::USAGE, false, "fix the run spec or the settings given on the command line"},
    {"build.failed", Exit::FAILED, false, "the strategy could not be built: read the build's messages in stderr"},
    {"replay.failed", Exit::FAILED, false, "the replay failed: check the data and the parameters' bounds"},
    {"strategy.name", Exit::USAGE, false, "a name is a letter, then letters, digits, - or _ (at most 64)"},
    {"strategy.template", Exit::USAGE, false, "cmdatl strategy new --help lists the templates"},
    {"strategy.exists", Exit::FAILED, false, "choose another NAME or --dir, or --force to replace regular files"},
    {"file.unwritable", Exit::FAILED, false, "check the directory and its permissions"},
    {"usage.bad_value", Exit::USAGE, false, "cmdatl COMMAND --help shows what each argument takes"},
    {"auth.no_store", Exit::FAILED, false,
      "a login needs the system's credential store: in this version, the macOS keychain only"},
    {"auth.store_locked", Exit::FAILED, false,
      "the operator can approve or unlock it from a local graphical login session: cmdatl auth status there once"},
    {"auth.not_found", Exit::FAILED, false, "cmdatl auth status lists the stored logins"},
    {"auth.exists", Exit::FAILED, false, "the operator can replace it with --force"},
    {"auth.bad_document", Exit::FAILED, false, "cmdatl help auth login shows each broker's fields"},
    {"auth.not_confirmed", Exit::FAILED, false,
      "a real login asks for real to be typed: only the operator can give it"},
    {"auth.no_answer", Exit::FAILED, false, "the operator can run cmdatl auth login again on a terminal"},
    {"config.missing", Exit::FAILED, false, "the operator sets CMDATL_CONFIG to their file"},
    {"config.invalid", Exit::FAILED, false,
      "the operator fixes their file at the line and column given; cmdatl env shows it"},
    {"adapter.not_found", Exit::FAILED, false,
      "no broker of that name: a built-in one, or a folder the operator installs in <config>/cmdatl/brokers"},
    {"adapter.invalid", Exit::FAILED, false, "the broker folder is refused as it stands: the operator fixes it"},
    {"adapter.unsafe", Exit::FAILED, false,
      "another user could change the broker folder: the operator makes it theirs and not writable by others"},
    {"fetch.failed", Exit::FAILED, false,
      "the broker folder's adapter did not serve the bars: check the login with cmdatl auth status, then fetch again"},
    {"unsupported.capability", Exit::FAILED, false,
      "the broker folder's client does not state what a run needs; nothing was sent: trade it another way"},
    {"unsupported.broker", Exit::FAILED, false,
      "the account and order commands trade the mock broker only; cmdatl run trades a KIS account"},
    {"state.unavailable", Exit::FAILED, false,
      "the operator sets XDG_STATE_HOME or HOME (another state directory is another mock account)"},
    {"busy.locked", Exit::FAILED, true, "another command holds the account: run it again when it is free"},
    {"unknown.lock_lost", Exit::UNKNOWN, false,
      "the account's lock file was replaced: stop and report to the operator, who finds what replaced it"},
    {"unknown.journal_missing", Exit::UNKNOWN, false,
      "stop and report to the operator, who restores the journal, or on the mock account keeps the account's directory for inspection and moves it aside, abandoning that mock market"},
    {"unknown.journal_damaged", Exit::UNKNOWN, false,
      "stop and report to the operator, who restores the journal, or on the mock account keeps the account's directory for inspection and moves it aside, abandoning that mock market"},
    {"journal.unopened", Exit::FAILED, false, "check the account's directory and its permissions"},
    {"mock.data", Exit::FAILED, false, "the operator fixes the mock bars file their file names"},
    {"unknown.mock_state", Exit::UNKNOWN, false,
      "stop and report to the operator, who restores the account's state file market.jsonl or moves the account's directory aside, abandoning that mock account"},
    {"quote.no_data", Exit::FAILED, false, "quote a symbol the mock bars have; cmdatl env shows the bars file"},
    {"clock.backwards", Exit::FAILED, false, "the clock only moves forward: cmdatl clock shows where it stands"},
    {"clock.past_data", Exit::FAILED, false, "the mock data ends there: cmdatl clock shows where"},
    {"unknown.clock_advance", Exit::UNKNOWN, false,
      "cmdatl clock shows where the clock stands: read it before any other command"},
    {"refused.symbol_not_allowed", Exit::REFUSED, false, "cmdatl env shows the operator's limits.symbols"},
    {"refused.position_limit", Exit::REFUSED, false, "cmdatl env shows the operator's limits.position"},
    {"refused.not_armed", Exit::REFUSED, false,
      "the armed file cmdatl env names is missing: only the operator creates it; ask the operator"},
    {"refused.orders_resting", Exit::REFUSED, false,
      "a run's withdraw cancels every order on the account, and only one an earlier run placed today on the run's "
      "symbols is the run's to withdraw: the operator cancels the others in the broker's app first"},
    {"unknown.owed_at_start", Exit::UNKNOWN, false,
      "the account's journal waits on an order on a symbol the run does not trade: ask the operator, who starts a run "
      "on that symbol to settle it from the broker's lists"},
    {"auth.account_mismatch", Exit::FAILED, false,
      "the login's account or environment is not the operator file's: fix [account] or log in again"},
    {"run.account_unreadable", Exit::FAILED, false, "nothing was sent; start the run again"},
    {"refused.daily_order_limit", Exit::REFUSED, false,
      "cmdatl env shows limits.daily_orders: more orders today are the operator's to allow; ask the operator"},
    {"refused.order_value", Exit::REFUSED, false,
      "cmdatl env shows limits.order_value: a larger order is the operator's to allow; ask the operator"},
    {"refused.no_plan", Exit::REFUSED, false, "cmdatl order plan gives a plan id"},
    {"refused.plan_used", Exit::REFUSED, false, "a plan sends once: cmdatl order plan again"},
    {"refused.stale_plan", Exit::REFUSED, false, "the account moved or the plan lapsed: cmdatl order plan again"},
    {"refused.phase", Exit::REFUSED, true,
      "cmdatl env shows limits.phases; on the mock account, when cmdatl status names nothing but phase or market_closed, cmdatl clock advance --next-open"},
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
      "the mock client refused before writing the request (the armed file, the input rules, the lock, the log): read the message"},
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
      "an order is owed or resting: cmdatl clock advance on the mock account, then cmdatl order reconcile again"},
    {"refused.holding_moved", Exit::REFUSED, true, "holdings changed between two reads: cmdatl order reconcile again"},
    {"unknown.unaccounted", Exit::UNKNOWN, false,
      "report to the operator, who reads the order list and holdings and runs cmdatl order release SYMBOL"},
    {"unknown.unresolved", Exit::UNKNOWN, false,
      "cmdatl order reconcile SYMBOL first; on the mock account the listing settles it after cmdatl clock advance --minutes 3"},
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
      "a run takes programs without on_fill, on_open or on_close, warming up on at most 10,000 bars; cmdatl strategy backtest replays the rest"},
    {"spec.no_instruments", Exit::USAGE, false,
      "name the symbols in the run spec's [instruments], as the data names them"},
    {"refused.order_type", Exit::REFUSED, false,
      "set [orders] type = \"limit\" and an offset within the operator's collar"},
    {"unsupported.interval", Exit::FAILED, false, "a run trades daily bars: give the run spec daily data"},
    {"run.failed", Exit::FAILED, true, "nothing was started or sent: run cmdatl run again"},
    {"run.reference_unreadable", Exit::FAILED, true, "nothing was sent: run cmdatl run again"},
    {"cache.unavailable", Exit::FAILED, false,
      "set XDG_CACHE_HOME to a writable absolute directory, or ask the operator"},
    {"unknown.owed_at_end", Exit::UNKNOWN, false, run_unknown_next()},
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
  };
  return catalog;
}

/**
 * @brief Every code `run` may answer, its own before the driver starts and the driver's: the table declares each on
 *   `run`.
 */
inline const std::vector<std::string> RUN_CODES{
  "usage.bad_value",
  "usage.missing_argument",
  "spec.invalid",
  "spec.no_instruments",
  "program.invalid",
  "program.live_unsupported",
  "unsupported.interval",
  "config.missing",
  "config.invalid",
  "unsupported.broker",
  "adapter.not_found",
  "adapter.invalid",
  "adapter.unsafe",
  "unsupported.capability",
  "auth.no_store",
  "auth.store_locked",
  "auth.not_found",
  "auth.account_mismatch",
  "state.unavailable",
  "refused.symbol_not_allowed",
  "refused.order_type",
  "refused.collar",
  "refused.not_armed",
  "refused.orders_resting",
  "refused.daily_order_limit",
  "busy.locked",
  "file.unwritable",
  "atlc.missing",
  "atlc.unsupported",
  "atlc.failed",
  "build.failed",
  "cache.unavailable",
  "run.failed",
  "run.reference_unreadable",
  "run.account_unreadable",
  "interrupted",
  "unknown.lock_lost",
  "unknown.journal_damaged",
  "unknown.owed_at_start",
  "unknown.recovery_halted",
  "unknown.closing_incomplete",
  "unknown.loop_abandoned",
  "unknown.local_failure",
  "unknown.owed_order",
  "unknown.owed_at_end",
  "unknown.market_halted",
  "unknown.internal",
  "internal.error",
};

} // namespace cmdatl
