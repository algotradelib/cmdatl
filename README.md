# cmdatl

algotradelib from the command line.
With [algotradelib](https://github.com/algotradelib/algotradelib) alone you write your own C++ for a backtest or a trading loop; with cmdatl you write the strategy in [atls](https://github.com/algotradelib/atlc), describe the run in a TOML file, and backtest, tune and trade it from the command line.
The same commands let an AI agent read an account and place orders one at a time, inside limits the operator sets.

Status: in development, before its first release; commands and replies may change without notice.
This version answers `cmdatl env`, `cmdatl schema` and `--help`; every other command is listed and answers that it is not built yet.

```sh
cmdatl status                                          # the account in one read
cmdatl strategy backtest mean-reversion.toml -P lookback=30
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

Running strategies also needs `atlc`, the atls compiler, on `PATH` or named by `CMDATL_ATLC`, and CMake and a C++17 compiler at run time, because cmdatl compiles each strategy once and keeps the build.

## Output

- Text on a terminal, compact JSON otherwise; `--json` or `--format json|jsonl|text` chooses.
- `--fields a,b.c` keeps only those fields; `--value PATH` prints one value bare, the one reply without `env`.
- Every other reply carries `env`; a failure carries one `error` object with a stable code, such as `refused.position_limit`.
- A field name no reply of the command has is refused before the command runs; one missing from a reply the command already produced comes back with the whole reply and a `warning`.
- Exit codes: 0 ok, 1 failed and nothing sent, 2 usage, 3 refused (safe to retry), 4 unknown (reconcile before anything else), 5 strategy errors.
- `cmdatl schema` describes every command, its fields and its error codes.

## Acknowledgements

This project was developed with the assistance of generative AI tools.

## Disclaimer

cmdatl is released under the MIT license; see [LICENSE](LICENSE).
The software is provided as is, without warranty of any kind, and the authors are not liable for any loss, including trading losses, arising from its use.
Nothing in this repository is investment advice.
