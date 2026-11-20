#pragma once

// A built-in broker's part of an account run (`account_main.hpp`): the logins checked against the operator's file and
// the two sessions built (`account/builtin.hpp`, shared with the commands on the account), then `run_account`.
// Orders go where they always did. The driver compiles the one client its `broker.hpp` names.

#include "../account/builtin.hpp"
#include "../brokers/derive.hpp"
#include "../core/builtins.hpp"
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

using cmdatl::account_mismatch;
using cmdatl::run_token_store;

struct BuiltinBrokerTest;

/**
 * @brief A built-in broker's part of an account run (see `account_main.hpp`): the account's login and, where the facts
 *   say so on demo (KIS), the real one for market data.
 *
 * `BuiltinBrokerTest` is defined by the tests alone, to read the configs `identify` built.
 * @tparam Client The broker's blocking client.
 */
template <typename Client>
class BuiltinAccountBroker {
public:
  /** @brief Whether a second login, the real one, is read for market data on demo. */
  static constexpr bool REAL_MARKET_DATA =
    brokers::facts_of<Client>().market_data_ == builtin::MarketDataLogin::REAL_WHEN_DEMO;

  /**
   * @brief The broker the driver trades, as the operator file names it.
   * @return The user's word (`kis`).
   */
  std::string name() const {
    return std::string(brokers::facts_of<Client>().word_);
  }

  /**
   * @brief How many logins `cmdatl run` frames: the account's, then where the facts say so the real login for market
   *   data (empty on real).
   * @return 2 for KIS, else 1.
   */
  std::size_t logins() const {
    return REAL_MARKET_DATA ? 2 : 1;
  }

  /**
   * @brief Reads the logins, checks them against the operator file, and names the account they drive.
   * @param base The reply's base.
   * @param config The operator file.
   * @param documents The logins.
   * @param effective Its number set to the login's account.
   * @param market_data Where market data comes from and its tape, for the start line.
   * @return The exit after a refusal, or nothing.
   */
  std::optional<int> identify(
    const json::object& base, const OperatorConfig& config, const std::vector<std::string>& documents,
    OperatorConfig& effective, std::string& market_data
  ) {
    const bool demo = config.env_ == "demo";
    if (documents[0].empty() || (REAL_MARKET_DATA && demo && documents[1].empty())) {
      return run_error(
        base, Exit::FAILED, "run.failed", "the driver received no logins on descriptor 3; start the run with cmdatl run"
      );
    }
    if (
      const auto mismatch =
        read_builtin_logins<Client>(config, documents, trading_config_, account_, market_config_, effective)
    ) {
      return run_error(base, Exit::FAILED, "auth.account_mismatch", *mismatch);
    }
    constexpr const auto& facts = brokers::facts_of<Client>();
    const std::string tape = facts.tape_ == builtin::Tape::KRX ? "KRX" : "KRX and NXT";
    market_data = market_config_ ? std::string(facts.word_) + "-real (read only), " + tape : "own, " + tape;
    return std::nullopt;
  }

  /**
   * @brief Builds the sessions and trades: every client paced at `builtin::REQUESTS_PER_SECOND`, its tokens shared
   *   through `run_token_store()`.
   * @tparam Program The program.
   * @param run The run.
   * @param effective The operator file, with the login's account.
   * @param strategy The program.
   * @return `run_account`'s exit.
   */
  template <typename Program>
  int start(AccountRun& run, const OperatorConfig& effective, Program&& strategy) {
    BuiltinSessions<Client> sessions(std::move(trading_config_), std::move(account_), std::move(market_config_));
    const auto make_clock = [](std::function<void()> after_slice) {
      return WallClock(std::move(after_slice));
    };
    return run_account(
      run, effective, std::forward<Program>(strategy), sessions.trading(), sessions.market(), make_clock
    );
  }

private:
  friend struct BuiltinBrokerTest;

  typename atl::BrokerTraits<Client>::Config trading_config_;
  typename atl::BrokerTraits<Client>::Account account_;
  std::optional<typename atl::BrokerTraits<Client>::Config> market_config_;
};

/**
 * @brief Runs a program on a built-in broker's account; see the file comment.
 * @tparam Program The program.
 * @tparam Client The broker's blocking client.
 * @tparam Symbols A callable from the run's arguments to its symbols.
 * @tparam Make A callable from the run's arguments and instruments to the program.
 * @param argc The count.
 * @param argv `run.id=ID`, then atlc's arguments.
 * @param symbols_of Gives the run's symbols.
 * @param make Builds the program.
 * @return The exit.
 */
template <typename Program, typename Client, typename Symbols, typename Make>
int builtin_account_main(int argc, char** argv, Symbols symbols_of, Make make) {
  BuiltinAccountBroker<Client> broker;
  return account_main<Program>(argc, argv, std::move(symbols_of), std::move(make), broker);
}

} // namespace driver
} // namespace cmdatl
