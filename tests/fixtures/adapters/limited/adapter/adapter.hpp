#pragma once

// A broker folder's adapter for tests whose client states no trading calendar, no working-order listing and no
// reference data: the run driver must compile against it and refuse it at start.

#include <algotradelib/mock>

#include <memory>
#include <string>

namespace cmdatl_adapter {

/** @brief The mock client, under another type, so its traits can state less. */
class Client : public algotradelib::mock::MockClient {
public:
  using algotradelib::mock::MockClient::MockClient;
};

} // namespace cmdatl_adapter

/** @brief The mock broker's traits, without the calendar, the working orders and reference data. */
template <>
struct algotradelib::BrokerTraits<cmdatl_adapter::Client> : algotradelib::BrokerTraits<algotradelib::mock::MockClient> {
  /** @brief The client. */
  using Client = cmdatl_adapter::Client;
  /** @brief No working-order listing. */
  static constexpr bool has_working_orders = false;
  /** @brief No reference data. */
  static constexpr bool has_reference_data = false;
  /** @brief No calendar. */
  static constexpr bool has_calendar = false;
};

namespace cmdatl_adapter {

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
 * @brief Never connects: the driver refuses this client before it would.
 * @return Nothing usable.
 */
inline Connection connect(const std::string&, const std::string&) {
  return Connection{};
}

} // namespace cmdatl_adapter
