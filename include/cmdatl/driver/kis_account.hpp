#pragma once

// The KIS part of an account run (`account_main.hpp`): the logins checked against the operator's file and the two
// sessions built (`account/kis.hpp`, shared with the commands on a KIS account), then `run_account`.
// Orders go where they always did.

#include "../account/kis.hpp"
#include "./account_main.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;
namespace json = boost::json;

using cmdatl::kis_account_mismatch;
using cmdatl::run_token_store;

struct KisBrokerTest;

/**
 * @brief The KIS part of an account run (see `account_main.hpp`): two logins, the account's and, on demo, the real one.
 *
 * `KisBrokerTest` is defined by the tests alone, to read the configs `identify` built.
 */
class KisBroker {
public:
  /**
   * @brief The broker the driver trades.
   * @return `kis`.
   */
  std::string name() const {
    return "kis";
  }

  /**
   * @brief How many logins `cmdatl run` frames: the account's, then the real login for market data (empty on real).
   * @return 2.
   */
  std::size_t logins() const {
    return 2;
  }

  /**
   * @brief Reads the logins, checks them against the operator file, and names the account they drive.
   * @param base The reply's base.
   * @param config The operator file.
   * @param documents The logins.
   * @param effective Its number set to the login's account.
   * @param market_data Where market data comes from and its tape (KRX), for the start line.
   * @return The exit after a refusal, or nothing.
   */
  std::optional<int> identify(
    const json::object& base, const OperatorConfig& config, const std::vector<std::string>& documents,
    OperatorConfig& effective, std::string& market_data
  ) {
    const bool demo = config.env_ == "demo";
    if (documents[0].empty() || (demo && documents[1].empty())) {
      return run_error(
        base, Exit::FAILED, "run.failed", "the driver received no logins on descriptor 3; start the run with cmdatl run"
      );
    }
    if (
      const auto mismatch = read_kis_logins(config, documents, trading_config_, account_, market_config_, effective)
    ) {
      return run_error(base, Exit::FAILED, "auth.account_mismatch", *mismatch);
    }
    market_data = market_config_ ? "kis-real (read only), KRX" : "own, KRX";
    return std::nullopt;
  }

  /**
   * @brief Builds the sessions and trades: both clients paced at one request a second, their tokens shared through
   *   `run_token_store()`.
   * @tparam Program The program.
   * @param run The run.
   * @param effective The operator file, with the login's account.
   * @param strategy The program.
   * @return `run_account`'s exit.
   */
  template <typename Program>
  int start(AccountRun& run, const OperatorConfig& effective, Program&& strategy) {
    KisSessions sessions(std::move(trading_config_), std::move(account_), std::move(market_config_));
    const auto make_clock = [](std::function<void()> after_slice) {
      return WallClock(std::move(after_slice));
    };
    return run_account(
      run, effective, std::forward<Program>(strategy), sessions.trading(), sessions.market(), make_clock
    );
  }

private:
  friend struct KisBrokerTest;

  atl::kis::KisConfig trading_config_;
  atl::kis::KisAccount account_;
  std::optional<atl::kis::KisConfig> market_config_;
};

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
  KisBroker broker;
  return account_main<Program>(argc, argv, std::move(symbols_of), std::move(make), broker);
}

} // namespace driver
} // namespace cmdatl
