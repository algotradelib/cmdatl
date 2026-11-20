// The backtest driver's entry: the program atlc generated, replayed by cmdatl's replay module (`cmdatl::replay::main`).
// `program.hpp` is written for each build: it names the program's struct and gives the fill model the run's market-price
// rule (`program_fills`). Built with a broker folder's adapter (`CMDATL_FETCH_ADAPTER`) or a built-in broker's client
// (`CMDATL_FETCH_BUILTIN`), it first fetches the bars into the store, with the login `strategy backtest` hands it on descriptor 3
// (`cmdatl::replay::fetch_main`).

#include "program.hpp"

#if defined(CMDATL_FETCH_ADAPTER)
// A broker folder's adapter, copied into the build by cmdatl strategy backtest, before the fetch.
#include "adapter/adapter.hpp"
#elif defined(CMDATL_FETCH_BUILTIN)
// The built-in broker whose history this driver fetches, written by cmdatl: `BuiltinClient` and its library header.
#include "broker.hpp"

#include <cmdatl/brokers/connect.hpp>

static_assert(
  cmdatl::brokers::facts_of<BuiltinClient>().name_ == CMDATL_DRIVER_BROKER,
  "the driver's broker.hpp names another broker than CMDATL_DRIVER_BROKER"
);
#endif
#if defined(CMDATL_FETCH_ADAPTER) || defined(CMDATL_FETCH_BUILTIN)
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
#if defined(CMDATL_FETCH_ADAPTER) || defined(CMDATL_FETCH_BUILTIN)
  // The login first, whatever happens next: the descriptor is closed once read; `strategy backtest` frames one.
  const auto payload = cmdatl::sys::read_staged_credentials();
  const auto documents = payload ? cmdatl::sys::unframe_credentials(*payload, 1) : std::nullopt;
  const auto login = documents ? std::optional<std::string>(documents->front()) : std::nullopt;
  return cmdatl::replay::fetch_main<Program>(
    argc, argv, program_fills, program_measures,
    [](const std::string& document, const std::string& options) {
#if defined(CMDATL_FETCH_ADAPTER)
      return cmdatl_adapter::connect(document, options);
#else
      return cmdatl::brokers::connect_bars<BuiltinClient>(document, options);
#endif
    },
    login
  );
#else
  return cmdatl::replay::main<Program>(argc, argv, program_fills, program_measures);
#endif
}
