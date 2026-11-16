# Your own replay

cmdatl installs the replay its backtests run as headers, `include/cmdatl/replay/` under its prefix, for a program of your own around `atlc build`'s output:

```cpp
// atlc build dip.atls -o strategy.hpp --emit-runtime .
#include "strategy.hpp"

#include <cmdatl/replay/replay.hpp>

int main(int argc, char** argv) {
  return cmdatl::replay::main<atls::Dip>(argc, argv);
}
```

- It reads the settings as `key=value` arguments, as a backtest passes them to the driver it builds: the run spec's keys, with `param.NAME` and `instrument.NAME` for `params.NAME` and `instruments.NAME`, numbers as plain decimals, dates as yyyymmdd, and `output.fills=FILE` and `output.equity=FILE` for `--fills` and `--equity`.
- It prints the report on stdout, or one `error:` line on stderr and exits 1.
- With `evaluate.method=grid` it evaluates many replays as `cmdatl strategy evaluate` does (`docs/run-spec.md`, "Many replays"): `grid.NAME=V1,V2,...` per parameter (values JSON numbers, names in any order), `evaluate.jobs=N`, `evaluate.objective=FIGURE` and `evaluate.direction=max|min`; it prints one JSON line per replay and a `{"summary":{...}}` line, or a `{"stopped":{...}}` line and an `error:` line on stderr, exit 1.
- It includes algotradelib and nothing else of cmdatl: compile with `-I<prefix>/include` and `-I` the directory given to `--emit-runtime` (here `.`, for the `atls/` headers the strategy includes), beside algotradelib's own flags, and with `-ffp-contract=off` on GCC for cmdatl's numbers; link threads (`-pthread`, or CMake's `Threads::Threads`), which an evaluation's `evaluate.jobs` uses.
- To read a Parquet store, define `CMDATL_DUCKDB` and link DuckDB's C library.
- It reads `replay.fill=decision-close` as a run spec's `fill` under `[replay]`.
  To price market orders by your own rule, pass a third argument that returns the fill model to use, as a run spec's `fill_header` does: `cmdatl::replay::main<atls::Dip>(argc, argv, [](auto model) { return model.with_market_price("typical_price", typical_price); })`.
- A fourth argument adds measures to the report, as a run spec's `measures` does: a callable returning pairs of a name and a function `double(const algotradelib::backtest::MeasureInput&)`, such as `[]() { return std::vector<std::pair<const char*, double (*)(const algotradelib::backtest::MeasureInput&)>>{{"final_level", final_level}}; }`.
