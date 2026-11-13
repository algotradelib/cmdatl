#pragma once

// An account command on a broker's account, written once over a setup that reads the logins and owns the sessions
// (`kis.hpp` for KIS; the tests' stand-in): only the commands the table marks `on_broker()` answer (the reads,
// `order plan`, `order reconcile` and `order release`, none of which sends), through the account's desk over those
// sessions; every other account command is refused before any login is opened.
//
// A setup has:
// - `std::optional<Reply> identify(const OperatorConfig& config, OperatorConfig& effective)`: reads the logins, checks
//   them against the operator's file, sets `effective.number_` to the account they name (as a run does), and
//   constructs the sessions, which sends nothing;
// - `trading()`, `market()`: the sessions;
// - `Clock::time_point now()`: the live clock; the reply is as of its reading after the calendar;
// - `bool stopped()`: whether a stop was asked for, checked while waiting for the lock, before the calendar and each of
//   the reads' own broker reads (not the library's reads inside once()), and by the placement boundary.
//
// A call in flight is not interrupted.

#include "../account/broker_desk.hpp"
#include "./cli.hpp"

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace cmdatl {

/**
 * @brief An account command on a broker's account; see the file comment.
 * @tparam Setup The setup.
 * @param command The command.
 * @param invocation Its arguments.
 * @param config The operator's file.
 * @param output The output, carrying the account's environment.
 * @param setup The setup.
 * @return The reply.
 */
template <typename Setup>
Reply broker_command_reply(
  const Command& command, const Invocation& invocation, const OperatorConfig& config, Output& output, Setup& setup
) {
  if (!command.broker_) {
    return error_reply(
      Exit::FAILED, "unsupported.broker",
      "this command works on the mock account only in this version; on a " + config.broker_ +
        " account status, account, quote, order list, order plan, order reconcile and order release work, and "
        "cmdatl run trades it",
      "read the account with cmdatl status, or run a strategy with cmdatl run"
    );
  }
  auto effective = config;
  if (auto refused = setup.identify(config, effective)) {
    return std::move(*refused);
  }
  const auto stopped = [&setup] {
    return setup.stopped();
  };
  using Desk = BrokerDesk<std::decay_t<decltype(setup.trading())>, std::decay_t<decltype(setup.market())>>;
  auto opened = BasicAccount<Desk>::open_broker(
    effective, output, stopped, [&setup, &stopped](const OperatorConfig& file, const OwnerLock& lock) {
      return std::make_unique<Desk>(
        setup.trading(), setup.market(), file, lock,
        [&setup] {
          return setup.now();
        },
        stopped
      );
    }
  );
  if (opened.refusal_) {
    return std::move(*opened.refusal_);
  }
  auto& account = *opened.account_;
  auto& desk = account.desk();
  Reply reply;
  // A stop or a failed read is 130 or 1 until a command that sends or changes the journal starts; inside it, unknown
  // (4) with its hint, as on the mock account (`run_on_account`).
  const bool changes = command.may_send_ || command.mutates_;
  bool entered = false;
  try {
    desk.begin();
    account.view_at();
    // Only beneath this account's intact journal: a journal not established leaves every file as it was.
    if (account.journal_intact() && !write_owner_record(account.lock(), owner_what(command, invocation))) {
      return lock_lost_reply();
    }
    entered = true;
    reply = run_account_kind(command.account_, invocation, account, output);
  } catch (const Stopped&) {
    if (entered && changes) {
      throw;
    }
    return error_reply(
      Exit::INTERRUPTED, "interrupted", "interrupted by a signal; no further read was made", "run the command again",
      true
    );
  } catch (const Unreadable& error) {
    if (entered && changes) {
      throw;
    }
    return error_reply(Exit::FAILED, "account.unreadable", error.what(), "run the command again", true);
  }
  if (!account.lock().still_at_path()) {
    reply = lock_lost_reply();
  }
  return reply;
}

} // namespace cmdatl
