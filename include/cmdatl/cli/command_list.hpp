#pragma once

#include "../account/reads.hpp"
#include "../auth/login.hpp"
#include "../core/table.hpp"
#include "../orders/orders.hpp"
#include "../strategy/backtest.hpp"
#include "../strategy/runs.hpp"
#include "../strategy/strategy.hpp"
#include "./describe.hpp"
#include "./environment.hpp"
#include "./guide.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace cmdatl {

/** @brief The exit codes of a command that sends nothing. */
inline const std::vector<Exit> READ_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE};
/** @brief The exit codes of a command that may send, or that plans or reconciles what was sent. */
inline const std::vector<Exit> SEND_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::REFUSED, Exit::UNKNOWN};
/** @brief The exit codes of a command that runs atlc and a driver on a program, and stops them on SIGINT or SIGTERM. */
inline const std::vector<Exit> PROGRAM_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::PROGRAM, Exit::INTERRUPTED};

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

  // clang-format off: clang-format would hang each chain under `make_command` (column 6); kept on 2-space steps.
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
    .returns(
      "cash", "the mock market's cash after every fill, before what resting buys reserve; null on a broker's account"
    )
    .returns("buying_power", "the broker's buying power")
    .returns("owed", "orders the journal still waits on")
    .returns("orders", "working orders")
    .returns("quotes", "each allowed symbol, as quote's rows")
    .returns("data_end", "the last session's close in the mock data; null on a broker's account")
    .returns("capabilities", "what the account's broker offers")
    .returns("state_dir", "the account's state directory")
    .on_broker()
    .on_account(AccountKind::STATUS));
  add(make_command({"account"}, "Read", "positions, cash, buying power, owed orders, orders left today")
    .example("cmdatl account")
    .example("cmdatl account --value cash")
    .example("cmdatl account --symbol 005930 --fields positions.quantity")
    .option("symbol", "SYMBOL", "only this instrument's rows")
    .returns("now", "the account's time, KST with its offset")
    .returns("positions", "holdings: symbol, quantity, sellable, average_price, last_price, market_value")
    .returns(
      "cash", "the mock market's cash after every fill, before what resting buys reserve; null on a broker's account"
    )
    .returns("buying_power", "the broker's buying power")
    .returns(
      "owed",
      "orders the journal still waits on: symbol, side, quantity, journal_id, state, order_id, broker_status, next"
    )
    .returns("orders_left_today", "placements the operator still allows today; null when the journal is damaged")
    .error("usage.bad_value")
    .on_broker()
    .on_account(AccountKind::ACCOUNT, check_symbols));
  add(make_command({"quote"}, "Read", "last price, the price band if any, and whether orders are accepted now")
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
    .on_broker()
    .on_account(AccountKind::QUOTE, check_symbols));
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
    .returns("mock", "the mock market: bars, start, cash, costs (commission, sell tax, slippage ticks: all zero)")
    .returns("schema_version", "the version of cmdatl schema's format")
    .returns("modes", "the trading modes this build offers")
    .runs([](const Invocation&, const Table&, Output&) {
      return env_reply();
    }));

  add(make_command({"auth", "login"}, "Broker logins", "keep a broker's key and secret in the system's credential store")
    .example("cmdatl auth login")
    .example("cmdatl auth login --broker kis --env demo")
    .example("cmdatl auth login --broker toss --with-secret --force")
    .example("cmdatl auth login --broker eodhd --adapter adapters/eodhd")
    .describe(
      "Asks on a terminal for what is not given: the broker, demo or real (for a broker with a demo), and the\n"
      "broker's fields (KIS: app key, app secret, account number, product code; Kiwoom: app key, secret key;\n"
      "Toss: client id, client secret), secrets without echo. --with-secret reads the broker's JSON credential\n"
      "document from standard input instead. The document is checked with the library's loaders and kept in the\n"
      "macOS keychain (no other store in this version), one login per broker and environment. Nothing is sent to\n"
      "the broker: the key is first used, and so first checked, by a demo or real run. Without a terminal nothing\n"
      "is asked. A macOS keychain dialog is the system's: a signal does not dismiss it.\n"
      "A broker or data vendor cmdatl was not built for is a broker folder, named by its path with --adapter: its\n"
      "broker.toml says what is asked, and its document is stored unchecked, bound to the folder's path, and read\n"
      "by the folder's adapter at a run or a backtest that names the same folder."
    )
    .option("broker", "NAME", "kis, kiwoom, toss or a broker folder's name; asked on a terminal when not given")
    .option("adapter", "PATH", "the broker folder of a broker that is not built in, against the working directory")
    .option("env", "ENV", "demo or real, for a broker with a demo; asked on a terminal when not given")
    .flag("with-secret", "read the broker's credential document as JSON from standard input")
    .flag("force", "replace a stored login for the same broker and environment without asking")
    .returns("broker", "the broker")
    .returns("environment", "demo or real")
    .returns("key", "the key, masked")
    .returns("account", "the account number, masked, for a broker whose login has one (KIS)")
    .returns("adapter", "a broker folder's login: the folder it is bound to, canonical (no field of the login is shown)")
    .returns("store", "keychain")
    .returns("checked", "false: the broker has not seen the login")
    .returns("replaced", "whether a stored login was replaced")
    .returns("token_warning", "present when the old login's saved access token may remain, usable until it expires")
    .exits({Exit::OK, Exit::FAILED, Exit::USAGE, Exit::INTERRUPTED})
    .error("usage.bad_value")
    .error("usage.missing_argument")
    .error("auth.no_store")
    .error("auth.store_locked")
    .error("auth.exists")
    .error("auth.bad_document")
    .error("auth.not_confirmed")
    .error("auth.no_answer")
    .error("adapter.invalid")
    .error("adapter.unsafe")
    .error("interrupted")
    .runs([](const Invocation& invocation, const Table&, Output& output) {
      return auth::login_command(invocation, output);
    }));
  add(make_command({"auth", "status"}, "Broker logins", "the stored logins, masked; sends nothing")
    .example("cmdatl auth status")
    .example("cmdatl auth status --broker kis --env demo")
    .option("broker", "NAME", "only this broker's logins")
    .option("env", "ENV", "only demo or only real logins")
    .returns("store", "keychain, or null where cmdatl has no credential store")
    .returns(
      "logins",
      "one per stored login: broker, environment, key (masked), account (masked, KIS), store, checked, and locked "
      "when the store would not open it; a broker folder's login shows adapter (null: the login, which holds its folder, "
      "is not read) and no field"
    )
    .returns("environment_variables", "per broker, which of its environment variables are set (names only)")
    .error("usage.bad_value")
    .error("auth.no_store")
    .error("auth.store_locked")
    .runs([](const Invocation& invocation, const Table&, Output&) {
      return auth::status_command(invocation);
    }));
  add(make_command({"auth", "logout"}, "Broker logins", "remove a stored login")
    .example("cmdatl auth logout --broker kis --env demo")
    .option("broker", "NAME", "the login's broker")
    .option("env", "ENV", "demo or real")
    .returns("removed", "the login removed, such as kis/demo")
    .returns("token_warning", "present when the old login's saved access token may remain, usable until it expires")
    .exits({Exit::OK, Exit::FAILED, Exit::USAGE, Exit::INTERRUPTED})
    .error("usage.bad_value")
    .error("usage.missing_argument")
    .error("auth.no_store")
    .error("auth.store_locked")
    .error("auth.not_found")
    .error("auth.no_answer")
    .error("interrupted")
    .runs([](const Invocation& invocation, const Table&, Output& output) {
      return auth::logout_command(invocation, output);
    }));

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
    .on_account(AccountKind::PLAN, check_order_args)
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
    .on_account(AccountKind::PLACE, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "cancel"}, "Trade", "withdraw working orders on SYMBOL (never places)")
    .example("cmdatl order cancel 005930")
    .arg("SYMBOL", "an instrument")
    .returns("now", "the account's time, one simulated second on unless at the data's end, KST with its offset")
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
    .on_account(AccountKind::CANCEL, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "reconcile"}, "Trade", "record fills from holdings (never places)")
    .example("cmdatl order reconcile")
    .example("cmdatl order reconcile 005930")
    .arg("SYMBOL", "an instrument", false)
    .returns("now", "the account's time, one simulated second on unless at the data's end, KST with its offset")
    .returns("checked", "each symbol read: symbol, state (clean, working, holding_moved or a reason), detail")
    .returns("owed", "what is still owed, after the call")
    .returns("reason", "clean, awaiting, or the first row's reason")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .error("quote.no_data")
    .on_account(AccountKind::RECONCILE, check_order_args)
    .exits(ORDER_EXITS));
  add(make_command({"order", "list"}, "Trade", "working orders")
    .example("cmdatl order list")
    .returns("now", "the account's time, KST with its offset")
    .returns("orders", "working orders: order_id, symbol, side, quantity, filled, price, status")
    .on_broker()
    .on_account(AccountKind::ORDER_LIST));
  add(make_command({"order", "release"}, "Trade", "operator only: settle an unknown order")
    .example("cmdatl order release 005930 --reason \"checked the broker's order list\"")
    .arg("SYMBOL", "an instrument")
    .option("reason", "TEXT", "why the operator settles it", true)
    .returns("now", "the account's time, one simulated second on unless at the data's end, KST with its offset")
    .returns("symbol", "the instrument")
    .returns("reason", "released; on a refusal or unknown, the reason")
    .returns("detail", "what was released")
    .returns("released", "the journal ids settled")
    .returns("approval_kept", "whether the approval file could not be renamed after its use (harmless)")
    .returns("owed", "orders the journal still waits on, for this symbol")
    .returns("events", "what the market did in the command's simulated second")
    .returns("result", "done, refused or unknown")
    .mutates("cmdatl account shows what is still owed; read it before releasing again")
    .on_account(AccountKind::RELEASE, check_order_args)
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
    .runs([](const Invocation& invocation, const Table&, Output&) {
      return new_reply(invocation);
    }));
  add(make_command({"strategy", "backtest"}, "Strategy", "replay a program on its run settings and report")
    .describe(
      "TARGET is a program (.atls), a run spec (TOML), or both, at most one of each; KEY=VALUE words are settings (a "
      "dotted key before the first =), before or after them, and win over the spec's. A spec given with a program "
      "applies to it and names no other. cmdatl strategy reference run-spec lists the settings. "
      "The program is built once per shape (CMake and a C++17 compiler are needed; builds are cached)."
    )
    .example("cmdatl strategy backtest dip.toml")
    .example("cmdatl strategy backtest dip.toml params.lookback=30 --fills fills.csv")
    .example("cmdatl strategy backtest dip.atls bars.source=csv bars.file=bars.csv bars.symbol=005930")
    .example("cmdatl strategy backtest dip.atls daily.toml instruments.stock=005930")
    .arg("TARGET", "a program (.atls), a run spec, or both")
    .arg("SETTING", "KEY=VALUE with a dotted key (params.lookback=30), before or after TARGET", false, true)
    .option("fills", "FILE", "also write every fill")
    .returns("report", "the replay's report, unchanged")
    .returns("spec", "the run spec, absolute, or null when no spec is given")
    .returns("program", "the program, absolute")
    .returns("settings_given", "the KEY=VALUE settings given (the spec's and the program's own values are not listed)")
    .returns("atlc", "atlc's version")
    .returns("fills", "the fills file, absolute, when asked")
    .returns(
      "fetched",
      "with bars.source naming a broker folder: the source, fetched_at, and per symbol the bars fetched, those new to "
      "the store, the first and last date and the file"
    )
    .returns("errors", "on a settings or program failure: the diagnostics, with file and line")
    .returns("stderr", "on a failure atlc, the build or the replay reported: its last stderr lines")
    .exits(PROGRAM_EXITS)
    .error("interrupted")
    .error("usage.missing_argument")
    .error("usage.bad_value")
    .error("cache.unavailable")
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
    .error("adapter.not_found")
    .error("adapter.invalid")
    .error("adapter.unsafe")
    .error("auth.no_store")
    .error("auth.store_locked")
    .error("auth.not_found")
    .error("auth.adapter_mismatch")
    .error("fetch.failed")
    .runs([](const Invocation& invocation, const Table&, Output& output) {
      return backtest_reply(invocation, output);
    }));
  add(make_command({"strategy", "reference"}, "Strategy", "the atls language reference, from the atlc cmdatl runs")
    .example("cmdatl strategy reference")
    .example("cmdatl strategy reference builtins")
    .example("cmdatl strategy reference run-spec")
    .arg("TOPIC", "a chapter (builtins, syntax, ...), or run-spec for cmdatl's run settings; the index when omitted", false)
    .returns("text", "the reference's index or chapter, as atlc reference prints it; run-spec as cmdatl has it")
    .returns("stderr", "on a failure atlc reported: its last stderr lines")
    .exits({Exit::OK, Exit::FAILED, Exit::USAGE, Exit::INTERRUPTED})
    .error("interrupted")
    .error("atlc.missing")
    .error("atlc.unsupported")
    .error("atlc.failed")
    .error("usage.bad_value")
    .error("unavailable.platform")
    .error("config.missing")
    .error("config.invalid")
    .runs([](const Invocation& invocation, const Table&, Output&) {
      return reference_reply(invocation);
    }));

  add(make_command({"run"}, "Run", "run a strategy on its own mock market or a KIS account, in the foreground")
    .describe(
      "Builds the program's driver (CMake and a C++17 compiler are needed; builds are cached) and becomes it: the run "
      "trades a mock market made from the settings' data or, when the operator's file names a broker account (KIS or "
      "a broker folder's), that account in real time, and prints its events as JSON lines, its reply last, whatever "
      "the output format. Settings are read as strategy backtest reads them. The operator's file limits every order "
      "the run places, and no setting changes it. Ctrl-C, SIGTERM or SIGHUP withdraws the run: no new orders, its "
      "resting orders cancelled; under nohup, SIGHUP is ignored. The agent commands do not see a run's mock market."
    )
    .example("cmdatl run dip.toml")
    .example("cmdatl run dip.toml params.lookback=21")
    .example("cmdatl run dip.atls daily.toml > run.jsonl")
    .arg("TARGET", "a program (.atls), a run spec, or both; replay.market = \"kr\" and limit orders")
    .arg("SETTING", "KEY=VALUE with a dotted key (params.lookback=30), before or after TARGET", false, true)
    .returns("run", "the run id")
    .returns("result", "ended, refused, failed or unknown")
    .returns("outcome", "clean, refused_orders, or how the run ended otherwise")
    .returns("stopped", "why the run stopped placing: end, signal, not_armed, lock_lost or event_log")
    .returns(
      "refused",
      "orders refused, by whom: boundary (the operator's limits or the market), guard, pricing; signal, once stopping"
    )
    .returns("from", "the run's first instant, market time")
    .returns("to", "the date of the data's last bar")
    .returns("ended_at", "the run's last instant, market time")
    .returns("executions", "fills")
    .returns("commission_paid", "commission, in won")
    .returns("tax_paid", "tax, in won")
    .returns("opening_equity", "the run's cash at its start")
    .returns("closing_equity", "cash and holdings at the last price, at the end")
    .returns("holdings_at_end", "shares held at the end, by symbol")
    .returns(
      "holdings_at_end_error",
      "on a broker account, why holdings_at_end is null: the broker's code, else the error's kind; null when read"
    )
    .returns("owed_at_end", "orders the run's journal still waits on")
    .exits({Exit::OK, Exit::FAILED, Exit::USAGE, Exit::REFUSED, Exit::UNKNOWN, Exit::PROGRAM, Exit::INTERRUPTED})
    .sends()
    .on_config(runs::run_reply));
  {
    auto& run = list.back();
    for (const auto& code : RUN_CODES) {
      run.error(code);
    }
  }

  add(make_command({"clock"}, "Mock account", "the mock account's time and phase")
    .example("cmdatl clock")
    .returns("now", "the account's time, KST with its offset")
    .returns("phase", "the market phase")
    .returns("data_end", "the last session's close in the mock data")
    .on_account(AccountKind::CLOCK));
  add(make_command({"clock", "advance"}, "Mock account", "move the mock clock")
    .example("cmdatl clock advance --next-open")
    .example("cmdatl clock advance --minutes 30")
    .example("cmdatl clock advance --to 2024-03-26T09:30")
    .describe(
      "Moves the mock clock forward by exactly one of the options, filling or expiring resting orders on the way.\n"
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
    .mutates("cmdatl clock shows where the clock stands: read it before any other command")
    .on_account(AccountKind::ADVANCE, check_advance));

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
  add(make_command({"guide", "agent"}, "Help", "how an agent reads the account, places orders and runs strategies")
    .example("cmdatl guide agent")
    .example("cmdatl guide agent --format text")
    .returns("text", "the guide, the same text as the cmdatl-agent skill")
    .runs([](const Invocation&, const Table&, Output&) {
      return guide_reply("agent");
    }));
  add(make_command({"guide", "strategy"}, "Help", "how to write a strategy in atls, backtest it and run it")
    .example("cmdatl guide strategy")
    .example("cmdatl guide strategy --format text")
    .returns("text", "the guide, the same text as the cmdatl-strategy skill")
    .runs([](const Invocation&, const Table&, Output&) {
      return guide_reply("strategy");
    }));
  // clang-format on
  return list;
}

} // namespace cmdatl
