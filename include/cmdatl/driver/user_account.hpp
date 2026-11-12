#pragma once

// A broker folder's part of an account run (`account_main.hpp`): the folder's `adapter.hpp`, copied into the build,
// gives the client and `connect`; this file checks what it returned and trades through the generic run.
//
// The adapter's contract, in namespace `cmdatl_adapter`:
// - `using Client = ...;`: a blocking client with `algotradelib::BrokerTraits<Client>`;
// - `connect(const std::string& login)`: from the stored login (JSON text), an object with
//   `std::shared_ptr<Client> client_`, `BrokerTraits<Client>::Account account_`, `std::string account_id_` (the account
//   the login drives: stable, never a credential) and `bool demo_`; it owns what the client borrows, for the whole run,
//   and throws on a login it cannot use.
//
// Fail closed for code cmdatl has not seen: a client stating no trading calendar, no working-order listing or no
// reference data, or a data-only one, is refused before anything is read (the run reads each at its start), and the
// generic run is never compiled for it. The adapter's error text is replaced before the run sees it (`ScrubbedSession`)
// and an exception's is not shown: either could carry a credential (a vendor that puts its token in a URL).

#include "../replay/adapter_text.hpp"
#include "./account_main.hpp"

#include <algotradelib/broker>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// The driver includes the folder's `adapter.hpp` (copied into the build as `adapter/adapter.hpp`) before this file.
#if !defined(CMDATL_ADAPTER_NAME)
#error "CMDATL_ADAPTER_NAME names the broker folder this driver is built for"
#endif
#if !defined(CMDATL_ADAPTER_FOLDER)
#error "CMDATL_ADAPTER_FOLDER is the canonical path of the broker folder this driver is built from"
#endif

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;
namespace json = boost::json;

/**
 * @brief A broker folder's session as the run sees it: every call forwarded, every error's text replaced by
 *   `replay::adapter_error_text`, its code kept only when short and plain (`replay::shown_code`), its request id cleared.
 * @tparam Inner The adapter's session.
 */
template <typename Inner>
class ScrubbedSession {
public:
  /** @brief The broker's capabilities. */
  using Caps = typename Inner::Caps;
  /** @brief The broker's tag. */
  using BrokerTag = typename Inner::BrokerTag;
  /** @brief The broker's traits. */
  using Traits = typename Inner::Traits;
  /** @brief An order reference. */
  using Ref = typename Inner::Ref;

  /**
   * @brief Wraps a session.
   * @param inner The session; borrowed.
   */
  explicit ScrubbedSession(Inner& inner) : inner_(&inner) {}

  /** @brief Forwarded. */
  void close() noexcept {
    inner_->close();
  }
  /** @brief Forwarded. @param order The order. @return The answer. */
  auto place(atl::OrderRequest order) {
    return scrub(inner_->place(std::move(order)));
  }
  /** @brief Forwarded. @param ref The order. @param instruction What to cancel. @return The answer. */
  auto cancel(const Ref& ref, const atl::AmendInstruction& instruction) {
    return scrub(inner_->cancel(ref, instruction));
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto bars(atl::BarQuery query) {
    return scrub(inner_->bars(std::move(query)));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto quote(atl::InstrumentId instrument) {
    return scrub(inner_->quote(std::move(instrument)));
  }
  /** @brief Forwarded. @param subject The instrument. @return The answer. */
  auto instrument(atl::InstrumentId subject) {
    return scrub(inner_->instrument(std::move(subject)));
  }
  /** @brief Forwarded. @param country The country. @param from The first date. @return The answer. */
  auto calendar(atl::Country country, std::optional<atl::Date> from = std::nullopt) {
    return scrub(inner_->calendar(country, std::move(from)));
  }
  /** @brief Forwarded. @return The answer. */
  auto holdings() {
    return scrub(inner_->holdings());
  }
  /** @brief Forwarded. @param currency The currency. @return The answer. */
  auto buying_power(atl::Currency currency) {
    return scrub(inner_->buying_power(std::move(currency)));
  }
  /** @brief Forwarded. @return The answer. */
  auto working_orders() {
    return scrub(inner_->working_orders());
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto order_history(const atl::OrderHistoryQuery& query) {
    return scrub(inner_->order_history(query));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto sellable_quantity(atl::InstrumentId instrument) {
    return scrub(inner_->sellable_quantity(std::move(instrument)));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto price_limits(atl::InstrumentId instrument) {
    return scrub(inner_->price_limits(std::move(instrument)));
  }

private:
  template <typename T>
  static atl::Result<T> scrub(atl::Result<T> answer) {
    if (!answer) {
      // The whole error normalized here: the boundary and the events read its code as a refusal's reason.
      auto& error = answer.error();
      error.message_ = replay::adapter_error_text(error);
      error.broker_code_ = replay::shown_code(error.broker_code_);
      error.request_id_.clear();
    }
    return answer;
  }

  Inner* inner_;
};

/** @brief The broker folder's part of an account run; see the file comment. */
class UserAccountBroker {
public:
  /** @brief The adapter's client. */
  using Client = cmdatl_adapter::Client;
  /** @brief Its capabilities. */
  using Caps = atl::BrokerCaps<Client>;
  /** @brief What `connect` returns. */
  using Connection = std::decay_t<decltype(cmdatl_adapter::connect(std::declval<const std::string&>()))>;

  static_assert(
    atl::BrokerTraits<Client>::blocking, "a broker folder's Client must be blocking (BrokerTraits<Client>::blocking)"
  );
  static_assert(
    std::is_same<std::decay_t<decltype(std::declval<Connection&>().client_)>, std::shared_ptr<Client>>::value,
    "cmdatl_adapter::connect must return an object whose client_ is a std::shared_ptr<cmdatl_adapter::Client>"
  );
  static_assert(
    std::is_same<
      std::decay_t<decltype(std::declval<Connection&>().account_)>, typename atl::BrokerTraits<Client>::Account
    >::value,
    "cmdatl_adapter::connect must return an object whose account_ is BrokerTraits<Client>::Account"
  );
  static_assert(
    std::is_same<std::decay_t<decltype(std::declval<Connection&>().account_id_)>, std::string>::value,
    "cmdatl_adapter::connect must return an object whose account_id_ is a std::string"
  );
  static_assert(
    std::is_same<std::decay_t<decltype(std::declval<Connection&>().demo_)>, bool>::value,
    "cmdatl_adapter::connect must return an object whose demo_ is a bool"
  );

  /** @brief Whether the run can be compiled for the client: what the run reads at its start, and orders. */
  static constexpr bool runnable =
    !Caps::data_only && Caps::has_calendar && Caps::has_working_orders && Caps::has_reference_data;

  /**
   * @brief The broker the driver was built for.
   * @return The folder's name.
   */
  std::string name() const {
    return CMDATL_ADAPTER_NAME;
  }

  /**
   * @brief How many logins `cmdatl run` frames: the account's.
   * @return 1.
   */
  std::size_t logins() const {
    return 1;
  }

  /**
   * @brief Refuses a client the run cannot trade through, then connects with the login and checks the account.
   * @param base The reply's base.
   * @param config The operator file.
   * @param documents The login.
   * @param effective Its number set to the adapter's account id.
   * @param market_data Where market data comes from: the same session.
   * @return The exit after a refusal, or nothing.
   */
  std::optional<int> identify(
    const json::object& base, const OperatorConfig& config, const std::vector<std::string>& documents,
    OperatorConfig& effective, std::string& market_data
  ) {
    // The operator file, read again by this driver, names the folder it was built from, or nothing is read or sent.
    std::error_code ec;
    const auto folder = std::filesystem::canonical(config.adapter_, ec);
    if (ec || folder.string() != CMDATL_ADAPTER_FOLDER) {
      return run_error(
        base, Exit::FAILED, "unsupported.broker",
        std::string("this driver was built from the broker folder ") + CMDATL_ADAPTER_FOLDER +
          ", and the operator file names " + config.adapter_
      );
    }
    json::array missing;
    if (Caps::data_only) {
      missing.emplace_back("data_only");
    }
    if (!Caps::has_calendar) {
      missing.emplace_back("has_calendar");
    }
    if (!Caps::has_working_orders) {
      missing.emplace_back("has_working_orders");
    }
    if (!Caps::has_reference_data) {
      missing.emplace_back("has_reference_data");
    }
    if (!missing.empty()) {
      return run_error(
        base, Exit::FAILED, "unsupported.capability",
        "the broker folder's client does not state what a run reads at its start (a trading calendar, the working "
        "orders, reference data) or is a data vendor; nothing was read or sent",
        "trade this broker another way; the adapter states its capabilities in BrokerTraits",
        {{"missing", std::move(missing)}}
      );
    }
    const bool demo = config.env_ == "demo";
    if (documents[0].empty()) {
      return run_error(
        base, Exit::FAILED, "run.failed", "the driver received no logins on descriptor 3; start the run with cmdatl run"
      );
    }
    try {
      connection_.emplace(cmdatl_adapter::connect(documents[0]));
    } catch (...) {
      // The adapter's text is not cmdatl's: it could quote the login.
      return run_error(
        base, Exit::FAILED, "auth.account_mismatch",
        "the broker folder's adapter could not use the stored login (its text is not shown); log in again"
      );
    }
    if (!connection_->client_) {
      return run_error(base, Exit::FAILED, "auth.account_mismatch", "the broker folder's adapter gave no client");
    }
    if (connection_->demo_ != demo) {
      return run_error(
        base, Exit::FAILED, "auth.account_mismatch",
        std::string("the login is a ") + (connection_->demo_ ? "demo" : "real") +
          " login, and the operator file's account is " + (demo ? "demo" : "real")
      );
    }
    if (!account_number_text(connection_->account_id_)) {
      return run_error(
        base, Exit::FAILED, "auth.account_mismatch",
        "the broker folder's adapter named no account a journal can be kept under (1 to 64 letters, digits, '_' or "
        "'-')"
      );
    }
    if (!config.number_.empty() && config.number_ != connection_->account_id_) {
      return run_error(
        base, Exit::FAILED, "auth.account_mismatch",
        "the login's account is not the operator file's account.number " + config.number_
      );
    }
    effective.number_ = connection_->account_id_;
    market_data = "own";
    return std::nullopt;
  }

  /**
   * @brief Trades through the adapter's session, its errors scrubbed, on the wall clock.
   * @tparam Program The program.
   * @param run The run.
   * @param effective The operator file, with the adapter's account.
   * @param strategy The program.
   * @return `run_account`'s exit.
   */
  template <typename Program>
  int start(AccountRun& run, const OperatorConfig& effective, Program&& strategy) {
    run.opaque_errors_ = true;
    if constexpr (runnable) {
      atl::BlockingSession<Client> adapter(connection_->client_, std::move(connection_->account_));
      ScrubbedSession<atl::BlockingSession<Client>> session(adapter);
      const auto make_clock = [](std::function<void()> after_slice) {
        return WallClock(std::move(after_slice));
      };
      return run_account(run, effective, std::forward<Program>(strategy), session, session, make_clock);
    } else {
      // `identify` refused such a client before anything was read.
      (void)effective;
      (void)strategy;
      return run_error(
        run.base_, Exit::FAILED, "unsupported.capability", "the broker folder's client cannot be traded"
      );
    }
  }

private:
  std::optional<Connection> connection_;
};

/**
 * @brief Runs a program on a broker folder's account; see the file comment.
 * @tparam Program The program.
 * @tparam Symbols A callable from the run's arguments to its symbols.
 * @tparam Make A callable from the run's arguments and instruments to the program.
 * @param argc The count.
 * @param argv `run.id=ID`, then atlc's arguments.
 * @param symbols_of Gives the run's symbols.
 * @param make Builds the program.
 * @return The exit.
 */
template <typename Program, typename Symbols, typename Make>
int user_account_main(int argc, char** argv, Symbols symbols_of, Make make) {
  UserAccountBroker broker;
  return account_main<Program>(argc, argv, std::move(symbols_of), std::move(make), broker);
}

} // namespace driver
} // namespace cmdatl
