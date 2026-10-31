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
    {"interrupted", Exit::INTERRUPTED, true, "SIGINT or SIGTERM stopped the command before anything could be sent: run it again"},
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
    {"config.missing", Exit::FAILED, false, "set CMDATL_CONFIG to the operator's file"},
    {"config.invalid", Exit::FAILED, false, "fix the operator's file at the line and column given; cmdatl env shows it"},
    {"unsupported.broker", Exit::FAILED, false, "this version trades the paper broker only"},
    {"state.unavailable", Exit::FAILED, false, "set XDG_STATE_HOME or HOME, and check the account's directory"},
    {"busy.locked", Exit::FAILED, true, "another command holds the account: run it again when it is free"},
    {"unknown.lock_lost", Exit::UNKNOWN, false, "the account's lock file was replaced: find what replaced it, then cmdatl status"},
    {"unknown.journal_missing", Exit::UNKNOWN, false, "restore the journal from where it was; on paper, keep the account's directory for inspection and move it aside, which abandons that paper market with it"},
    {"unknown.journal_damaged", Exit::UNKNOWN, false, "restore the journal from where it was; on paper, keep the account's directory for inspection and move it aside, which abandons that paper market with it"},
    {"journal.unopened", Exit::FAILED, false, "check the account's directory and its permissions"},
    {"paper.data", Exit::FAILED, false, "fix the paper bars file the operator's file names"},
    {"unknown.paper_state", Exit::UNKNOWN, false, "restore paper.jsonl, or move the account's directory aside to start a fresh paper account"},
    {"quote.no_data", Exit::FAILED, false, "quote a symbol the paper bars have; cmdatl env shows the bars file"},
    {"clock.backwards", Exit::FAILED, false, "the clock only moves forward: cmdatl clock shows where it stands"},
    {"clock.past_data", Exit::FAILED, false, "the paper data ends there: cmdatl clock shows where"},
    {"unknown.clock_advance", Exit::UNKNOWN, false, "cmdatl clock shows where the clock stands; do not repeat the advance before reading it"},
    {"refused.symbol_not_allowed", Exit::REFUSED, false, "cmdatl env shows the operator's limits.symbols"},
    {"refused.position_limit", Exit::REFUSED, false, "cmdatl env shows the operator's limits.position"},
    {"refused.not_armed", Exit::REFUSED, true, "the operator creates the armed file cmdatl env names"},
    {"refused.daily_order_limit", Exit::REFUSED, true, "cmdatl env shows limits.daily_orders; the count starts again on the next KST date"},
    {"refused.order_value", Exit::REFUSED, false, "cmdatl env shows limits.order_value; plan a smaller change"},
    {"refused.no_plan", Exit::REFUSED, false, "cmdatl order plan gives a plan id"},
    {"refused.plan_used", Exit::REFUSED, false, "a plan sends once: cmdatl order plan again"},
    {"refused.stale_plan", Exit::REFUSED, false, "the account moved or the plan lapsed: cmdatl order plan again"},
    {"refused.phase", Exit::REFUSED, true, "cmdatl env shows limits.phases; on paper, cmdatl clock advance --next-open"},
    {"refused.collar", Exit::REFUSED, false, "cmdatl quote shows the last price; cmdatl env shows limits.collar"},
    {"refused.off_tick", Exit::REFUSED, false, "a limit on the market's tick grid"},
    {"refused.no_price", Exit::REFUSED, true, "cmdatl quote shows whether the symbol has a last price"},
    {"refused.reference_unread", Exit::REFUSED, true, "run the command again"},
    {"refused.order_list_unread", Exit::REFUSED, true, "run the command again"},
    {"refused.holdings_unread", Exit::REFUSED, true, "run the command again"},
    {"refused.awaiting_confirmation", Exit::REFUSED, true, "holdings have not shown an order yet: cmdatl order reconcile SYMBOL, then plan again"},
    {"refused.unjournaled_order", Exit::REFUSED, false, "an order no cmdatl journal line names rests on the symbol: the operator withdraws it where it was placed"},
    {"refused.guard", Exit::REFUSED, false, "a risk limit refused it: cmdatl env shows the limits"},
    {"refused.not_reached", Exit::REFUSED, true, "the paper client refused before writing the request (the armed file, the input rules, the lock, the log): read the message"},
    {"refused.withheld", Exit::REFUSED, true, "the runner withheld the plan (unknown or stale book, or a hold): cmdatl order reconcile SYMBOL, then plan again"},
    {"refused.journal_unwritable", Exit::REFUSED, false, "nothing reached the market; the next cmdatl account shows whether the unsent request was journaled"},
    {"refused.not_approved", Exit::REFUSED, false, "the operator writes the approval file beside the operator's file, one SYMBOL JOURNAL_ID line per owed order"},
    {"refused.nothing_owed", Exit::REFUSED, false, "cmdatl account shows what is owed"},
    {"refused.named", Exit::REFUSED, true, "the broker named the order: cmdatl order reconcile SYMBOL records it from holdings"},
    {"refused.awaiting", Exit::REFUSED, true, "an order is owed or resting: cmdatl clock advance on paper, then cmdatl order reconcile again"},
    {"refused.holding_moved", Exit::REFUSED, true, "holdings changed between two reads: cmdatl order reconcile again"},
    {"unknown.unaccounted", Exit::UNKNOWN, false, "only the operator settles it: read cmdatl order list and holdings, then cmdatl order release SYMBOL"},
    {"unknown.unresolved", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first; on paper the listing settles it after cmdatl clock advance --minutes 3"},
    {"unknown.unconfirmed", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first; then the operator reads the order list and holdings"},
    {"unknown.indeterminate", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first; cmdatl order list shows whether the order rests"},
    {"unknown.withdrawal", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first; an order may have filled"},
    {"unknown.mixed", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first"},
    {"unknown.unmapped", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first, then report the defect"},
    {"unknown.journal_write", Exit::UNKNOWN, false, "cmdatl order reconcile SYMBOL first; cmdatl account shows what the journal holds"},
    {"unknown.broker_refused", Exit::UNKNOWN, false, "the market refused it and nothing rests: cmdatl order reconcile SYMBOL shows nothing owed before planning again"},
  };
  return catalog;
}

} // namespace cmdatl
