#pragma once

// The command path on a built-in broker's account: the logins read in this process, as `cmdatl run` reads them, and the
// two sessions a run builds (`account/builtin.hpp`). Included by `src/accounts.cpp` alone, which `main` hands to
// `cmdatl::run`, so no other translation unit of the binary compiles a broker's client.

#include "../account/builtin.hpp"
#include "../auth/login.hpp"
#include "../brokers/derive.hpp"
#include "../strategy/runs.hpp"
#include "./broker_command.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

/**
 * @brief A built-in broker's setup for `broker_command_reply` (see `broker_command.hpp`).
 * @tparam Client The broker's blocking client.
 */
template <typename Client>
class BuiltinSetup {
public:
  /**
   * @brief A setup reading the logins from a store.
   * @param store The credential store, borrowed; null for the system's, opened by `identify`.
   */
  explicit BuiltinSetup(auth::CredentialStore* store = nullptr) : store_(store) {}

  /**
   * @brief Reads and checks the logins, then constructs the sessions, which sends nothing; where the broker lists its
   *   accounts, confirms the operator file's number with one listing (`confirm_account`).
   * @param config The operator's file.
   * @param effective Its number set to the login's account.
   * @return The refusal, or nothing.
   */
  std::optional<Reply> identify(const OperatorConfig& config, OperatorConfig& effective) {
    std::unique_ptr<auth::CredentialStore> system;
    if (store_ == nullptr) {
      system = auth::open_store(auth::interaction_allowed(0));
    }
    std::vector<std::string> documents;
    if (
      auto refused = runs::read_run_logins(
        config, auth::lookup_broker(std::string(brokers::facts_of<Client>().word_), ""), store_ ? store_ : system.get(),
        documents
      )
    ) {
      return refused;
    }
    typename algotradelib::BrokerTraits<Client>::Config trading;
    typename algotradelib::BrokerTraits<Client>::Account account;
    std::optional<typename algotradelib::BrokerTraits<Client>::Config> market;
    if (const auto mismatch = read_builtin_logins<Client>(config, documents, trading, account, market, effective)) {
      return error_reply(Exit::FAILED, "auth.account_mismatch", *mismatch, ACCOUNT_MISMATCH_NEXT);
    }
    sessions_.emplace(std::move(trading), std::move(account), std::move(market));
    const auto check = confirm_account(sessions_->trading(), effective.number_);
    if (!check.confirmed_) {
      return check.mismatch_ ?
        error_reply(Exit::FAILED, "auth.account_mismatch", check.message_, ACCOUNT_MISMATCH_NEXT) :
        error_reply(Exit::FAILED, "account.unreadable", check.message_);
    }
    return std::nullopt;
  }

  /** @brief The account's session. */
  typename BuiltinSessions<Client>::Session& trading() {
    return sessions_->trading();
  }

  /** @brief The market-data session. */
  typename BuiltinSessions<Client>::Session& market() {
    return sessions_->market();
  }

  /** @brief The system clock. */
  Clock::time_point now() const {
    return Clock::now();
  }

  /** @brief Whether SIGINT, SIGTERM or SIGHUP arrived. */
  bool stopped() const {
    return interrupted();
  }

private:
  auth::CredentialStore* store_;
  std::optional<BuiltinSessions<Client>> sessions_;
};

/**
 * @brief An account command on a built-in broker's account, with the system's credential store.
 * @tparam Client The broker's blocking client.
 * @param command The command.
 * @param invocation Its arguments.
 * @param snapshot The operator's file, read and `OK`, naming the broker's account.
 * @param output The output.
 * @return The reply.
 */
template <typename Client>
Reply builtin_command(
  const Command& command, const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output
) {
  BuiltinSetup<Client> setup;
  return broker_command_reply(command, invocation, *snapshot.config_, output, setup);
}

} // namespace cmdatl
