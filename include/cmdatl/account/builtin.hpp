#pragma once

// What a run and a command on a built-in broker's account share: the logins checked against the operator's file, and
// the two sessions with the token store and the calendar cache. Included by the built-in driver and by the command path
// (`cli/builtin.hpp`), so nothing else compiles a broker's client.
//
// Where the facts say a demo account's market data comes from the real login (KIS: a demo host refuses reference data
// and the holiday calendar, `EGW02006`), it is read with the traits' `no_account()`, which cannot trade, as the
// library's `example-live-program` does. Every client is paced at one request a second, the rate a KIS demo key is
// refused above (`EGW00201`), and reads KRX's tape wherever its config chooses one (`brokers::set_tape`).
// A broker that lists its accounts (Alpaca: the keys are the account) has its number learned by `auth login` and stored
// in the login (`brokers::ACCOUNT_NUMBER_KEY`), as KIS's login carries its own; `confirm_account` checks it with the
// broker before the account is opened.

#include "../brokers/derive.hpp"
#include "../brokers/stores.hpp"
#include "../core/builtins.hpp"
#include "./account.hpp"
#include "./operator.hpp"

#include <algotradelib/broker>
#include <algotradelib/network>

#include <boost/asio/ssl/context.hpp>
#include <boost/json.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
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
 * @brief Reads a built-in account's logins and checks them against the operator's file; every config reads KRX's tape
 *   where it chooses one.
 *
 * Where the broker lists its accounts the number is the one `auth login` learned from the broker and stored in the
 * login; the operator file's, when written, must equal it (`confirm_account` checks it with the broker), and `account`
 * stays the traits' default. A login stored before cmdatl learned the number is refused with what to run.
 * @tparam Client The broker's blocking client.
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
  constexpr bool discovers = algotradelib::BrokerCaps<Client>::has_account_discovery;
  constexpr const auto& facts = brokers::facts_of<Client>();
  const bool demo = config.env_ == "demo";
  try {
    trading.from_json_text(documents[0], "the stored login");
    if constexpr (!discovers) {
      account.from_json_text(documents[0], "the stored login");
    }
  } catch (const std::exception&) {
    return std::string("the stored login could not be read; log in again");
  }
  if constexpr (discovers) {
    boost::system::error_code ec;
    const auto parsed = boost::json::parse(documents[0], ec);
    const auto* number =
      !ec && parsed.is_object() ? parsed.as_object().if_contains(brokers::ACCOUNT_NUMBER_KEY) : nullptr;
    if (number == nullptr || !number->is_string() || number->get_string().empty()) {
      const std::string login = "cmdatl auth login --broker " + std::string(facts.word_) + " --env " + config.env_;
      return "the stored login has no account number (it was stored before cmdatl learned it): run " + login + " again";
    }
    const std::string account_id(number->get_string());
    if (const auto mismatch = account_mismatch(config.number_, demo, account_id, brokers::is_demo(trading))) {
      return mismatch;
    }
    effective.number_ = account_id;
  } else {
    const std::string account_id = Traits::display_of(account);
    if (account_id.empty()) {
      return std::string("the stored login could not be read; log in again");
    }
    if (const auto mismatch = account_mismatch(config.number_, demo, account_id, brokers::is_demo(trading))) {
      return mismatch;
    }
    effective.number_ = account_id;
  }
  // One tape for every read, set on each config: bars never come from another book than the quotes.
  brokers::set_tape(trading);
  if constexpr (facts.real_market_data_on_demo_) {
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
    brokers::set_tape(real);
    market = std::move(real);
  }
  return std::nullopt;
}

/** @brief What `confirm_account` found. */
struct AccountCheck {
  /** @brief The broker lists the operator file's account (or the broker lists none, and the login carries it). */
  bool confirmed_{true};
  /** @brief The broker refused the number (`auth.account_mismatch`), rather than failing to answer. */
  bool mismatch_{false};
  /** @brief What went wrong, in cmdatl's words: of a broker's error only a short printable code is shown. */
  std::string message_;
};

/**
 * @brief Checks with the broker that it lists the operator file's account, where the broker lists its accounts; one
 *   listing request, which changes nothing at the broker; nothing for any other broker.
 * @tparam Session The account's session.
 * @param session The session, on the login's client.
 * @param number The operator file's account number.
 * @return Confirmed, a mismatch, or an unanswered listing.
 */
template <typename Session>
AccountCheck confirm_account(Session& session, const std::string& number) {
  namespace atl = algotradelib;
  using Client = std::decay_t<decltype(session.client())>;
  AccountCheck check;
  if constexpr (atl::BrokerCaps<Client>::has_account_discovery) {
    const auto resolved = atl::resolve_account(session.client(), std::nullopt, number);
    // The library matches without separators; the number names the account's directory and journal, so only the
    // broker's own spelling is one account.
    if (resolved && resolved.value().display_number_ == number) {
      return check;
    }
    check.confirmed_ = false;
    if (resolved) {
      check.mismatch_ = true;
      check.message_ = "the operator file's account.number " + number +
        " is not written exactly as the broker shows the login's account; write it as the broker shows it";
      return check;
    }
    const auto& error = resolved.error();
    const auto& code = error.broker_code_;
    check.mismatch_ = error.category_ == atl::ErrorCategory::INVALID_ARGUMENT &&
      (code == "account_not_found" || code == "no_accounts" || code == "several_accounts" ||
        code == "account_without_number");
    if (check.mismatch_) {
      check.message_ = "the login's account is not the operator file's account.number " + number;
    } else {
      // The broker's message may quote its reply body: a short printable code at most, as a read's failure shows.
      check.message_ = "the broker did not answer the account list";
      if (
        !code.empty() && code.size() <= 32 &&
        code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") == std::string::npos
      ) {
        check.message_ += " (code " + code + ")";
      }
    }
  } else {
    static_cast<void>(session);
    static_cast<void>(number);
  }
  return check;
}

/**
 * @brief A built-in account's two sessions: the account's, and where the facts say so on demo the real login's with
 *   `no_account()`, for market data; every client paced at one request a second, on the shared token store and calendar cache.
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
  : tls_(broker_tls()), stores_(run_client_stores()) {
    trading_.emplace(paced_client<Client>(*tls_, std::move(trading), stores_), std::move(account));
    if constexpr (brokers::facts_of<Client>().real_market_data_on_demo_) {
      if (market) {
        // No account: a session that cannot trade, so a swap by mistake places nothing.
        market_.emplace(
          paced_client<Client>(*tls_, std::move(*market), stores_), algotradelib::BrokerTraits<Client>::no_account()
        );
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
  std::unique_ptr<boost::asio::ssl::context> tls_;
  algotradelib::network::ClientStores stores_;
  std::optional<Session> trading_;
  std::optional<Session> market_;
};

} // namespace cmdatl
