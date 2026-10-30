#include <cmdatl/cli.hpp>

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

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
  // cmdatl::run turns everything after a command starts into a reply or a minimal report with its exit code; an
  // exception reaching here was thrown before any command started, so nothing was sent.
  try {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return cmdatl::run(args, std::cout, std::cerr, stdout_is_terminal());
  } catch (const std::exception& e) {
    std::cerr << "cmdatl: internal error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "cmdatl: internal error\n";
  }
  return 1;
}
