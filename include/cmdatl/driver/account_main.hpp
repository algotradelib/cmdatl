#pragma once

// An account driver's entry, the same for every broker: the logins read from descriptor 3, the run's arguments, the
// operator file and its limits checked, then the broker's own part (`kis_account.hpp`, `user_account.hpp`) reads its
// logins, says which account they drive, and builds the sessions `run_account` trades through.
//
// A broker's part is an object with:
// - `std::string name() const`: the broker the driver was built for; an operator file naming another is refused;
// - `std::size_t logins() const`: how many documents `cmdatl run` framed;
// - `std::optional<int> identify(base, config, documents, effective, market_data)`: reads the logins, checks them
//   against the operator file, sets `effective.number_` to the account they drive and `market_data` for the start
//   line; returns the exit after writing the reply, or nothing;
// - `template <typename Program> int start(AccountRun&, const OperatorConfig& effective, Program&&)`: builds the
//   sessions and returns `run_account`'s exit.

#include "../account/operator.hpp"
#include "../core/runs.hpp"
#include "../system/credential_pipe.hpp"
#include "../system/process.hpp"
#include "./account.hpp"
#include "./events.hpp"
#include "./run.hpp"

#include <boost/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;
namespace json = boost::json;

/**
 * @brief Runs a program on a broker's account; see the file comment.
 * @tparam Program The program.
 * @tparam Symbols A callable from the run's arguments to its symbols.
 * @tparam Make A callable from the run's arguments and instruments to the program.
 * @tparam Broker The broker's part.
 * @param argc The count.
 * @param argv `run.id=ID`, then atlc's arguments.
 * @param symbols_of Gives the run's symbols.
 * @param make Builds the program.
 * @param broker The broker's part.
 * @return The exit.
 */
template <typename Program, typename Symbols, typename Make, typename Broker>
int account_main(int argc, char** argv, Symbols symbols_of, Make make, Broker& broker) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  (void)symbols_of;
  (void)make;
  (void)broker;
  return 1;
#else
  handle_signals();
  // The logins first, whatever happens next: the descriptor is closed once read.
  const auto payload = sys::read_staged_credentials();
  json::object base{{"env", broker.name()}};
  RunArgs args;
  if (const auto problem = read_args(argc, argv, args)) {
    return run_error(base, Exit::USAGE, "usage.bad_value", *problem);
  }
  base["run"] = args.id_;
  if (Program::atls_session) {
    return run_error(
      base, Exit::PROGRAM, "program.live_unsupported", SESSION_UNSUPPORTED, SESSION_UNSUPPORTED_NEXT,
      {{"unsupported", "session"}}
    );
  }
  if (const auto refusal = spec_refusal(args.atlc_)) {
    return run_error(base, Exit::USAGE, "spec.invalid", refusal->second, "", {{"key", refusal->first}});
  }
  const auto snapshot = load_operator_config();
  if (snapshot.state_ != ConfigSnapshot::State::OK) {
    return relay(base, config_reply(snapshot));
  }
  const auto& config = *snapshot.config_;
  if (config.broker_ != broker.name()) {
    return run_error(
      base, Exit::FAILED, "unsupported.broker",
      "this driver trades a " + broker.name() + " account, and the operator file names " + config.broker_
    );
  }
  // The options the adapter connects with are the ones `cmdatl run` checked against its declarations, or none is used.
  if (!args.options_) {
    return run_error(
      base, Exit::FAILED, "run.failed", "the driver received no run.options; start the run with cmdatl run"
    );
  }
  if (options::text_of(config.options_) != *args.options_) {
    return run_error(
      base, Exit::FAILED, "config.invalid",
      "the operator's file changed after cmdatl run checked it: its [account.options] are not the ones checked; "
      "nothing was read or sent",
      "start the run again"
    );
  }
  base["env"] = broker.name() + "-" + config.env_;
  const auto documents = payload ? sys::unframe_credentials(*payload, broker.logins()) : std::nullopt;
  if (!documents) {
    return run_error(
      base, Exit::FAILED, "run.failed", "the driver received no logins on descriptor 3; start the run with cmdatl run"
    );
  }
  auto effective = config;
  std::string market_data = "own";
  if (const auto refused = broker.identify(base, config, *documents, effective, market_data)) {
    return *refused;
  }
  std::vector<std::string> symbols;
  std::optional<Program> strategy;
  std::vector<atl::InstrumentId> ids;
  try {
    symbols = symbols_of(args.atlc_);
    for (const auto& symbol : symbols) {
      ids.push_back(atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol});
    }
    strategy.emplace(make(args.atlc_, ids));
  } catch (const std::invalid_argument& error) {
    return run_error(base, Exit::PROGRAM, "program.invalid", error.what());
  } catch (const std::out_of_range& error) {
    return run_error(base, Exit::PROGRAM, "program.invalid", error.what());
  }
  if (symbols.empty()) {
    return run_error(
      base, Exit::USAGE, "spec.no_instruments", "the run names no instrument to trade",
      "name the symbols in the run spec's [instruments]"
    );
  }
  for (const auto& symbol : symbols) {
    if (!config.allows(symbol)) {
      return run_error(
        base, Exit::REFUSED, "refused.symbol_not_allowed", symbol + " is not in the operator's limits.symbols", "",
        {{"symbol", symbol}}
      );
    }
  }
  const std::size_t warmup = strategy->warmup_bars();
  if (warmup > MAX_WARMUP_BARS) {
    return run_error(
      base, Exit::PROGRAM, "program.live_unsupported",
      "the program warms up on " + std::to_string(warmup) + " bars, more than a run reads (" +
        std::to_string(MAX_WARMUP_BARS) + ")",
      "shorten the program's warm-up", {{"unsupported", "warmup"}}
    );
  }
  const auto order_type = atlc_value(args.atlc_, "orders.type").value_or("market");
  if (config.limits_.order_type_ == "limit" && order_type != "limit") {
    return run_error(
      base, Exit::REFUSED, "refused.order_type",
      "the run spec's [orders] type is " + order_type + "; the operator allows limit orders only",
      "set [orders] type = \"limit\" and an offset within the operator's collar"
    );
  }
  if (const auto offset = atlc_value(args.atlc_, "orders.offset")) {
    double value = 0;
    try {
      value = std::stod(*offset);
    } catch (const std::exception&) {
      return run_error(base, Exit::USAGE, "spec.invalid", "orders.offset " + *offset + " is not a number");
    }
    if (!(value <= config.limits_.collar_)) {
      return run_error(
        base, Exit::REFUSED, "refused.collar",
        "the run spec's [orders] offset " + *offset + " is above the operator's collar " +
          decimal_text(config.limits_.collar_)
      );
    }
  }
  const auto dir = account_dir(effective);
  if (!dir) {
    return run_error(
      base, Exit::FAILED, "state.unavailable", "neither XDG_STATE_HOME nor HOME names an absolute directory"
    );
  }
  AccountRun run;
  run.base_ = base;
  run.id_ = args.id_;
  run.symbols_ = symbols;
  run.ids_ = ids;
  run.warmup_ = warmup;
  run.market_data_ = market_data;
  run.account_dir_ = *dir;
  try {
    return broker.start(run, effective, std::move(*strategy));
  } catch (const std::exception& error) {
    return run_error(base, Exit::FAILED, "internal.error", run.opaque_errors_ ? OPAQUE_EXCEPTION : error.what());
  }
#endif
}

} // namespace driver
} // namespace cmdatl
