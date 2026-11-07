// The backtest driver's entry: the program atlc generated, replayed by cmdatl's replay module (`cmdatl::replay::main`).
// `program.hpp` is written for each build: it names the program's struct and gives the fill model the run's market-price
// rule (`program_fills`). Built with a broker folder's adapter (`CMDATL_FETCH_ADAPTER`), it first fetches the bars
// through the adapter into the store, with the login `strategy backtest` hands it on descriptor 3
// (`cmdatl::replay::fetch_main`).

#include "program.hpp"

#if defined(CMDATL_FETCH_ADAPTER)
// A broker folder's adapter, copied into the build by cmdatl strategy backtest, before the fetch.
#include "adapter/adapter.hpp"

#include <cmdatl/replay/fetch.hpp>
#include <cmdatl/system/credential_pipe.hpp>
#endif

#include <cmdatl/replay/replay.hpp>

#include <optional>
#include <string>

/**
 * @brief Replays the program; see `cmdatl::replay::main` (and `cmdatl::replay::fetch_main` with an adapter).
 * @param argc The count.
 * @param argv The settings, each `key=value`.
 * @return 0 after printing the report, 1 with an `error:` line, 3 when a fetch failed.
 */
int main(int argc, char** argv) {
#if defined(CMDATL_FETCH_ADAPTER)
  // The login first, whatever happens next: the descriptor is closed once read; `strategy backtest` frames one.
  const auto payload = cmdatl::sys::read_staged_credentials();
  const auto documents = payload ? cmdatl::sys::unframe_credentials(*payload, 1) : std::nullopt;
  const auto login = documents ? std::optional<std::string>(documents->front()) : std::nullopt;
  return cmdatl::replay::fetch_main<Program>(
    argc, argv, program_fills,
    [](const std::string& document) {
      return cmdatl_adapter::connect(document);
    },
    login
  );
#else
  return cmdatl::replay::main<Program>(argc, argv, program_fills);
#endif
}
