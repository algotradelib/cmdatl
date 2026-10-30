# cmdatl

algotradelib from the command line.
With [algotradelib](https://github.com/algotradelib/algotradelib) alone you write your own C++ for a backtest or a trading loop; with cmdatl you write the strategy in [atls](https://github.com/algotradelib/atlc), describe the run in a TOML file, and backtest and trade it from the command line.
The same commands let an AI agent read an account and place orders one at a time, inside limits the operator sets.

Status: in development, before its first release; commands and replies may change without notice.
This version answers `cmdatl strategy new`, `cmdatl strategy backtest`, `cmdatl env`, `cmdatl schema` and `cmdatl help`; every other command is listed and answers that it is not built yet.

```sh
cmdatl strategy new dip --template mean-reversion      # dip.atls and dip.toml to start from
cmdatl strategy backtest dip.toml -P lookback=30       # atlc builds and replays it; the report as JSON
cmdatl status                                          # the account in one read
cmdatl run start mean-reversion.toml --mode paper      # a trading loop in the background
cmdatl run log --follow                                # watch it
cmdatl run stop                                        # and stop it
```

An agent plans an order, then places that plan:

```sh
cmdatl order plan 005930 --target 10 --limit 78200     # sends nothing; replies with a plan id
cmdatl order place p9cc4b94da4
cmdatl account --value cash                            # one value, nothing else
```

## Install

cmdatl is built with CMake and a C++17 compiler against an installed algotradelib; see [the library's install guide](https://github.com/algotradelib/algotradelib#install).

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/algotradelib/prefix
cmake --build build
```

Backtesting also needs `atlc`, the atls compiler, on `PATH` or named by `CMDATL_ATLC`, and CMake and a C++17 compiler at run time: `cmdatl strategy backtest` runs `atlc run`, which builds each strategy once against algotradelib and keeps the build.
`cmdatl strategy new` needs neither.

## Strategies

- `cmdatl strategy new NAME` writes `NAME.atls`, a starter program (`--template` `mean-reversion`, `crossover`, `momentum` or `breakout`), and `NAME.toml`, a run spec that reads `bars.csv` beside it (the columns `symbol,date,open,high,low,close,volume`).
  It never replaces a file unless `--force` is given, and then only regular files.
- `cmdatl strategy backtest SPEC` checks the spec with `atlc spec` before anything is built, then runs `atlc run`; the reply carries atlc's report unchanged under `report`, with the `-P` given and atlc's version.
  atlc's build progress and warnings arrive as they happen: on stderr, or as `progress` events with `--format jsonl`.
- `atlc check NAME.atls` checks a program alone; the language is atlc's ([its reference](https://github.com/algotradelib/atlc)).

## Output

- Text on a terminal, compact JSON otherwise; `--json` or `--format json|jsonl|text` chooses.
- `--fields a,b.c` keeps only those fields; `--value PATH` prints one value bare, the one reply without `env`.
- Every other reply carries `env`; a failure carries one `error` object with a stable code, such as `refused.position_limit`.
- A field name no reply of the command has is refused before the command runs; one missing from a reply the command already produced comes back with the whole reply and a `warning`.
- Exit codes: 0 ok, 1 failed and nothing sent, 2 usage, 3 refused (safe to retry), 4 unknown (reconcile before anything else), 5 strategy errors, 130 interrupted (Ctrl-C or SIGTERM stopped a command that sends nothing; it still replies).
- On a terminal a failure is written to stderr; in JSON it is the reply, on stdout.
- `cmdatl help COMMAND` (or `--help`) shows a command's usage and examples; `cmdatl schema` describes every command, its fields and its error codes for a program.

## Acknowledgements

This project was developed with the assistance of generative AI tools.

## Disclaimer

cmdatl is released under the MIT license; see [LICENSE](LICENSE).
The software is provided as is, without warranty of any kind, and the authors are not liable for any loss, including trading losses, arising from its use.
Nothing in this repository is investment advice.
