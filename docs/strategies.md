# Strategies

- `cmdatl strategy new NAME` writes `NAME.atls`, a starter program (`--template` `mean-reversion`, `crossover`, `momentum` or `breakout`), and `NAME.toml`, a run spec that reads `bars.csv` beside it (the columns `symbol,date,open,high,low,close,volume`).
  It never replaces a file unless `--force` is given, and then only regular files.
- `cmdatl strategy backtest TARGET [KEY=VALUE]...` replays a program (`.atls`), a run spec, or both; `KEY=VALUE` words are settings (`params.lookback=30`, `replay.cash=1e8`, `bars.file=bars.csv`) and win over the spec's.
  A run spec given with a program applies to it, so one spec can serve several programs (`cmdatl strategy backtest dip.atls daily.toml`); given alone, it names its program with `strategy`.
  Every setting is checked against the program before anything is built; the reply carries the replay's report unchanged under `report`, with the settings given and atlc's version.
  `cmdatl strategy reference run-spec` lists every setting.
  The build's progress and atlc's warnings arrive as they happen: on stderr, or as `progress` events with `--format jsonl`.
- `atlc check NAME.atls` checks a program alone; the language is atlc's: `cmdatl strategy reference` prints its reference's index and `cmdatl strategy reference builtins` (or another chapter) a chapter, from the atlc cmdatl runs ([online](https://github.com/algotradelib/atlc/tree/main/docs/reference)).

## C++ of your own

A program may call C++ you write: a function declared in `functions {}` with `extern` and no body, or an indicator declared with `extern indicator` ([atlc's reference](https://github.com/algotradelib/atlc/blob/main/docs/reference/functions-and-indicators.md#external-c), "External C++").

```
functions {
  extern real clip(real z, real cap);
}
extern indicator mean_of(real x, int n) {
  warmup n;
  value real mean;
}
```

A header in your project defines them (here the function `atls::user::clip` and the class `atls::user::mean_of`), and the run spec names it with `header`, relative to the spec:

```toml
strategy = "dip.atls"
header = "include/user.hpp"
```

- `cmdatl strategy backtest` and `cmdatl run` compile the header into the driver before the generated strategy, and build again when it changes; `atlc check` needs none.
- Without `header` such a program fails its build, and the compiler's message names what to define.
- The header is native code in the process that runs the program, and trades in a run: name only code you trust ([the run settings](run-spec.md), "The program").

## Examples

`examples/` holds five atls programs, each with a run spec over the synthetic daily bars in `examples/data/daily.csv`; those specs are backtests (market orders and no market), which `cmdatl run` refuses.
`examples/run.toml` runs `mean-reversion` as a run (a market and limit orders), and `examples/mock.toml` is a mock account over the same bars: the commands at the top of the [README](../README.md) use both.

```sh
cmdatl strategy backtest examples/mean-reversion.toml params.lookback=30
```

- `mean-reversion` buys when the price falls well below its recent mean and sells when it comes back.
- `pairs` trades the spread of two instruments around its hedge ratio, short sales included.
- `trailing-stop` buys a breakout and sells a fraction below the highest price since the fill; it uses `on_fill`, which a run refuses.
- `universe` trades every symbol of the file at the open and the close, and leaves a missing bar out.
- `user-indicator` is mean reversion on a band written as a user indicator, with a function that caps the position.
