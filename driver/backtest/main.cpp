// The backtest driver's entry: the program atlc generated, replayed by cmdatl's replay module (`cmdatl::replay::main`).
// `program.hpp` is written for each build: it names the program's struct and gives the fill model the run's market-price
// rule (`program_fills`).

#include "program.hpp"

#include <cmdatl/replay/replay.hpp>

/**
 * @brief Replays the program; see `cmdatl::replay::main`.
 * @param argc The count.
 * @param argv The settings, each `key=value`.
 * @return 0 after printing the report, 1 with an `error:` line.
 */
int main(int argc, char** argv) {
  return cmdatl::replay::main<Program>(argc, argv, program_fills);
}
