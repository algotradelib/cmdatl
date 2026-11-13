#pragma once

// What a run and a command on a KIS account share: the logins checked against the operator's file, and the two
// sessions with the token store. Included by the KIS driver and by the command path built for KIS only, so nothing
// else compiles the KIS client.
//
// On a demo account the market data comes from the real login with an empty account, which cannot trade: a KIS demo
// host refuses reference data and the holiday calendar (`EGW02006`), as the library's `example-live-program` does.
// Both clients are paced at one request a second, the rate a KIS demo key is refused above (`EGW00201`).
// Both read only KRX's prices (the library reads KRX and NXT combined by default): a daily bar closes at 15:30.

#include "../system/files.hpp"
#include "./account.hpp"
#include "./operator.hpp"

#include <algotradelib/kis>
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
 * @brief The store a KIS account's clients share their access tokens through: the library's file store on
 *   `token_folder()`, its missing parents made `0700`. Without a cache folder it names none, and refuses at the first
 *   read, before anything is sent.
 * @return The store.
 */
inline std::shared_ptr<algotradelib::network::TokenStore> run_token_store() {
  const auto folder = token_folder().value_or(std::string());
  (void)make_private_dirs(folder);
  return std::make_shared<algotradelib::network::FileTokenStore>(folder);
}

/**
 * @brief The cache a KIS account's clients share the trading calendar through, so a later process on the same KST day
 *   makes no holiday inquiry: the library's file cache on `cache_folder("calendars")`, made as the token folder is.
 *   Without a cache folder it names none: the calendar is then asked of KIS each time.
 * @return The cache.
 */
inline std::shared_ptr<algotradelib::network::CalendarCache> run_calendar_cache() {
  const auto folder = cache_folder("calendars").value_or(std::string());
  (void)make_private_dirs(folder);
  return std::make_shared<algotradelib::network::FileCalendarCache>(folder);
}

/**
 * @brief Whether a login is the operator file's account: the account number and product code as `12345678-01`, and the
 *   environment.
 * @param number The operator file's `[account] number`, or empty when it names none (the login decides).
 * @param demo Whether the operator file's environment is demo.
 * @param account_number The login's account number.
 * @param product_code The login's product code.
 * @param login_demo Whether the login is a demo login.
 * @return What differs, or nothing.
 */
inline std::optional<std::string> kis_account_mismatch(
  const std::string& number, bool demo, const std::string& account_number, const std::string& product_code,
  bool login_demo
) {
  if (login_demo != demo) {
    return std::string("the login is a ") + (login_demo ? "demo" : "real") +
      " login, and the operator file's account is " + (demo ? "demo" : "real");
  }
  if (!number.empty() && account_number + "-" + product_code != number) {
    return "the login's account is not the operator file's account.number " + number;
  }
  return std::nullopt;
}

/**
 * @brief Reads a KIS account's logins and checks them against the operator's file; every config reads KRX's tape.
 * @param config The operator's file.
 * @param documents The account's login, then on demo the real login (read for market data only).
 * @param trading Set to the account's config.
 * @param account Set to the account.
 * @param market Set to the real login's config on demo.
 * @param effective Its number set to the login's account, which the operator file's number, when given, matched.
 * @return What is wrong (`auth.account_mismatch`), or nothing; the loaders' messages may quote what they read, so they
 *   are never repeated.
 */
inline std::optional<std::string> read_kis_logins(
  const OperatorConfig& config, const std::vector<std::string>& documents, algotradelib::kis::KisConfig& trading,
  algotradelib::kis::KisAccount& account, std::optional<algotradelib::kis::KisConfig>& market, OperatorConfig& effective
) {
  namespace kis = algotradelib::kis;
  const bool demo = config.env_ == "demo";
  try {
    trading.from_json_text(documents[0], "the stored login");
    account.from_json_text(documents[0], "the stored login");
  } catch (const std::exception&) {
    return std::string("the stored login could not be read; log in again");
  }
  if (
    const auto mismatch =
      kis_account_mismatch(config.number_, demo, account.account_number_, account.product_code_, trading.is_demo_)
  ) {
    return mismatch;
  }
  effective.number_ = account.account_number_ + "-" + account.product_code_;
  // One tape for every read, set on each config: bars never come from another book than the quotes.
  trading.market_tape_ = kis::MarketTape::KRX;
  if (!demo) {
    return std::nullopt;
  }
  kis::KisConfig real;
  try {
    real.from_json_text(documents[1], "the stored real login");
  } catch (const std::exception&) {
    return std::string("the stored real login could not be read; log in again");
  }
  if (real.is_demo_) {
    return std::string("the login stored as kis/real is a demo login");
  }
  real.market_tape_ = kis::MarketTape::KRX;
  market = std::move(real);
  return std::nullopt;
}

/**
 * @brief A KIS account's two sessions: the account's, and on demo the real login's with no account, for market data;
 *   both clients paced at one request a second, their tokens and calendar shared through `run_token_store()` and
 *   `run_calendar_cache()`.
 *
 * Constructing them sends nothing.
 */
class KisSessions {
public:
  /** @brief A KIS session. */
  using Session = algotradelib::BlockingSession<algotradelib::kis::KisBlockingClient>;

  /**
   * @brief Builds the sessions.
   * @param trading The account's config.
   * @param account The account.
   * @param market The real login's config on demo, or nothing (the account's session reads the market data).
   */
  KisSessions(
    algotradelib::kis::KisConfig trading, algotradelib::kis::KisAccount account,
    std::optional<algotradelib::kis::KisConfig> market
  )
  : tls_(boost::asio::ssl::context::tls_client), tokens_(run_token_store()), calendars_(run_calendar_cache()) {
    tls_.set_default_verify_paths();
    tls_.set_verify_mode(boost::asio::ssl::verify_peer);
    trading_.emplace(client(std::move(trading)), std::move(account));
    if (market) {
      // No account: a session that cannot trade, so a swap by mistake places nothing.
      market_.emplace(client(std::move(*market)), algotradelib::kis::KisAccount{});
    }
  }

  KisSessions(const KisSessions&) = delete;
  KisSessions& operator=(const KisSessions&) = delete;

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
   * @brief A paced client on the shared token store.
   * @param config Its config.
   * @return The client.
   */
  std::shared_ptr<algotradelib::kis::KisBlockingClient> client(algotradelib::kis::KisConfig config) {
    algotradelib::network::HttpClientOptions paced;
    paced.rate_limit_ = algotradelib::network::RateLimitBudget{1, 1, 0};
    return std::make_shared<algotradelib::kis::KisBlockingClient>(tls_, std::move(config), paced, tokens_, calendars_);
  }

  boost::asio::ssl::context tls_;
  std::shared_ptr<algotradelib::network::TokenStore> tokens_;
  std::shared_ptr<algotradelib::network::CalendarCache> calendars_;
  std::optional<Session> trading_;
  std::optional<Session> market_;
};

} // namespace cmdatl
