// The backtest driver's entry: the program atlc generated, replayed by atlc's own runtime (`atls::run::main`), as
// `atlc run` replays it. `program.hpp` is written for each build and names the program's struct.

#include "program.hpp"

#include <atls/run.hpp>

/**
 * @brief Replays the program; see `atls::run::main`.
 * @param argc The count.
 * @param argv The settings, each `key=value`.
 * @return 0 after printing the report, 1 with an `error:` line.
 */
int main(int argc, char** argv) {
  return atls::run::main<Program>(argc, argv);
}
