#pragma once

// What every built-in broker's clients share, across clients and processes: the access tokens and the trading calendar,
// in cmdatl's cache. A run, a command and a backtest's fetch on one key reuse one token instead of asking a new one.

#include "../system/files.hpp"

#include <algotradelib/network>

#include <memory>
#include <string>

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

} // namespace cmdatl
