#pragma once

#include "../account/reads.hpp"
#include "../core/table.hpp"
#include "../orders/orders.hpp"
#include "../strategy/strategy.hpp"
#include "./describe.hpp"
#include "./environment.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace cmdatl {

/** @brief The exit codes of a command that sends nothing. */
inline const std::vector<Exit> READ_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE};
/** @brief The exit codes of a command that may send, or that plans or reconciles what was sent. */
inline const std::vector<Exit> SEND_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::REFUSED, Exit::UNKNOWN};
/** @brief The exit codes of a command that runs atlc on a program, and stops it on SIGINT or SIGTERM. */
inline const std::vector<Exit> PROGRAM_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::PROGRAM, Exit::INTERRUPTED};

/** @brief `-P k=v`, shared by the commands that run a program. */
inline const OptionSpec PARAMETER{"param", "k=v", "set a program parameter for this call", false, true, "P"};

/**
 * @brief Every command, in the order `cmdatl --help` lists them.
 *
 * Each command is defined once, here; the parser, the help and the schema are built from this list, so renaming
 * or reshaping a command is one entry (keep the old path with `alias` while the interface is experimental).
 *
 * @return The commands.
 */
inline std::vector<Command> command_list() {
  std::vector<Command> list;
  const auto add = [&list](Command& command) {
    if (command.exits_.empty()) {
      command.exits_ = command.may_send_ ? SEND_EXITS : READ_EXITS;
    }
    if (command.available_ && command.path_.size() == 2 && command.path_[0] == "order" && command.path_[1] != "list") {
      for (const auto& code : order_error_codes()) {
        command.error(code);
      }
    }
    list.push_back(command);
  };

  add(make_command({"status"}, "Read", "clock, phase, readiness, positions, cash, owed and working orders, quotes")
    .example("cmdatl status")
    .example("cmdatl status --fields now,orders_accepted_now,not_accepted")
    .returns("now", "the account's time, KST with its offset")
    .returns("phase", "the market phase: pre_open, open, post_close, closed or unknown")
    .returns("armed", "whether the operator's armed file exists")
    .returns("orders_accepted_now", "whether the preliminary gates of a placement pass now (a place still checks more)")
    .returns("not_accepted", "the gates that fail, in a fixed order; empty exactly when orders_accepted_now")
    .returns("orders_left_today", "placements the operator still allows today; null when the journal is damaged")
    .returns("positions", "holdings: symbol, quantity, sellable, average_price, last_price, market_value")
    .returns("cash", "the paper market's cash after every fill's value, commission and tax")
    .returns("buying_power", "the broker's buying power")
    .returns("owed", "orders the journal still waits on")
    .returns("orders", "working orders")
    .returns("quotes", "each allowed symbol, as quote's rows")
    .returns("data_end", "the last session's close in the paper data")
    .returns("capabilities", "what the account's broker offers")
    .returns("state_dir", "the account's state directory")
    .on_account([](const Invocation& invocation, Account& account, Output& output) {
      return status_reply(invocation, account, account.desk(), output);
    }));
  add(make_command({"account"}, "Read", "positions, cash, buying power, owed orders, orders left today")
    .example("cmdatl account")
    .example("cmdatl account --value cash")
    .example("cmdatl account --symbol 005930 --fields positions.quantity")
    .option("symbol", "SYMBOL", "only this instrument's rows")
    .returns("now", "the account's time, KST with its offset")
    .returns("positions", "holdings: symbol, quantity, sellable, average_price, last_price, market_value")
    .returns("cash", "the paper market's cash after every fill's value, commission and tax")
    .returns("buying_power", "the broker's buying power")
    .returns("owed", "orders the journal still waits on: symbol, side, quantity, journal_id, state, order_id, broker_status, next")
    .returns("orders_left_today", "placements the operator still allows today; null when the journal is damaged")
    .error("usage.bad_value")
    .on_account(
      [](const Invocation& invocation, Account& account, Output& output) {
        return account_reply(invocation, account, account.desk(), output);
      },
      check_symbols
    ));
  add(make_command({"quote"}, "Read", "last price, price band, and whether orders are accepted now")
    .example("cmdatl quote 005930 000660")
    .arg("SYMBOL", "an instrument", true, true)
    .returns("now", "the account's time, KST with its offset")
    .returns("phase", "the market phase")
    .returns(
      "quotes",
      "one per symbol: symbol, last_price, upper_limit, lower_limit, symbol_allowed, orders_accepted_now, not_accepted"
    )
    .error("quote.no_data")
    .error("usage.bad_value")
    .on_account(
      [](const Invocation& invocation, Account& account, Output& output) {
        return quote_reply(invocation, account, account.desk(), output);
      },
      check_symbols
    ));
  add(make_command({"env"}, "Read", "configuration: cmdatl, algotradelib, atlc, the operator's file (no secrets)")
    .example("cmdatl env")
    .example("cmdatl env --value atlc.ok")
    .returns("cmdatl", "cmdatl's version")
    .returns("algotradelib", "the library's version")
    .returns("atlc", "requested, path, source, version, minimum, ok, and error when the operator's file was not read")
    .returns("config", "the operator's file: path, exists, state (unset, missing, invalid, ok), and error")
    .returns("account", "broker, env and number, or null")
    .returns("limits", "every limit, defaults filled (the collar as a decimal string), or null")
    .returns("armed", "the armed file: path, exists")
    .returns("state_dir", "the account's state directory")
    .returns("owner", "who last held the account, or null")
    .returns("capabilities", "what the account's broker offers")
    .returns("paper", "the paper market: bars, start, cash, costs (commission and sell tax as decimal strings)")
    .returns("schema_version", "the version of cmdatl schema's format")
    .returns("modes", "the trading modes this build offers")
    .runs([](const Invocation&, const Table&, Output&) { return env_reply(); }));

  add(make_command({"order", "plan"}, "Trade", "what would be sent; gives a plan id")
    .example("cmdatl order plan 005930 --target 10 --limit 78200")
    .example("cmdatl order plan 005930 --target 0 --limit 79000")
    .describe(
      "What `order place` would send to move SYMBOL to N shares with a limit at P, checked against the\n"
      "operator's limits, the market phase and the account. Sends nothing. The plan id expires when the\n"
      "account changes or after the operator's plan lifetime. Each order command takes one simulated second."
    )
    .arg("SYMBOL", "an instrument the operator allows")
    .option("target", "N", "the position to hold, whole shares (0 to sell out)", true)
    .option("limit", "P", "limit price, on the tick grid, inside the operator's collar", true)
    .returns("now", "the account's time after the command's simulated second, KST with its offset")
    .returns("symbol", "the instrument")
    .returns("target", "the position asked for")
    .returns("limit", "the limit price")
    .returns("phase", "the market phase the decision was made in")
    .returns("reason", "plan or met; on a refusal, once()'s reason")
    .returns("detail", "what the plan would do")
    .returns("held", "the position now")
    .returns("plan_id", "the plan, for order place")
    .returns("would", "buy, sell or withdraw")
    .returns("quantity", "shares, for a buy or a sell")
    .returns("expires_at", "when the plan id lapses, KST with its offset")
    .returns("next", "the command to run next")
    .returns("owed", "orders the journal still waits on, for this symbol")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .on_account(plan_reply, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "place"}, "Trade", "send that plan, if nothing moved since")
    .example("cmdatl order place p9cc4b94da4")
    .arg("PLAN_ID", "a plan id from order plan")
    .returns("now", "the account's time after the command's simulated second, KST with its offset")
    .returns("plan_id", "the plan")
    .returns("symbol", "the instrument")
    .returns("target", "the position asked for")
    .returns("limit", "the limit price")
    .returns("phase", "the market phase the decision was made in")
    .returns("reason", "placed, met or withdrawal_sent; on a refusal or unknown, the reason")
    .returns("detail", "what happened")
    .returns("order", "the order sent: order_id (null until the market names it), side, quantity, limit")
    .returns("withdrawals", "the order ids a withdrawal was sent for")
    .returns("next", "the command to run next")
    .returns("owed", "orders the journal still waits on, for this symbol")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .sends()
    .on_account(place_reply, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "cancel"}, "Trade", "withdraw working orders on SYMBOL (never places)")
    .example("cmdatl order cancel 005930")
    .arg("SYMBOL", "an instrument")
    .returns("now", "the account's time after the command's simulated second, KST with its offset")
    .returns("symbol", "the instrument")
    .returns("phase", "the market phase")
    .returns("reason", "withdrawal_sent or nothing_working; on a refusal or unknown, the reason")
    .returns("detail", "what happened")
    .returns("withdrawals", "the order ids a withdrawal was sent for (sent, not confirmed)")
    .returns("owed", "orders the journal still waits on, for this symbol")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .error("quote.no_data")
    .sends()
    .on_account(cancel_reply, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "reconcile"}, "Trade", "record fills from holdings (never places)")
    .example("cmdatl order reconcile")
    .example("cmdatl order reconcile 005930")
    .arg("SYMBOL", "an instrument", false)
    .returns("now", "the account's time after the command's simulated second, KST with its offset")
    .returns("checked", "each symbol read: symbol, state (clean, working, holding_moved or a reason), detail")
    .returns("owed", "what is still owed, after the call")
    .returns("reason", "clean, awaiting, or the first row's reason")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .error("quote.no_data")
    .on_account(reconcile_reply, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "list"}, "Trade", "working orders")
    .example("cmdatl order list")
    .returns("now", "the account's time, KST with its offset")
    .returns("orders", "working orders: order_id, symbol, side, quantity, filled, price, status")
    .on_account([](const Invocation& invocation, Account& account, Output& output) {
      return order_list_reply(invocation, account, account.desk(), output);
    }));
  add(make_command({"order", "release"}, "Trade", "operator only: settle an unknown order")
    .example("cmdatl order release 005930 --reason \"checked the broker's order list\"")
    .arg("SYMBOL", "an instrument")
    .option("reason", "TEXT", "why the operator settles it", true)
    .returns("now", "the account's time after the command's simulated second, KST with its offset")
    .returns("symbol", "the instrument")
    .returns("reason", "released; on a refusal or unknown, the reason")
    .returns("detail", "what was released")
    .returns("released", "the journal ids settled")
    .returns("approval_kept", "whether the approval file could not be renamed after its use (harmless)")
    .returns("owed", "orders the journal still waits on, for this symbol")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .mutates("cmdatl account shows what is still owed; read it before releasing again")
    .on_account(release_reply, check_order_args)
    .exits(ORDER_EXITS));

  add(make_command({"strategy", "new"}, "Strategy", "a program and a run spec to start from")
    .example("cmdatl strategy new dip --template mean-reversion")
    .example("cmdatl strategy new dip --dir strategies --force")
    .arg("NAME", "the program's name: a letter, then letters, digits, - or _")
    .option("template", "T", "mean-reversion (the default), crossover, momentum or breakout")
    .option("dir", "DIR", "where to write them (the working directory by default)")
    .flag("force", "replace NAME.atls and NAME.toml if they are regular files")
    .returns("program", "the .atls file, absolute")
    .returns("spec", "the run spec, absolute")
    .returns("next", "the backtest to run once bars.csv is beside the spec, and atlc check for the program alone")
    .exits({Exit::OK, Exit::FAILED, Exit::USAGE, Exit::INTERRUPTED})
    .error("strategy.name")
    .error("strategy.template")
    .error("strategy.exists")
    .error("file.unwritable")
    .error("interrupted")
    .runs([](const Invocation& invocation, const Table&, Output&) { return new_reply(invocation); }));
  add(make_command({"strategy", "backtest"}, "Strategy", "replay a run spec with atlc and report")
    .example("cmdatl strategy backtest dip.toml")
    .example("cmdatl strategy backtest dip.toml -P lookback=30 --fills fills.csv")
    .arg("SPEC", "a run spec")
    .option(PARAMETER)
    .option("fills", "FILE", "also write every fill")
    .returns("report", "atlc's report, unchanged")
    .returns("spec", "the run spec, absolute")
    .returns("params_given", "the -P given (the spec's and the program's own values are not listed)")
    .returns("atlc", "atlc's version")
    .returns("fills", "the fills file, absolute, when asked")
    .returns("errors", "on a failure atlc spec found: atlc's diagnostics, with file and line")
    .returns("stderr", "on a failure atlc reported: its last stderr lines")
    .exits(PROGRAM_EXITS)
    .error("interrupted")
    .error("atlc.missing")
    .error("atlc.unsupported")
    .error("atlc.failed")
    .error("program.invalid")
    .error("spec.invalid")
    .error("build.failed")
    .error("replay.failed")
    .error("unavailable.platform")
    .error("config.missing")
    .error("config.invalid")
    .runs([](const Invocation& invocation, const Table&, Output& output) {
      return backtest_reply(invocation, output);
    }));
  add(make_command({"strategy", "reference"}, "Strategy", "the atls language and its built-ins")
    .example("cmdatl strategy reference")
    .example("cmdatl strategy reference indicators")
    .arg("TOPIC", "a chapter", false)
    .returns("text", "the reference"));

  add(make_command({"run", "start"}, "Run", "start a strategy trading an account, in the background")
    .example("cmdatl run start dip.toml --mode paper")
    .example("cmdatl run start dip.toml --mode paper -P lookback=30 --follow")
    .arg("SPEC", "a run spec")
    .option("mode", "M", "paper (demo and live are not available yet)", true)
    .option(PARAMETER)
    .flag("follow", "keep printing the run's events until it ends")
    .returns("run", "the run id")
    .returns("state", "running, refused or unknown")
    .exits({Exit::OK, Exit::FAILED, Exit::USAGE, Exit::REFUSED, Exit::UNKNOWN, Exit::PROGRAM})
    .sends()
    .account());
  add(make_command({"run", "status"}, "Run", "a run's state and summary")
    .example("cmdatl run status")
    .arg("RUN", "a run id", false)
    .returns("run", "the run id")
    .returns("state", "running, stopped or unknown")
    .returns("summary", "trades and position")
    .account());
  add(make_command({"run", "log"}, "Run", "a run's events")
    .example("cmdatl run log --follow")
    .arg("RUN", "a run id", false)
    .flag("follow", "keep printing until the run ends")
    .returns("events", "placed, filled, refused, heartbeat")
    .account());
  add(make_command({"run", "stop"}, "Run", "stop a run: quiesce, or also withdraw or flatten")
    .example("cmdatl run stop")
    .example("cmdatl run stop --flatten")
    .arg("RUN", "a run id", false)
    .flag("withdraw", "also cancel the account's working orders")
    .flag("flatten", "withdraw, then close the positions")
    .returns("state", "stopped or unknown")
    .returns("summary", "the final summary")
    .exits(SEND_EXITS)
    .sends()
    .account());

  add(make_command({"clock"}, "Paper account", "the paper account's time and phase")
    .example("cmdatl clock")
    .returns("now", "the account's time, KST with its offset")
    .returns("phase", "the market phase")
    .returns("data_end", "the last session's close in the paper data")
    .on_account([](const Invocation& invocation, Account& account, Output& output) {
      return clock_reply(invocation, account, account.desk(), output);
    }));
  add(make_command({"clock", "advance"}, "Paper account", "move the paper clock")
    .example("cmdatl clock advance --next-open")
    .example("cmdatl clock advance --minutes 30")
    .example("cmdatl clock advance --to 2024-03-26T09:30")
    .describe(
      "Moves the paper clock forward by exactly one of the options, filling or expiring resting orders on the way.\n"
      "Exit 4 means the advance may have happened: read cmdatl clock before advancing again."
    )
    .option("minutes", "N", "by N minutes, 1 to 20160")
    .option("hours", "N", "by N hours, 1 to 336")
    .option("to", "TIME", "to a KST time, YYYY-MM-DDTHH:MM")
    .flag("next-open", "to the next session's open in the data")
    .returns("from", "the time before, KST with its offset")
    .returns("now", "the time after")
    .returns("phase", "the market phase after")
    .returns("events", "what the market did: at, kind, order_id, symbol, side, quantity, price, commission, tax")
    .error("usage.bad_value")
    .error("clock.backwards")
    .error("clock.past_data")
    .error("unknown.clock_advance")
    .mutates("cmdatl clock shows where the clock stands; do not repeat the advance before reading it")
    .on_account(
      [](const Invocation& invocation, Account& account, Output& output) {
        return advance_reply(invocation, account, account.desk(), output);
      },
      check_advance
    ));

  add(make_command({"help"}, "Help", "help for cmdatl, a group or a command (as --help)")
    .example("cmdatl help")
    .example("cmdatl help order plan")
    .arg("COMMAND", "a command or group", false, true)
    .returns("text", "the help")
    .error("usage.unknown_command")
    .runs([](const Invocation& invocation, const Table& table, Output&) {
      const auto found = invocation.values_.find("COMMAND");
      const auto words = found == invocation.values_.end() ? std::vector<std::string>{} : found->second;
      std::ostringstream text;
      if (!write_help_for(text, table, words)) {
        const auto suggestions = table.suggest(words);
        return error_reply(
          Exit::USAGE,
          "usage.unknown_command",
          "no command " + join(words),
          suggestions.empty() ? "cmdatl help lists the commands" : "did you mean: " + join(suggestions, ", ")
        );
      }
      Reply reply;
      reply.text_ = text.str();
      return reply;
    }));
  add(make_command({"schema"}, "Help", "the machine contract: commands, fields, exit and error codes")
    .example("cmdatl schema")
    .example("cmdatl schema order")
    .example("cmdatl schema errors")
    .arg("COMMAND", "a command or group, or errors", false, true)
    .returns("schema_version", "raised when a command is renamed")
    .returns("commands", "one entry per command")
    .returns("errors", "with `schema errors`: the error catalog")
    .returns("global_options", "with no selector: the options every command takes")
    .error("usage.unknown_command")
    .runs([](const Invocation& invocation, const Table& table, Output&) {
      const auto found = invocation.values_.find("COMMAND");
      const auto words = found == invocation.values_.end() ? std::vector<std::string>{} : found->second;
      return schema_reply(table, words);
    }));
  add(make_command({"guide"}, "Help", "how to use cmdatl: agent or strategy")
    .example("cmdatl guide agent")
    .arg("TOPIC", "agent or strategy")
    .returns("text", "the guide"));
  return list;
}

}  // namespace cmdatl
