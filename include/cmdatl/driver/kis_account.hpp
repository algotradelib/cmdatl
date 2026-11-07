#pragma once

// The KIS part of an account run (`account_main.hpp`): the logins checked against the operator's file, the trading
// session and the market-data session built, then `run_account`.
//
// On a demo account the market data comes from the real login with an empty account, which cannot trade: a KIS demo
// host refuses reference data and the holiday calendar (`EGW02006`), as the library's `example-live-program` does.
// Both clients are paced at one request a second, the rate a KIS demo key is refused above (`EGW00201`).

#include "./account_main.hpp"

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

/** @brief The KIS part of an account run (see `account_main.hpp`): two logins, the account's and, on demo, the real one. */
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
   * @param market_data Where market data comes from, for the start line.
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
    try {
      trading_config_.from_json_text(documents[0], "the stored login");
      account_.from_json_text(documents[0], "the stored login");
    } catch (const std::exception&) {
      // The loaders' messages may quote what they read: never repeated.
      return run_error(base, Exit::FAILED, "auth.account_mismatch", "the stored login could not be read; log in again");
    }
    if (
      const auto mismatch = kis_account_mismatch(
        config.number_, demo, account_.account_number_, account_.product_code_, trading_config_.is_demo_
      )
    ) {
      return run_error(base, Exit::FAILED, "auth.account_mismatch", *mismatch);
    }
    // The account the run trades: the login's, which the operator file's number, when given, has just matched.
    effective.number_ = account_.account_number_ + "-" + account_.product_code_;
    if (demo) {
      atl::kis::KisConfig real;
      try {
        real.from_json_text(documents[1], "the stored real login");
      } catch (const std::exception&) {
        return run_error(
          base, Exit::FAILED, "auth.account_mismatch", "the stored real login could not be read; log in again"
        );
      }
      if (real.is_demo_) {
        return run_error(base, Exit::FAILED, "auth.account_mismatch", "the login stored as kis/real is a demo login");
      }
      market_config_ = std::move(real);
      market_data = "kis-real (read only)";
    }
    return std::nullopt;
  }

  /**
   * @brief Builds the sessions and trades: both clients paced at one request a second.
   * @tparam Program The program.
   * @param run The run.
   * @param effective The operator file, with the login's account.
   * @param strategy The program.
   * @return `run_account`'s exit.
   */
  template <typename Program>
  int start(AccountRun& run, const OperatorConfig& effective, Program&& strategy) {
    namespace ssl = boost::asio::ssl;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);
    atl::network::HttpClientOptions paced;
    paced.rate_limit_ = atl::network::RateLimitBudget{1, 1, 0};
    using Session = atl::BlockingSession<atl::kis::KisBlockingClient>;
    Session session(
      std::make_shared<atl::kis::KisBlockingClient>(tls, std::move(trading_config_), paced), std::move(account_)
    );
    const auto make_clock = [](std::function<void()> after_slice) {
      return WallClock(std::move(after_slice));
    };
    if (market_config_) {
      // No account: a session that cannot trade, so a swap by mistake places nothing.
      Session market(
        std::make_shared<atl::kis::KisBlockingClient>(tls, std::move(*market_config_), paced), atl::kis::KisAccount{}
      );
      return run_account(run, effective, std::forward<Program>(strategy), session, market, make_clock);
    }
    return run_account(run, effective, std::forward<Program>(strategy), session, session, make_clock);
  }

private:
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
