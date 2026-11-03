# cmdatl

<!-- badges: start -->
[![cmake-check](https://github.com/algotradelib/cmdatl/actions/workflows/cmake-check.yaml/badge.svg?branch=main)](https://github.com/algotradelib/cmdatl/actions/workflows/cmake-check.yaml?query=branch%3Amain)
[![codecov](https://codecov.io/gh/algotradelib/cmdatl/graph/badge.svg)](https://codecov.io/gh/algotradelib/cmdatl)
<!-- badges: end -->

algotradelib from the command line.
With [algotradelib](https://github.com/algotradelib/algotradelib) alone you write your own C++ for a backtest or a trading loop; with cmdatl you write the strategy in [atls](https://github.com/algotradelib/atlc), describe the run in a TOML file, and backtest it or run it from the command line.
The same commands let an AI agent read an account and place orders one at a time, inside limits the operator sets.
In this version the account is a paper account: a market simulated from stored daily bars, with no broker and no network.

From a clone, with cmdatl and atlc installed (below):

```sh
cmdatl strategy backtest examples/mean-reversion.toml params.lookback=30 # builds and replays it; the report as JSON
export CMDATL_CONFIG=$PWD/examples/paper.toml # the example paper account: the operator's file
touch examples/paper.armed # the operator arms the account; without this file nothing may be placed
cmdatl status # the account in one read
cmdatl order plan AAA --target 10 --limit 102 # sends nothing; replies with a plan id
cmdatl order place p8c16a0bcd2 # the plan id the reply named
cmdatl clock advance --minutes 30 # the paper clock moves only when told; the order fills
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

Backtests and runs compile each strategy at run time, so they also need CMake and a C++17 compiler, the algotradelib cmdatl was built against, and `atlc`, the atls compiler, on `PATH` or named by `CMDATL_ATLC` ([its install](https://github.com/algotradelib/atlc#install): `cargo install --git https://github.com/algotradelib/atlc`, with a Rust toolchain).
Those builds find algotradelib where cmdatl was built against it, but not through `CMAKE_PREFIX_PATH`: of the variables that locate packages they pass on only `OPENSSL_ROOT_DIR`, `BOOST_ROOT`, `Boost_ROOT` and `Eigen3_ROOT` (and `DUCKDB_ROOT` for a store).
So keep `OPENSSL_ROOT_DIR` exported on macOS for every backtest and run as for the build above, and export the others when Boost or Eigen live outside the library's prefix and the system's paths.
atlc only translates a program to C++; cmdatl builds that once per program, keeps the build, and replays it.
A backtest that reads a Parquet store also needs DuckDB's C library, named by the `[build]` table's `duckdb` or by `DUCKDB_ROOT`.
`cmdatl strategy reference` needs atlc only; `cmdatl strategy new` needs neither.

## Strategies

- `cmdatl strategy new NAME` writes `NAME.atls`, a starter program (`--template` `mean-reversion`, `crossover`, `momentum` or `breakout`), and `NAME.toml`, a run spec that reads `bars.csv` beside it (the columns `symbol,date,open,high,low,close,volume`).
  It never replaces a file unless `--force` is given, and then only regular files.
- `cmdatl strategy backtest TARGET [KEY=VALUE]...` replays a program (`.atls`), a run spec, or both; `KEY=VALUE` words are settings (`params.lookback=30`, `replay.cash=1e8`, `data.file=bars.csv`) and win over the spec's.
  A run spec given with a program applies to it, so one spec can serve several programs (`cmdatl strategy backtest dip.atls daily.toml`); given alone, it names its program with `strategy`.
  Every setting is checked against the program before anything is built; the reply carries the replay's report unchanged under `report`, with the settings given and atlc's version.
  `cmdatl strategy reference run-spec` lists every setting.
  The build's progress and atlc's warnings arrive as they happen: on stderr, or as `progress` events with `--format jsonl`.
- `atlc check NAME.atls` checks a program alone; the language is atlc's: `cmdatl strategy reference` prints its reference's index and `cmdatl strategy reference builtins` (or another chapter) a chapter, from the atlc cmdatl runs ([online](https://github.com/algotradelib/atlc/tree/main/docs/reference)).

## Examples

`examples/` holds five atls programs, each with a run spec over the synthetic daily bars in `examples/data/daily.csv`; those specs are backtests (market orders and no market), which `cmdatl run` refuses.
`examples/run.toml` runs `mean-reversion` as a run (a market and limit orders), and `examples/paper.toml` is a paper account over the same bars: the commands at the top use both.

```sh
cmdatl strategy backtest examples/mean-reversion.toml params.lookback=30
```

- `mean-reversion` buys when the price falls well below its recent mean and sells when it comes back.
- `pairs` trades the spread of two instruments around its hedge ratio, short sales included.
- `trailing-stop` buys a breakout and sells a fraction below the highest price since the fill; it uses `on_fill`, which a run refuses.
- `universe` trades every symbol of the file at the open and the close, and leaves a missing bar out.
- `user-indicator` is mean reversion on a band written as a user indicator, with a function that caps the position.

## The paper account

A paper account trades stored daily bars, not a broker: nothing reaches a network or a real account.
The operator describes it in a TOML file named by `CMDATL_CONFIG`, never by an argument; one account per file.

```toml
atlc = "/opt/atlc/bin/atlc" # optional: atlc, between CMDATL_ATLC and PATH

[account]
broker = "paper" # the only broker of this version
number = "paper-0001" # names the account's state directory

[limits]
symbols = ["005930", "000660"]
position = { "005930" = 100, "000660" = 20 }
order_value = 5000000 # KRW, one order
daily_orders = 3 # placements per KST date
collar = 0.05 # optional, at most 0.30
phases = ["open"] # optional: pre_open, open, post_close

[paper]
bars = "bars.csv" # the columns symbol,date,open,high,low,close,volume in any order, beside this file
cash = 100000000
start = "2024-03-25T09:30" # KST, at or after the first date's close
```

- Every key is checked: an unknown key, a value of another type or out of range is refused with the file, the line and the column (`config.invalid`); `cmdatl env` shows what was read.
- Optional keys, with their defaults:
  - `atlc`: none (`CMDATL_ATLC`, then `PATH`).
  - `account.env`: `"paper"`, the only value with the paper broker.
  - `limits.order_type`: `"limit"`, the only order type.
  - `limits.collar`: 0.05, how far a limit may sit from the last price, as a fraction (above 0, at most 0.30).
  - `limits.phases`: `["open"]`.
  - `limits.plan_ttl`: 60, the seconds a plan id stays good (1 to 3600).
  - `limits.portfolio_age`: 30, how old, in seconds, holdings and the order list may be when an order is decided (1 to 86400).
  - `paper.listing_lag`, `paper.holdings_lag`: 0, the seconds an accepted order stays out of the order list and a fill out of holdings, as a broker's may (up to 86400).
- The account's state is tied to the bars file's bytes, `cash`, `start` and the two lags: changing any of them on a used account is refused (exit 4, not this account's).
  To start an account again, move its state directory aside.
- The armed file is `<stem>.armed` beside the operator's file (`paper.toml` gives `paper.armed`); while it is missing nothing may be placed, and `orders_accepted_now` says so.
- The account's state lives in `$XDG_STATE_HOME/cmdatl/accounts/<broker>-<env>-<number>` (else `~/.local/state/cmdatl`): its lock, its order journal and `paper.jsonl`, the paper market's log.
  Another `XDG_STATE_HOME` is another paper account.
- Keep the operator's file out of the state directory.
  cmdatl applies the operator's file to every command, but the agent runs as the operator's own user: cmdatl cannot stop a process of that user from changing the operator's file, the armed file, the release approval or the account's state directory.
- The commands in this section and the next use the sample file's symbols; on the example account, use `AAA` and `BBB`.

```sh
cmdatl status # time, phase, readiness, positions, cash, owed and working orders, quotes
cmdatl quote 005930 000660 # last price, band, and whether an order would pass the first gates now
cmdatl account --symbol 005930 # positions, cash, buying power, owed orders, orders left today
cmdatl order list # working orders
cmdatl clock advance --next-open # the paper clock moves only when told; resting orders fill or expire
cmdatl clock advance --minutes 30
```

- `orders_accepted_now` is true exactly when these preliminary gates pass now: the armed file exists, the day's count is known and not used up, the phase is one the operator allows, and the market takes orders for the symbol (08:30-15:30 KST on a date it trades).
  `not_accepted` names the gates that fail.
  A placement still makes its own price, position, value, buying-power and owed-order checks, and may refuse.
- Exit 4 from `cmdatl clock advance` means the advance may have happened: read `cmdatl clock` before advancing again.
- The lock: two cmdatl commands never hold an account at once, provided nobody removes or replaces its `lock` file; a command waits for the account at most 10 s, saying who holds it, then answers `busy.locked`.
- Durability: an input the paper log accepted survives a killed process on local storage; nothing more is promised (not a power loss on macOS, not a network filesystem).
- A journal or a paper log that is missing, damaged or not this account's is exit 4, with what to do in `error.next`, and nothing is written.
  Only a torn last line of an otherwise sound paper log is cut: an input a killed process did not finish writing.

## Orders

An agent moves a position to a target in two steps, a plan and its placement, and nothing places or cancels except through them.

```sh
cmdatl order plan 005930 --target 10 --limit 78200 # what would be sent; a plan id
cmdatl order place p9cc4b94da4 # sends that plan, if nothing moved since
cmdatl order cancel 005930 # withdraws every working order on 005930; never places
cmdatl order reconcile # records fills from holdings for every owed order; sends nothing
cmdatl order reconcile 005930
cmdatl order release 005930 --reason "read the order list and holdings"
```

- A target is a position, not a quantity: `--target 0` sells out, and a target the holding and the working orders already meet sends nothing (`met`).
- Every limit comes from the operator's file at the moment of sending; no option of an order command names a limit, phase, collar or order type.
  Every placement is a limit order inside `limits.collar` of the last price and on the tick grid.
- A plan id is bound to what the plan saw: the account, the request, the holding and working orders of the symbol, the cash and the journal's size.
  It is used once, it lapses after `limits.plan_ttl` simulated seconds (60 by default), and anything moving makes it stale (`refused.stale_plan`): plan again.
- Each order command takes one simulated second, as a broker call takes time; `events` is what the market did in that second, which can fill or expire an order already resting, even on a plan or a refusal.
- Exit 3 means nothing reached the market in this call: plan again only once its reason has passed (a lapsed plan, a closed market); a limit the operator set is not retried around.
  Exit 4 means an order may have gone out, or the journal cannot say: run `cmdatl order reconcile SYMBOL` before anything else, and read `error.next`.
  A request the market itself refused is exit 4 `unknown.broker_refused`, since it reached the market; `cmdatl order reconcile SYMBOL` then shows nothing owed.
- No reply at all (a killed process): treat it as exit 4.
- `cmdatl order cancel SYMBOL` withdraws by symbol, needs no armed file and runs in any phase; `withdrawal_sent` means sent, not confirmed, and `cmdatl order list` shows when the orders are gone.
- `cmdatl order reconcile` reports each symbol as `clean`, `working` (an order rests), `holding_moved` (holdings changed between two reads) or a refusal's reason; anything still owed afterwards is exit 3 `refused.awaiting`.
- `owed` rows say what to do next; reading them never settles them, only an order command writes the journal.
- An order the broker never named stays owed until the listing settles it or the operator releases it.
  `cmdatl order release SYMBOL --reason TEXT` needs the armed file and the operator's approval `<stem>.release` beside the operator's file (`paper.toml` gives `paper.release`): one line per owed order, `SYMBOL JOURNAL_ID` with one space, naming exactly the journal ids `cmdatl account` shows as owed on that symbol.
  The approval is renamed after one use.
- A damaged journal has no cmdatl remedy: on paper, the operator keeps the account's directory for inspection and moves it aside, which abandons that paper market with it.

## Runs

A run trades a strategy through algotradelib's live runner, the code that would trade a broker's account, on a mock market built from the run spec's data.

```sh
cmdatl run dip.toml # builds the program once, then runs it in the foreground: events as JSON lines, the reply last
cmdatl run dip.toml params.lookback=30 > run.jsonl # the same, into a file
```

- A run's market is the library's mock broker, inside the run's own process: each run is a fresh market with its own journal, and nothing is shared with the paper account above or with another run.
  The agent commands do not see a run's orders, and a run's holdings at its end are a result, not an account anything can trade.
- The run's settings describe the market, read exactly as `cmdatl strategy backtest` reads them: the bars, `[replay] cash`, the costs, the instruments.
  The operator's file describes what may be placed: `limits.symbols`, the position caps (a run with several symbols gets the smallest of their caps), `order_value`, `daily_orders` (per run, per 24 hours of market time), `order_type`, `collar` and `phases`; no setting loosens them.
- A run needs `[replay] market = "kr"`, daily bars from a file (not a store), `[orders] type = "limit"` with an `offset` within `limits.collar`, symbols in `limits.symbols`, and the armed file: each missing one is refused before anything is built.
  It also needs a program without `on_fill`, a warm-up of at most 10,000 bars, and daily bars: those are refused by the run itself, after the program is built and before anything is placed.
  A program with session legs (`on_open`, `on_close`) is refused too, whatever the fill setting says: `run` learns it from `atlc check --info` and answers `program.live_unsupported` before anything is built.
  Every refusal says what to do in `error.next`.
- Every placement is checked once more as the last step before the market: the armed file, a limit inside the collar of the last price, and a permitted phase, closing orders included.
  Daily bars from date-only rows end at 09:00 KST, in `open`; bars stamped at the 15:30 close fall in `post_close`, so a run of those trades only when the operator permits `post_close`.
- The run starts at the first bar and steps through the data 15 market minutes at a time, as fast as the machine allows; at the end it withdraws whatever rests and confirms it before it reports.
  A run's fills are those of `cmdatl strategy backtest` on the same spec when the program decides from bars alone, the bars end on that step (date-only rows do), and none of the operator's limits, the cash or the order budget refuses an order the backtest makes, since the backtest applies none of them.
- The output is JSON lines whatever the output format: events (each with `event`: `start`, `placed`, `filled`, `canceled`, `expired`, `refused`, `stop`, `clock`, `end`), then the reply as the last line, and the command exits with the run's exit.
  The reply's `refused` counts orders refused by the operator's limits or the market (`boundary`), by the library's guard (`guard`) and targets atlc could not price (`pricing`); a run with any is `outcome: refused_orders`.
  A run whose outcome is unknown (a journal write failed, an order it could not confirm) exits 4; its market ended with its process and no account was affected.
- Ctrl-C, SIGTERM and SIGHUP (a closed terminal) withdraw the run: no new placements, its resting orders cancelled and confirmed, then the reply; so does a closed output, and a stopped run whose output nobody reads gives up writing after a second.
  A run stopped this way exits 0 with `stopped: signal`; 130 is for a command stopped before anything could be sent.
  Placements refused once a stop is noted are counted under `signal` in the reply's `refused` and leave the run clean.
  Removing the armed file stops every run's placements at its next step.
  For a run that outlives its terminal, start it with `nohup` and redirect its output: a SIGHUP ignored when cmdatl starts stays ignored.
  `--fields` and `--value` do not apply to `run`, which prints every event and its whole reply.
- A run needs CMake and a C++17 compiler: `run` builds a program once per shape under `$XDG_CACHE_HOME/cmdatl/drivers` (else `~/.cache/cmdatl/drivers`); a later run of an unchanged program builds nothing, and a change to the program, atlc or algotradelib builds again.

## Output

- Text on a terminal, compact JSON otherwise; `--json` or `--format json|jsonl|text` chooses.
- `--fields a,b.c` keeps only those fields; `--value PATH` prints one value bare, the one reply without `env`.
- Every other reply carries `env`; a failure carries one `error` object with a stable code, such as `refused.position_limit`.
- A field name no reply of the command has is refused before the command runs; one missing from a reply the command already produced comes back with the whole reply and a `warning`.
- Exit codes: 0 ok, 1 failed and nothing sent, 2 usage, 3 refused (nothing was sent), 4 unknown (follow error.next before anything else), 5 strategy errors, 130 interrupted (Ctrl-C, SIGTERM or SIGHUP stopped a command before anything could be sent; it still replies, and once an order command has begun to send it answers with its own outcome instead).
- On a terminal a failure is written to stderr; in JSON it is the reply, on stdout.
- `cmdatl help COMMAND` (or `--help`) shows a command's usage and examples; `cmdatl schema` describes every command, its fields and its error codes for a program.

## Your own replay

cmdatl installs the replay its backtests run as headers, `include/cmdatl/replay/` under its prefix, for a program of your own around `atlc build`'s output:

```cpp
// atlc build dip.atls -o strategy.hpp --emit-runtime .
#include "strategy.hpp"

#include <cmdatl/replay/replay.hpp>

int main(int argc, char** argv) {
  return cmdatl::replay::main<atls::Dip>(argc, argv);
}
```

- It reads the settings as `key=value` arguments, as a backtest passes them to the driver it builds: the run spec's keys, with `param.NAME` and `instrument.NAME` for `params.NAME` and `instruments.NAME`, numbers as plain decimals, dates as yyyymmdd, and `output.fills=FILE` for `--fills`.
- It prints the report on stdout, or one `error:` line on stderr and exits 1.
- It includes algotradelib and nothing else of cmdatl: compile with `-I<prefix>/include` and `-I` the directory given to `--emit-runtime` (here `.`, for the `atls/` headers the strategy includes), beside algotradelib's own flags, and with `-ffp-contract=off` on GCC for cmdatl's numbers.
- To read a Parquet store, define `CMDATL_DUCKDB` and link DuckDB's C library.
- To price market orders by your own rule, pass a third argument that returns the fill model to use, as a run spec's `fill_header` under `[replay]` does: `cmdatl::replay::main<atls::Dip>(argc, argv, [](auto model) { return model.with_market_price("decision_close", decision_close); })`.

## Guides and skills

Two guides teach cmdatl to an AI agent: the agent guide (reading the account, the order rails, exit codes, runs) and the strategy guide (atls, run specs, backtests, runs).

```sh
cmdatl guide agent # the guide of this binary
cmdatl guide strategy --format text # bare text, also through a pipe
```

- They are the same text as the Agent Skills in `skills/` (`cmdatl-agent` and `cmdatl-strategy`), built into the binary from those files.
- The repository is a Claude Code plugin: `claude --plugin-dir /path/to/cmdatl` loads both skills from a clone.
- For tools that read a project's `.agents/skills/`, copy the two folders from `skills/`, or from `share/cmdatl/skills` where cmdatl is installed.

## Acknowledgements

This project was developed with the assistance of generative AI tools.

## Disclaimer

cmdatl is released under the MIT license; see [LICENSE](LICENSE).
The software is provided as is, without warranty of any kind, and the authors are not liable for any loss, including trading losses, arising from its use.
Nothing in this repository is investment advice.
