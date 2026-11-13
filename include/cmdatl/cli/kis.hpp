#pragma once

// The command path on a KIS account: the logins read in this process, as `cmdatl run` reads them, and the two sessions
// a run builds (`account/kis.hpp`). Included by `main` alone, which hands `kis_command` to `cmdatl::run`, so no other
// translation unit compiles the KIS client.

#include "../account/kis.hpp"
#include "../auth/login.hpp"
#include "../strategy/runs.hpp"
#include "./broker_command.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

/** @brief KIS's setup for `broker_command_reply` (see `broker_command.hpp`). */
class KisSetup {
public:
  /**
   * @brief A setup reading the logins from a store.
   * @param store The credential store, borrowed; null for the system's, opened by `identify`.
   */
  explicit KisSetup(auth::CredentialStore* store = nullptr) : store_(store) {}

  /**
   * @brief Reads and checks the logins, then constructs the sessions, which sends nothing.
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
      auto refused =
        runs::read_run_logins(config, auth::lookup_broker("kis", ""), store_ ? store_ : system.get(), documents)
    ) {
      return refused;
    }
    algotradelib::kis::KisConfig trading;
    algotradelib::kis::KisAccount account;
    std::optional<algotradelib::kis::KisConfig> market;
    if (const auto mismatch = read_kis_logins(config, documents, trading, account, market, effective)) {
      return error_reply(Exit::FAILED, "auth.account_mismatch", *mismatch);
    }
    sessions_.emplace(std::move(trading), std::move(account), std::move(market));
    return std::nullopt;
  }

  /**
   * @brief The account's session.
   * @return It.
   */
  KisSessions::Session& trading() {
    return sessions_->trading();
  }

  /**
   * @brief The market-data session.
   * @return It.
   */
  KisSessions::Session& market() {
    return sessions_->market();
  }

  /**
   * @brief The system clock.
   * @return Now.
   */
  Clock::time_point now() const {
    return Clock::now();
  }

  /**
   * @brief Whether SIGINT, SIGTERM or SIGHUP arrived.
   * @return Whether one did.
   */
  bool stopped() const {
    return interrupted();
  }

private:
  auth::CredentialStore* store_;
  std::optional<KisSessions> sessions_;
};

/**
 * @brief An account command on a KIS account, with the system's credential store.
 * @param command The command.
 * @param invocation Its arguments.
 * @param snapshot The operator's file, read and `OK`, naming a KIS account.
 * @param output The output.
 * @return The reply.
 */
inline Reply kis_command(
  const Command& command, const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output
) {
  KisSetup setup;
  return broker_command_reply(command, invocation, *snapshot.config_, output, setup);
}

} // namespace cmdatl
