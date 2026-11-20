#pragma once

// What a run and a command on a built-in broker's account share: the logins checked against the operator's file, and
// the two sessions on the shared stores (`brokers/stores.hpp`). Included by the built-in driver and by the command path
// (`cli/builtin.hpp`), so nothing else compiles a broker's client.
//
// Where the facts say a demo account's market data comes from the real login (KIS: a demo host refuses reference data
// and the holiday calendar, `EGW02006`), it is read with the traits' `no_account()`, which cannot trade, as the
// library's `example-live-program` does. Every client is paced at `builtin::REQUESTS_PER_SECOND` (one request a second,
// the rate a KIS demo key is refused above, `EGW00201`), and reads the tape the facts choose (KIS: KRX's alone, so a
// daily bar closes at 15:30).

#include "../brokers/derive.hpp"
#include "../brokers/stores.hpp"
#include "../core/builtins.hpp"
#include "./account.hpp"
#include "./operator.hpp"

#include <algotradelib/broker>
#include <algotradelib/network>

#include <boost/asio/ssl/context.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

/**
 * @brief Whether a login is the operator file's account: its account id, and the environment.
 * @param number The operator file's `[account] number`, or empty when it names none (the login decides).
 * @param demo Whether the operator file's environment is demo.
 * @param account_id The login's account as the broker names it (`display_of`, KIS: `12345678-01`).
 * @param login_demo Whether the login is a demo login.
 * @return What differs, or nothing.
 */
inline std::optional<std::string> account_mismatch(
  const std::string& number, bool demo, const std::string& account_id, bool login_demo
) {
  if (login_demo != demo) {
    return std::string("the login is a ") + (login_demo ? "demo" : "real") +
      " login, and the operator file's account is " + (demo ? "demo" : "real");
  }
  if (!number.empty() && account_id != number) {
    return "the login's account is not the operator file's account.number " + number;
  }
  return std::nullopt;
}

/**
 * @brief Reads a built-in account's logins and checks them against the operator's file; every config reads the tape
 *   the facts choose.
 * @tparam Client The broker's blocking client, one whose login carries its account.
 * @param config The operator's file.
 * @param documents The account's login, then where the facts say so on demo the real login (read for market data only).
 * @param trading Set to the account's config.
 * @param account Set to the account.
 * @param market Set to the real login's config where that login is read.
 * @param effective Its number set to the login's account, which the operator file's number, when given, matched.
 * @return What is wrong (`auth.account_mismatch`), or nothing; the loaders' messages may quote what they read, so they
 *   are never repeated.
 */
template <typename Client>
std::optional<std::string> read_builtin_logins(
  const OperatorConfig& config, const std::vector<std::string>& documents,
  typename algotradelib::BrokerTraits<Client>::Config& trading,
  typename algotradelib::BrokerTraits<Client>::Account& account,
  std::optional<typename algotradelib::BrokerTraits<Client>::Config>& market, OperatorConfig& effective
) {
  using Traits = algotradelib::BrokerTraits<Client>;
  static_assert(
    !algotradelib::BrokerCaps<Client>::has_account_discovery,
    "an account the broker lists (resolve_account) comes with that broker's account branch"
  );
  constexpr const auto& facts = brokers::facts_of<Client>();
  const bool demo = config.env_ == "demo";
  try {
    trading.from_json_text(documents[0], "the stored login");
    account.from_json_text(documents[0], "the stored login");
  } catch (const std::exception&) {
    return std::string("the stored login could not be read; log in again");
  }
  const std::string account_id = Traits::display_of(account);
  if (account_id.empty()) {
    return std::string("the stored login could not be read; log in again");
  }
  if (const auto mismatch = account_mismatch(config.number_, demo, account_id, brokers::is_demo(trading))) {
    return mismatch;
  }
  effective.number_ = account_id;
  // One tape for every read, set on each config: bars never come from another book than the quotes.
  brokers::set_tape<Client>(trading);
  if constexpr (facts.market_data_ == builtin::MarketDataLogin::REAL_WHEN_DEMO) {
    if (!demo) {
      return std::nullopt;
    }
    typename Traits::Config real;
    try {
      real.from_json_text(documents[1], "the stored real login");
    } catch (const std::exception&) {
      return std::string("the stored real login could not be read; log in again");
    }
    if (brokers::is_demo(real)) {
      return "the login stored as " + std::string(facts.word_) + "/real is a demo login";
    }
    brokers::set_tape<Client>(real);
    market = std::move(real);
  }
  return std::nullopt;
}

/**
 * @brief A built-in account's two sessions: the account's, and where the facts say so on demo the real login's with
 *   `no_account()`, for market data; every client paced at `builtin::REQUESTS_PER_SECOND`, on `run_client_stores()`.
 *
 * Constructing them sends nothing.
 * @tparam Client The broker's blocking client.
 */
template <typename Client>
class BuiltinSessions {
public:
  /** @brief The broker's session. */
  using Session = algotradelib::BlockingSession<Client>;
  /** @brief The broker's config. */
  using Config = typename algotradelib::BrokerTraits<Client>::Config;

  /**
   * @brief Builds the sessions.
   * @param trading The account's config.
   * @param account The account.
   * @param market The real login's config on demo, or nothing (the account's session reads the market data).
   */
  BuiltinSessions(
    Config trading, typename algotradelib::BrokerTraits<Client>::Account account, std::optional<Config> market
  )
  : tls_(boost::asio::ssl::context::tls_client), stores_(run_client_stores()) {
    tls_.set_default_verify_paths();
    tls_.set_verify_mode(boost::asio::ssl::verify_peer);
    trading_.emplace(client(std::move(trading)), std::move(account));
    if constexpr (brokers::facts_of<Client>().market_data_ == builtin::MarketDataLogin::REAL_WHEN_DEMO) {
      if (market) {
        // No account: a session that cannot trade, so a swap by mistake places nothing.
        market_.emplace(client(std::move(*market)), algotradelib::BrokerTraits<Client>::no_account());
      }
    } else {
      static_cast<void>(market);
    }
  }

  BuiltinSessions(const BuiltinSessions&) = delete;
  BuiltinSessions& operator=(const BuiltinSessions&) = delete;

  /** @brief The account's session. */
  Session& trading() {
    return *trading_;
  }

  /** @brief The session market data is read from: the real login's on demo, else the account's. */
  Session& market() {
    return market_ ? *market_ : *trading_;
  }

private:
  /**
   * @brief A paced client on the shared stores.
   * @param config Its config.
   * @return The client.
   */
  std::shared_ptr<Client> client(Config config) {
    algotradelib::network::HttpClientOptions paced;
    paced.rate_limit_ =
      algotradelib::network::RateLimitBudget{builtin::REQUESTS_PER_SECOND, builtin::REQUESTS_PER_SECOND, 0};
    return algotradelib::network::make_client<Client>(tls_, std::move(config), paced, stores_);
  }

  boost::asio::ssl::context tls_;
  algotradelib::network::ClientStores stores_;
  std::optional<Session> trading_;
  std::optional<Session> market_;
};

} // namespace cmdatl
