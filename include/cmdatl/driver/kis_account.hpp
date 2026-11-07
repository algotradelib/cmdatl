#pragma once

// The KIS account driver's entry: the logins read from descriptor 3, checked against the operator's file, the trading
// session and the market-data session built, then `run_account`.
//
// On a demo account the market data comes from the real login with an empty account, which cannot trade: a KIS demo
// host refuses reference data and the holiday calendar (`EGW02006`), as the library's `example-live-program` does.
// Both clients are paced at one request a second, the rate a KIS demo key is refused above (`EGW00201`).

#include "../account/operator.hpp"
#include "../core/runs.hpp"
#include "../system/credential_pipe.hpp"
#include "../system/process.hpp"
#include "./account.hpp"
#include "./events.hpp"
#include "./run.hpp"

#include <algotradelib/kis>

#include <boost/asio/ssl/context.hpp>
#include <boost/json.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;
namespace json = boost::json;

/**
 * @brief Runs a program on a KIS account; see the file comment.
 * @tparam Program The program.
 * @tparam Symbols A callable from the run's arguments to its symbols.
 * @tparam Make A callable from the run's arguments and instruments to the program.
 * @param argc The count.
 * @param argv `run.id=ID`, then atlc's arguments.
 * @param symbols_of Gives the run's symbols.
 * @param make Builds the program.
 * @return The exit.
 */
template <typename Program, typename Symbols, typename Make>
int kis_account_main(int argc, char** argv, Symbols symbols_of, Make make) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  (void)symbols_of;
  (void)make;
  return 1;
#else
  handle_signals();
  // The logins first, whatever happens next: the descriptor is closed once read.
  const auto payload = sys::read_staged_credentials();
  json::object base{{"env", "kis"}};
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
  if (config.broker_ != "kis") {
    return run_error(
      base, Exit::FAILED, "unsupported.broker",
      "this driver trades a KIS account, and the operator file names " + config.broker_
    );
  }
  const bool demo = config.env_ == "demo";
  base["env"] = std::string("kis-") + config.env_;
  // The logins, framed by `cmdatl run`: the account's, then the real login for market data (empty on a real account).
  const auto documents = payload ? sys::unframe_credentials(*payload, 2) : std::nullopt;
  if (!documents || (*documents)[0].empty() || (demo && (*documents)[1].empty())) {
    return run_error(
      base, Exit::FAILED, "run.failed", "the driver received no logins on descriptor 3; start the run with cmdatl run"
    );
  }
  atl::kis::KisConfig trading_config;
  atl::kis::KisAccount account;
  try {
    trading_config.from_json_text((*documents)[0], "the stored login");
    account.from_json_text((*documents)[0], "the stored login");
  } catch (const std::exception&) {
    // The loaders' messages may quote what they read: never repeated.
    return run_error(base, Exit::FAILED, "auth.account_mismatch", "the stored login could not be read; log in again");
  }
  if (
    const auto mismatch = kis_account_mismatch(
      config.number_, demo, account.account_number_, account.product_code_, trading_config.is_demo_
    )
  ) {
    return run_error(base, Exit::FAILED, "auth.account_mismatch", *mismatch);
  }
  std::optional<atl::kis::KisConfig> market_config;
  if (demo) {
    atl::kis::KisConfig real;
    try {
      real.from_json_text((*documents)[1], "the stored real login");
    } catch (const std::exception&) {
      return run_error(
        base, Exit::FAILED, "auth.account_mismatch", "the stored real login could not be read; log in again"
      );
    }
    if (real.is_demo_) {
      return run_error(base, Exit::FAILED, "auth.account_mismatch", "the login stored as kis/real is a demo login");
    }
    market_config = std::move(real);
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
  const auto dir = account_dir(config);
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
  run.market_data_ = demo ? "kis-real (read only)" : "own";
  run.account_dir_ = *dir;
  try {
    namespace ssl = boost::asio::ssl;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);
    atl::network::HttpClientOptions paced;
    paced.rate_limit_ = atl::network::RateLimitBudget{1, 1, 0};
    using Session = atl::BlockingSession<atl::kis::KisBlockingClient>;
    Session session(
      std::make_shared<atl::kis::KisBlockingClient>(tls, std::move(trading_config), paced), std::move(account)
    );
    const auto make_clock = [](std::function<void()> after_slice) {
      return WallClock(std::move(after_slice));
    };
    if (market_config) {
      // No account: a session that cannot trade, so a swap by mistake places nothing.
      Session market(
        std::make_shared<atl::kis::KisBlockingClient>(tls, std::move(*market_config), paced), atl::kis::KisAccount{}
      );
      return run_account(run, config, std::move(*strategy), session, market, make_clock);
    }
    return run_account(run, config, std::move(*strategy), session, session, make_clock);
  } catch (const std::exception& error) {
    return run_error(base, Exit::FAILED, "internal.error", error.what());
  }
#endif
}

} // namespace driver
} // namespace cmdatl
