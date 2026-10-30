#pragma once

#include "./describe.hpp"
#include "./environment.hpp"
#include "./table.hpp"

#include <string>
#include <vector>

namespace cmdatl {

/** @brief The exit codes of a command that sends nothing. */
inline const std::vector<Exit> READ_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE};
/** @brief The exit codes of a command that may send, or that plans or reconciles what was sent. */
inline const std::vector<Exit> SEND_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::REFUSED, Exit::UNKNOWN};
/** @brief The exit codes of a command that compiles a strategy. */
inline const std::vector<Exit> PROGRAM_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::PROGRAM};

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
    list.push_back(command);
  };

  add(make_command({"status"}, "Read", "clock, phase, armed, run, positions, cash, owed and working orders")
    .returns("now", "the account's time")
    .returns("phase", "the market phase")
    .returns("armed", "whether orders may go out")
    .returns("run", "the run that owns the account, or null")
    .returns("positions", "holdings")
    .returns("cash", "cash")
    .returns("owed", "orders the journal still waits on")
    .returns("orders", "working orders")
    .account());
  add(make_command({"account"}, "Read", "positions, cash, buying power, owed orders, orders left today")
    .option("symbol", "SYMBOL", "only this instrument's rows")
    .returns("positions", "holdings")
    .returns("cash", "cash")
    .returns("buying_power", "buying power")
    .returns("owed", "orders the journal still waits on")
    .returns("orders_left_today", "orders the operator still allows today")
    .account());
  add(make_command({"quote"}, "Read", "last price, price band, and whether orders are accepted now")
    .arg("SYMBOL", "an instrument", true, true)
    .returns("quotes", "one per symbol: symbol, last, band, symbol_allowed, orders_accepted_now")
    .account());
  add(make_command({"env"}, "Read", "configuration: cmdatl, algotradelib, atlc, the operator's file (no secrets)")
    .returns("cmdatl", "cmdatl's version")
    .returns("algotradelib", "the library's version")
    .returns("atlc", "requested, path, source, version, minimum, ok")
    .returns("config", "the operator's file: path, exists")
    .returns("modes", "the trading modes this build offers")
    .runs([](const Invocation&, const Table&, Output&) { return env_reply(); }));

  add(make_command({"order", "plan"}, "Trade", "what would be sent; gives a plan id")
    .describe(
      "What `order place` would send to move SYMBOL to N shares with a limit at P, checked against the\n"
      "operator's limits, the market phase and the account. Sends nothing. The plan id expires when the\n"
      "account changes or after the operator's plan lifetime."
    )
    .arg("SYMBOL", "an instrument the operator allows")
    .option("target", "N", "the position to hold, whole shares (0 to sell out)", true)
    .option("limit", "P", "limit price, on the tick grid, inside the operator's collar", true)
    .returns("plan_id", "the plan, for order place")
    .returns("would", "buy, sell or nothing")
    .returns("quantity", "shares")
    .returns("limit", "the limit price")
    .returns("held", "the position now")
    .returns("result", "done, refused or unknown")
    .returns("next", "the command to run next")
    .exits(SEND_EXITS)
    .account());
  add(make_command({"order", "place"}, "Trade", "send that plan, if nothing moved since")
    .arg("PLAN_ID", "a plan id from order plan")
    .returns("order", "the order sent")
    .returns("result", "done, refused or unknown")
    .sends()
    .account());
  add(make_command({"order", "cancel"}, "Trade", "withdraw working orders on SYMBOL (never places)")
    .arg("SYMBOL", "an instrument")
    .returns("result", "withdrawal_sent or nothing_working")
    .sends()
    .account());
  add(make_command({"order", "reconcile"}, "Trade", "record fills from holdings (never places)")
    .arg("SYMBOL", "an instrument", false)
    .returns("checked", "instruments read")
    .returns("owed", "what is still owed")
    .exits(SEND_EXITS)
    .account());
  add(make_command({"order", "list"}, "Trade", "working orders")
    .returns("orders", "working orders")
    .account());
  add(make_command({"order", "release"}, "Trade", "operator only: settle an unknown order")
    .arg("SYMBOL", "an instrument")
    .option("reason", "TEXT", "why the operator settles it", true)
    .returns("result", "done or refused")
    .sends()
    .account());

  add(make_command({"strategy", "new"}, "Strategy", "a program and a run spec to start from")
    .arg("NAME", "the program's name")
    .option("template", "T", "mean-reversion, crossover, momentum or breakout")
    .option("dir", "DIR", "where to write them")
    .returns("program", "the .atls file")
    .returns("spec", "the run spec")
    .returns("next", "the command to run next"));
  add(make_command({"strategy", "check"}, "Strategy", "parse and check a program (no compiler needed)")
    .arg("FILE", "an .atls program")
    .returns("diagnostics", "atlc's diagnostics")
    .exits(PROGRAM_EXITS));
  add(make_command({"strategy", "backtest"}, "Strategy", "replay a run spec and report")
    .arg("SPEC", "a run spec")
    .option(PARAMETER)
    .option("fills", "FILE", "also write every fill")
    .returns("report", "the replay's report")
    .returns("params", "the parameters used")
    .exits(PROGRAM_EXITS));
  add(make_command({"strategy", "tune"}, "Strategy", "a parameter grid, in and out of sample")
    .arg("SPEC", "a run spec")
    .option({"grid", "k=a:b:s", "a parameter's values: from, to, step, or a list", true, true, ""})
    .option(PARAMETER)
    .option("split", "DATE", "in sample before, out of sample from")
    .option("top", "N", "rows to show")
    .returns("sets", "one row per parameter set")
    .returns("distinct_sets_tried", "trials counted for this program")
    .exits(PROGRAM_EXITS));
  add(make_command({"strategy", "reference"}, "Strategy", "the atls language and its built-ins")
    .arg("TOPIC", "a chapter", false)
    .returns("text", "the reference"));

  add(make_command({"run", "start"}, "Run", "start a strategy trading an account, in the background")
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
    .arg("RUN", "a run id", false)
    .returns("run", "the run id")
    .returns("state", "running, stopped or unknown")
    .returns("summary", "trades and position")
    .account());
  add(make_command({"run", "log"}, "Run", "a run's events")
    .arg("RUN", "a run id", false)
    .flag("follow", "keep printing until the run ends")
    .returns("events", "placed, filled, refused, heartbeat")
    .account());
  add(make_command({"run", "stop"}, "Run", "stop a run: quiesce, or also withdraw or flatten")
    .arg("RUN", "a run id", false)
    .flag("withdraw", "also cancel the account's working orders")
    .flag("flatten", "withdraw, then close the positions")
    .returns("state", "stopped or unknown")
    .returns("summary", "the final summary")
    .exits(SEND_EXITS)
    .sends()
    .account());

  add(make_command({"clock"}, "Paper account", "the paper account's time and phase")
    .returns("now", "the account's time")
    .returns("phase", "the market phase")
    .account());
  add(make_command({"clock", "advance"}, "Paper account", "move the paper clock")
    .option("minutes", "N", "by N minutes")
    .option("hours", "N", "by N hours")
    .option("to", "TIME", "to a time")
    .flag("next-open", "to the next open")
    .returns("now", "the account's time")
    .returns("fills", "fills on the way")
    .account());

  add(make_command({"schema"}, "Help", "the machine contract: commands, fields, exit and error codes")
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
    .arg("TOPIC", "agent or strategy")
    .returns("text", "the guide"));
  return list;
}

}  // namespace cmdatl
