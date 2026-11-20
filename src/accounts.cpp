// The account path on a built-in broker, handed to `cmdatl::run` by `main`: the user's word mapped to the library's
// client at compile time (`with_broker`). Code is compiled only for the brokers cmdatl permits as accounts
// (`builtin::Facts::account_`); every other broker compiles to a refusal, which nothing reaches: the operator's file
// refuses such a broker first, and `cli.hpp` calls this only for a word with `account_`.

#include <algotradelib/builtin_brokers>
#include <cmdatl/cli/builtin.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace cmdatl {

/**
 * @brief An account command on a built-in broker's account, with the system's credential store.
 * @param command The command.
 * @param invocation Its arguments.
 * @param snapshot The operator's file, read and `OK`, naming a built-in broker's account.
 * @param output The output.
 * @return The reply.
 */
Reply builtin_account_command(
  const Command& command, const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output
) {
  const auto* facts = builtin::from_word(snapshot.config_->broker_);
  auto reply = algotradelib::with_broker<algotradelib::builtin_brokers>(
    facts != nullptr ? facts->name_ : std::string_view(), [&](auto tag) -> Reply {
      using Client = typename decltype(tag)::type;
      if constexpr (brokers::facts_of<Client>().account_) {
        return builtin_command<Client>(command, invocation, snapshot, output);
      } else {
        return error_reply(Exit::FAILED, "unsupported.broker", "the broker is not supported in this version");
      }
    }
  );
  return std::move(reply.value());
}

} // namespace cmdatl
