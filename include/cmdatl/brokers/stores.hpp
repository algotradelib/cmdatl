#pragma once

// What every built-in broker's clients share, across clients and processes: the access tokens and the trading calendar,
// in cmdatl's cache. A run, a command and a backtest's fetch on one key reuse one token instead of asking a new one.

#include "../system/files.hpp"

#include <algotradelib/network>

#include <boost/asio/ssl/context.hpp>

#include <memory>
#include <string>
#include <utility>

namespace cmdatl {

/**
 * @brief The store a built-in broker's clients share their access tokens through: the library's file store on
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
 * @brief The cache a built-in broker's clients share the trading calendar through, so a later process on the same KST
 *   day makes no holiday inquiry: the library's file cache on `cache_folder("calendars")`, made as the token folder is.
 *   Without a cache folder it names none: the calendar is then asked of the broker each time.
 * @return The cache.
 */
inline std::shared_ptr<algotradelib::network::CalendarCache> run_calendar_cache() {
  const auto folder = cache_folder("calendars").value_or(std::string());
  (void)make_private_dirs(folder);
  return std::make_shared<algotradelib::network::FileCalendarCache>(folder);
}

/**
 * @brief The stores a built-in client is constructed with (`network::make_client`); a client whose constructor takes no
 *   calendar cache uses the token store alone.
 * @return The token store and the calendar cache.
 */
inline algotradelib::network::ClientStores run_client_stores() {
  return algotradelib::network::ClientStores{run_token_store(), run_calendar_cache()};
}

/**
 * @brief A TLS context for a built-in broker's clients, which verifies the broker's certificate with the system's roots.
 * @return The context; the clients borrow it, so it outlives them.
 */
inline std::unique_ptr<boost::asio::ssl::context> broker_tls() {
  auto tls = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
  tls->set_default_verify_paths();
  tls->set_verify_mode(boost::asio::ssl::verify_peer);
  return tls;
}

/**
 * @brief A built-in broker's client, paced at one request a second (the rate a KIS demo key is refused above,
 *   `EGW00201`), on the given stores; constructing it sends nothing.
 * @tparam Client The broker's blocking client.
 * @param tls The TLS context, borrowed (`broker_tls`).
 * @param config Its config.
 * @param stores The stores (`run_client_stores`).
 * @return The client.
 */
template <typename Client, typename Config>
std::shared_ptr<Client> paced_client(
  boost::asio::ssl::context& tls, Config config, const algotradelib::network::ClientStores& stores
) {
  algotradelib::network::HttpClientOptions paced;
  paced.rate_limit_ = algotradelib::network::RateLimitBudget{1, 1, 0};
  return algotradelib::network::make_client<Client>(tls, std::move(config), paced, stores);
}

} // namespace cmdatl
