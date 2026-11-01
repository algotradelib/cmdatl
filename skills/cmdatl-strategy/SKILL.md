---
name: cmdatl-strategy
description: How to write a trading strategy in atls and backtest and run it with cmdatl - start from a template, check the program, edit the run spec, read a backtest's report, and start a run on its own mock market. Use for any task that writes, tests or runs a strategy with cmdatl or atls.
---

# Strategies with cmdatl

One cmdatl command per shell call; replies are compact JSON when piped.
On a name, a field or a value, cmdatl's replies and help are right where this guide differs; `cmdatl guide strategy --format text` prints the installed binary's guide.

## Start from a template

`cmdatl strategy new NAME --template mean-reversion` (or `crossover`, `momentum`, `breakout`) writes `NAME.atls`, the program, and `NAME.toml`, the run spec, which reads `bars.csv` beside it (columns `symbol,date,open,high,low,close,volume`).
It never replaces a file unless `--force` is given.
Read both files before editing.

## The language

atls is atlc's language: `atlc check NAME.atls` checks a program alone, with each error's line and code.
Look a built-in up in atlc's reference (https://github.com/algotradelib/atlc/tree/main/docs/reference, `builtins.md`) rather than guess.

```
params { int<lower=2> slow = 30; int<lower=0> quantity = 50; }
series { real m = sma(close, slow); }
on_bar { if (close > m) { target(quantity); } else { target(0); } }
```

- `target(x)` is the position to hold, in shares; `held` is the current one.
- Indicators and lags (`x[k]`, `x` k bars ago) go in `series`, never under an `if` or in `on_bar`: `bool up = m > m[1];`.
- Nothing states `slow > fast` between parameters: keep them so yourself.

## The run spec

The spec is atlc's TOML: `[data]` (the file and its columns), `[instruments]` (the symbol the program trades), `[replay]` (`cash`, `commission` and the other costs), `[orders]` (`type = "market"` or `"limit"`, and `offset` for a limit).

## Backtests

`cmdatl strategy backtest NAME.toml -P lookback=30` builds the program once (later backtests of an unchanged program build nothing) and replays it.
`-P name=value` sets a parameter for this call without editing the file.
The reply's `report` is atlc's, unchanged: `total_return`, `sharpe_annualized`, `max_drawdown`, `executions`, `commission_paid`, `tax_paid` and more; `--fields report.total_return,report.max_drawdown` keeps those.
Build progress goes to stderr.
Exit 5 is a program error (`program.invalid`, with atlc's diagnostics in `errors`), 2 a spec or a parameter out of its bounds (`spec.invalid`, `usage.bad_value`), 1 a failed build or replay.

## Runs

A run trades the program through algotradelib's live runner on a mock market built from the spec's data; the operator's file limits every order it places.
The starter spec cannot run as written: its `[orders]` places market orders and it names no market.
Replace its `[instruments]`, `[replay]` and `[orders]` tables with:

```toml
[instruments]
stock = "005930"                  # a symbol in the operator's limits.symbols

[replay]
cash = 100000000
commission = 0.0005
market = "kr"                     # the market a run trades

[orders]
type = "limit"                    # a run places limit orders only
offset = 0.01                     # the limit 1 % past the price, inside the operator's limits.collar
```

- A run also needs daily bars from a file and the operator's armed file; each missing condition is refused before anything is built, with what to do in `error.next`.
  The armed file is the operator's: never create it; on `refused.not_armed`, ask.
- After the build, a run refuses a program with `on_fill` or session legs (`on_open`, `on_close`) and a warm-up above 10,000 bars (exit 5, `program.live_unsupported`).
- Daily bars from date-only rows end at 09:00 KST, in `open`; bars stamped at the 15:30 close fall in `post_close`, so a run of those trades only when the operator permits `post_close`.
- `cmdatl run start NAME.toml --wait` answers with the summary (`executions`, `commission_paid`, `closing_equity`, `holdings_at_end`); without `--wait`, `cmdatl run status` and `cmdatl run log` show it.
  Starting, watching and stopping runs, and every exit code: the agent guide (`cmdatl guide agent`).

When a run's fills differ from the backtest's, the README's rule says why:
The run starts at the first bar and steps through the data 15 market minutes at a time, as fast as the machine allows; at the end it withdraws whatever rests and confirms it before it reports.
A run's fills are those of `cmdatl strategy backtest` on the same spec when the program decides from bars alone, the bars end on that step (date-only rows do), and none of the operator's limits, the cash or the order budget refuses an order the backtest makes, since the backtest applies none of them.

## A session

```sh
cmdatl strategy new dip --template mean-reversion
atlc check dip.atls
cmdatl strategy backtest dip.toml -P lookback=30
cmdatl run start dip.toml --wait
cmdatl run status
cmdatl run log
```

Between `strategy new` and the backtest, put `bars.csv` beside the spec and replace its three tables with the block above.
