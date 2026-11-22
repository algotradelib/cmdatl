#pragma once

// A built-in broker as a backtest's bar source: the connection `replay::fetch_main` takes, in the shape a broker folder's
// `connect` returns. The fetch reaches the session only through `replay::BarsOnly`; the account bound is the traits'
// `no_account()` where they declare one (it cannot trade), else `Account{}`, chosen generically.

#include "./derive.hpp"
#include "./stores.hpp"

#include <algotradelib/broker>
#include <algotradelib/network>

#include <boost/asio/ssl/context.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace cmdatl {
namespace brokers {

/** @brief Whether a client's traits declare `no_account()` (C++17 detection). */
template <typename Client, typename = void>
struct HasNoAccount : std::false_type {};
template <typename Client>
struct HasNoAccount<Client, std::void_t<decltype(algotradelib::BrokerTraits<Client>::no_account())>> : std::true_type {
};

/**
 * @brief A bar source's connection: the TLS context first (the client borrows it, so it is destroyed last), the client,
 *   the account bound, an empty account id and `demo_` false.
 * @tparam Client The broker's blocking client.
 */
template <typename Client>
struct Connection {
  std::unique_ptr<boost::asio::ssl::context> tls_;
  std::shared_ptr<Client> client_;
  typename algotradelib::BrokerTraits<Client>::Account account_{};
  std::string account_id_;
  bool demo_{false};
};

/**
 * @brief Connects a built-in broker for its history with a stored real login, or the demo one where the row says so
 *   (`demo_bars_`); sends nothing.
 * @tparam Client The broker's blocking client.
 * @param login The login (the library's JSON document).
 * @param options `{}`: a built-in takes none (the spec refused any).
 * @return The connection: paced at one request a second, KRX's tape where the config chooses one, on cmdatl's token
 *   store and calendar cache.
 * @throws std::exception for an unreadable login, one of the other environment, or options (`fetch_main` shows a fixed
 *   text).
 */
template <typename Client>
Connection<Client> connect_bars(const std::string& login, const std::string& options) {
  using Traits = algotradelib::BrokerTraits<Client>;
  typename Traits::Config config;
  config.from_json_text(login, "the stored login");
  if (is_demo(config) != facts_of<Client>().demo_bars_ || options != "{}") {
    throw std::invalid_argument("a bar source reads the login its facts name and takes no options");
  }
  set_tape(config);
  Connection<Client> out;
  if constexpr (HasNoAccount<Client>::value) {
    out.account_ = Traits::no_account();
  }
  out.tls_ = broker_tls();
  out.client_ = paced_client<Client>(*out.tls_, std::move(config), run_client_stores());
  return out;
}

/**
 * @brief Asks the broker which account a login's keys trade: one read of its account listing, which changes nothing;
 *   for a broker that lists its accounts (`BrokerCaps::has_account_discovery`) only.
 * @tparam Client The client.
 * @param login The canonical login document, without the number.
 * @return The account's number as the broker shows it.
 * @throws std::runtime_error naming whether the read reached the broker and a short code, never the broker's text.
 */
template <typename Client>
std::string discover_account(const std::string& login) {
  namespace atl = algotradelib;
  if constexpr (atl::BrokerCaps<Client>::has_account_discovery) {
    typename atl::BrokerTraits<Client>::Config config;
    config.from_json_text(login, "the login");
    // The context outlives the client, which is destroyed first.
    const auto tls = broker_tls();
    const auto client = paced_client<Client>(*tls, std::move(config), run_client_stores());
    const auto resolved = atl::resolve_account(*client, std::nullopt, std::string_view());
    if (!resolved) {
      const auto& code = resolved.error().broker_code_;
      const bool shown = !code.empty() && code.size() <= 32 &&
        code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") ==
          std::string::npos;
      throw std::runtime_error(
        std::string(
          resolved.error().reached_broker_ ?
            "the broker refused the account read" :
            "the account read did not reach the broker"
        ) +
        (shown ? " (code " + code + ")" : std::string())
      );
    }
    return resolved.value().display_number_;
  } else {
    static_cast<void>(login);
    throw std::logic_error("the broker's login carries its account");
  }
}

} // namespace brokers
} // namespace cmdatl
