#pragma once

#include <algotradelib/broker>

#include <boost/json.hpp>

namespace cmdatl {

/**
 * @brief What an account's broker offers, by cmdatl's own names, from the library's compile-time constants a command
 *   reads.
 * @tparam Client The broker client type.
 * @param clock Whether the account's time moves by command (`clock advance`): the paper broker only.
 * @return `working_orders`, `price_limits`, `buying_power`, `calendar`, `trigger_price` and `clock`.
 */
template <typename Client>
boost::json::object account_capabilities(bool clock) {
  using Caps = algotradelib::BrokerCaps<Client>;
  return boost::json::object{
    {"working_orders", Caps::has_working_orders},
    {"price_limits", Caps::has_price_limits},
    {"buying_power", Caps::has_buying_power},
    {"calendar", Caps::has_calendar},
    {"trigger_price", Caps::has_trigger_price},
    {"clock", clock},
  };
}

}  // namespace cmdatl
