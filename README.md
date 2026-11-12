# cmdatl

<!-- badges: start -->
[![cmake-check](https://github.com/algotradelib/cmdatl/actions/workflows/cmake-check.yaml/badge.svg?branch=main)](https://github.com/algotradelib/cmdatl/actions/workflows/cmake-check.yaml?query=branch%3Amain)
[![codecov](https://codecov.io/gh/algotradelib/cmdatl/graph/badge.svg)](https://codecov.io/gh/algotradelib/cmdatl)
<!-- badges: end -->

<!-- multilingual: start -->
<p align="left">
  <b>English</b> &middot;
  <a href="README.ko-kr.md">한국어</a>
</p>
<!-- multilingual: end -->

algotradelib from the command line.
With [algotradelib](https://github.com/algotradelib/algotradelib) alone you write your own C++ for a backtest or a trading loop; with cmdatl you write the strategy in [atls](https://github.com/algotradelib/atlc), describe the run in a TOML file, and backtest it or run it from the command line.
The same commands let an AI agent read an account and place orders one at a time, inside limits the operator sets.
In this version the account and order commands work on a mock account: a market simulated from stored daily bars and kept in a state file, with no broker and no network.
`cmdatl run` also trades a KIS account the operator's file names, a demo account (모의투자) unless the file says otherwise; see [Runs](docs/runs.md).
Broker logins are kept in the macOS keychain, so a run on a broker's account, or a backtest that fetches its bars through a broker folder, needs macOS; other backtests, the mock account and runs on a mock market work on Linux too.

From a clone, with cmdatl and atlc installed (below):

```sh
cmdatl strategy backtest examples/mean-reversion.toml params.lookback=30 # builds and replays it; the report as JSON
export CMDATL_CONFIG=$PWD/examples/mock.toml # the example mock account: the operator's file
touch examples/mock.armed # the operator arms the account; without this file nothing may be placed
cmdatl status # the account in one read
cmdatl order plan AAA --target 10 --limit 102 # sends nothing; replies with a plan id
cmdatl order place p8c16a0bcd2 # the plan id the reply named
cmdatl clock advance --to 2024-01-04T15:30 # the mock clock moves only when told; the order fills against the next day's bar, at its close
cmdatl order reconcile AAA # records the fill from holdings; sends nothing
cmdatl run examples/run.toml # a strategy run on its own mock market: its events, then its reply
```

`cmdatl strategy new dip --template mean-reversion` starts a program of your own: `dip.atls` and `dip.toml`, a run spec that reads `bars.csv` beside it (`cp examples/data/daily.csv bars.csv` gives it the examples' bars).

How cmdatl's parts use algotradelib's modules and atlc:

![cmdatl and algotradelib](docs/architecture.png)

## Install

cmdatl needs a C++17 compiler, CMake 3.20 or later, and algotradelib [as an installed package](https://github.com/algotradelib/algotradelib/blob/main/doxygen/index.md#as-an-installed-package), with the Boost 1.87 and OpenSSL 3.0 it was built with installed too.
It is built and tested on Linux and macOS; Windows is not supported yet.

```sh
export OPENSSL_ROOT_DIR=$(brew --prefix openssl@3) # macOS only: Homebrew's OpenSSL 3 is not on the default paths
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/algotradelib/prefix
cmake --build build
cmake --install build --prefix /your/prefix # bin/cmdatl, include/cmdatl/replay/, share/cmdatl/skills/
```

CLI11, toml++ and Catch2 are downloaded when they are not installed (`CMDATL_FETCH_DEPS`, on by default); `-DCMDATL_BUILD_TESTS=OFF` leaves the tests out.
Backtests and runs also need CMake, a C++17 compiler and atlc at run time: see [What backtests and runs need](docs/install.md).

## Documentation

- [What backtests and runs need](docs/install.md): atlc, CMake and the variables a strategy's build passes on.
- [Strategies](docs/strategies.md): new programs, backtests and the examples.
- [Run settings](docs/run-spec.md): every setting of a run spec, also `cmdatl strategy reference run-spec`.
- [The mock account](docs/mock-account.md): the operator's file, the armed file, the account's state, and when orders fill.
- [Orders](docs/orders.md): plans, placements, cancels, reconciling and releasing.
- [Runs](docs/runs.md): a strategy on the live runner, on a mock market or a KIS account.
- [Broker logins](docs/logins.md): `cmdatl auth`.
- [Your own broker or data vendor](docs/brokers.md): broker folders.
- [Output](docs/output.md): formats, fields and exit codes.
- [Your own replay](docs/replay.md): the installed replay headers.
- [Guides and skills](docs/guides.md): the agent and strategy guides.

## Acknowledgements

This project was developed with the assistance of generative AI tools.

## Disclaimer

cmdatl is released under the MIT license; see [LICENSE](LICENSE).
The software is provided as is, without warranty of any kind, and the authors are not liable for any loss, including trading losses, arising from its use.
Nothing in this repository is investment advice.
