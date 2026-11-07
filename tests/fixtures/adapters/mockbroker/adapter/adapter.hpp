#pragma once

// A broker folder's adapter for tests: the library's mock broker behind the contract `user_account.hpp` reads. The
// login is `{"account": ID, "demo": BOOL}`; `{"fail": TEXT}` makes `connect` throw with TEXT in its message, as an
// adapter quoting a login would.

#include <algotradelib/mock>

#include <boost/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace cmdatl_adapter {

/** @brief The client. */
using Client = algotradelib::mock::MockClient;

/**
 * @brief The market `connect` trades on, set by the test before the run.
 * @return It, or null.
 */
inline algotradelib::mock::Market*& market() {
  static algotradelib::mock::Market* current = nullptr;
  return current;
}

/** @brief What `connect` returns. */
struct Connection {
  /** @brief The client. */
  std::shared_ptr<Client> client_;
  /** @brief The account. */
  algotradelib::mock::MockAccount account_;
  /** @brief The account the login drives. */
  std::string account_id_;
  /** @brief Whether the login is a demo one. */
  bool demo_{false};
};

/**
 * @brief Connects with a login.
 * @param login The login's JSON text.
 * @return The connection.
 */
inline Connection connect(const std::string& login) {
  const auto document = boost::json::parse(login).as_object();
  if (const auto* fail = document.if_contains("fail")) {
    throw std::runtime_error("cannot use the login " + std::string(fail->as_string()));
  }
  if (market() == nullptr) {
    throw std::runtime_error("no market");
  }
  Connection connection;
  connection.client_ = std::make_shared<Client>(*market());
  connection.account_id_ = std::string(document.at("account").as_string());
  connection.demo_ = document.contains("demo") && document.at("demo").as_bool();
  return connection;
}

} // namespace cmdatl_adapter
