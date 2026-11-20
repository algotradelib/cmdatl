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
 * @brief Connects a built-in broker for its history with a stored real login; sends nothing.
 * @tparam Client The broker's blocking client.
 * @param login The login (the library's JSON document).
 * @param options `{}`: a built-in takes none (the spec refused any).
 * @return The connection: paced at one request a second, KRX's tape where the config chooses one, on cmdatl's token
 *   store and calendar cache.
 * @throws std::exception for an unreadable or demo login, or options (`fetch_main` shows a fixed text).
 */
template <typename Client>
Connection<Client> connect_bars(const std::string& login, const std::string& options) {
  using Traits = algotradelib::BrokerTraits<Client>;
  typename Traits::Config config;
  config.from_json_text(login, "the stored login");
  if (is_demo(config) || options != "{}") {
    throw std::invalid_argument("a bar source reads a real login and takes no options");
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

} // namespace brokers
} // namespace cmdatl
