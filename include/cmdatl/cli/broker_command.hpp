#pragma once

// An account command on a broker's account, written once over a setup that reads the logins and owns the sessions
// (`kis.hpp` for KIS; the tests' stand-in): only the four reads answer, through a desk over those sessions; every other
// account command is refused before any login is opened. Nothing here places, cancels or changes an order.
//
// A setup has:
// - `std::optional<Reply> identify(const OperatorConfig& config, OperatorConfig& effective)`: reads the logins, checks
//   them against the operator's file, sets `effective.number_` to the account they name (as a run does), and
//   constructs the sessions, which sends nothing;
// - `trading()`, `market()`: the sessions;
// - `Clock::time_point now()`: the clock the reply is as of;
// - `bool stopped()`: whether a stop was asked for, checked while waiting for the lock and before every broker call.
//
// A call in flight is not interrupted: the next one is not made.

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
  if (command.broker_read_ == BrokerRead::NONE) {
    return error_reply(
      Exit::FAILED, "unsupported.broker",
      "this command trades the mock broker only in this version; on a " + config.broker_ +
        " account status, account, quote and order list read it, and cmdatl run trades it",
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
    effective, output, stopped, [&setup, &stopped](const OperatorConfig&, const OwnerLock&) {
      return std::make_unique<Desk>(
        setup.trading(), setup.market(),
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
  try {
    desk.begin();
    account.view_at();
    // Only beneath this account's intact journal: a journal not established leaves every file as it was.
    if (account.journal_intact() && !write_owner_record(account.lock(), owner_what(command, invocation))) {
      return lock_lost_reply();
    }
    switch (command.broker_read_) {
      case BrokerRead::STATUS:
        reply = status_reply(invocation, account, desk, output);
        break;
      case BrokerRead::ACCOUNT:
        reply = account_reply(invocation, account, desk, output);
        break;
      case BrokerRead::QUOTE:
        reply = quote_reply(invocation, account, desk, output);
        break;
      case BrokerRead::ORDER_LIST:
        reply = order_list_reply(invocation, account, desk, output);
        break;
      case BrokerRead::NONE:
        // Refused above.
        break;
    }
  } catch (const Stopped&) {
    return error_reply(
      Exit::INTERRUPTED, "interrupted", "interrupted by a signal; no further read was made", "run the command again",
      true
    );
  } catch (const Unreadable& error) {
    return error_reply(Exit::FAILED, "account.unreadable", error.what(), "run the command again", true);
  }
  if (!account.lock().still_at_path()) {
    reply = lock_lost_reply();
  }
  return reply;
}

} // namespace cmdatl
