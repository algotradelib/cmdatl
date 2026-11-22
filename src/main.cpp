#include <cmdatl/cmdatl>

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace cmdatl {
// The account path on a built-in broker: `src/accounts.cpp`, the one unit of the binary that compiles a broker's client.
Reply builtin_account_command(const Command&, const Invocation&, const ConfigSnapshot&, Output&);
std::string builtin_discover_account(std::string_view word, const std::string& login);
} // namespace cmdatl

/**
 * @brief Whether stdout is a terminal.
 * @return Whether it is.
 */
static bool stdout_is_terminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stdout)) != 0;
#else
  return ::isatty(STDOUT_FILENO) != 0;
#endif
}

/**
 * @brief cmdatl's entry point.
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return The exit code.
 */
int main(int argc, char** argv) {
  // SIGINT and SIGTERM stop a command's child and still give a reply; a closed output fails through its stream.
  cmdatl::handle_signals();
  // cmdatl::run turns everything after a command starts into a reply or a minimal report with its exit code; an
  // exception reaching here was thrown before any command started, so nothing was sent.
  try {
    const std::vector<std::string> args(argv + 1, argv + argc);
    // auth login asks a broker that lists its accounts which account its keys trade, through the same unit.
    cmdatl::auth::account_discoverer() = cmdatl::builtin_discover_account;
    // The commands on a built-in broker's account are compiled in `src/accounts.cpp` alone.
    return cmdatl::run(args, std::cout, std::cerr, stdout_is_terminal(), cmdatl::builtin_account_command);
  } catch (const std::exception& e) {
    std::cerr << "cmdatl: internal error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "cmdatl: internal error\n";
  }
  return 1;
}
