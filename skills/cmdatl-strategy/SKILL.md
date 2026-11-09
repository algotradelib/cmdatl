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
`cmdatl strategy reference` prints the language reference's index, and `cmdatl strategy reference builtins` (or `syntax`, `program-structure`) a chapter, as the installed atlc has them; `cmdatl strategy reference run-spec` is cmdatl's own list of the run settings.
Look a built-in up there rather than guess.

```
params { int<lower=2> slow = 30; int<lower=0> quantity = 50; }
series { real m = sma(close, slow); }
on_bar { if (close > m) { target(quantity); } else { target(0); } }
```

- `target(x)` is the position to hold, in shares; `held` is the current one.
- On a broker account `held` is the account's holding, on the history bars read at the start too, so a `state` set from `held` is rebuilt from today's holding at every start; the run may sell every share of its symbols the account holds.
- Indicators and lags (`x[k]`, `x` k bars ago) go in `series`, never under an `if` or in `on_bar`: `bool up = m > m[1];`.
- Nothing states `slow > fast` between parameters: keep them so yourself.

## The run spec and settings

The spec is TOML: `[bars]` (the file and its columns), `[instruments]` (the symbol the program trades), `[replay]` (`cash`, `commission` and the other costs), `[params]`, `[orders]` (`type = "market"` or `"limit"`, and `offset` for a limit).
On the command line, a `KEY=VALUE` word with a dotted key is a setting for this call and wins over the spec's: `params.lookback=30`, `replay.cash=1e8`, `instruments.stock=005930` (a string, as typed).
Give a program (`NAME.atls`), a spec, or both, with the settings in any order; a spec given with a program applies to it (one spec can serve several programs) and names no other with `strategy`.

## Backtests

`cmdatl strategy backtest NAME.toml params.lookback=30` builds the program once (later backtests of an unchanged program build nothing) and replays it.
`params.NAME=VALUE` sets a parameter for this call without editing the file.
The reply's `report` is the replay's, unchanged: `total_return`, `sharpe_annualized`, `max_drawdown`, `executions`, `commission_paid`, `tax_paid` and more; `--fields report.total_return,report.max_drawdown` keeps those.
Build progress goes to stderr.
Exit 5 is a program error (`program.invalid`, with atlc's diagnostics in `errors`), 2 a settings error (`spec.invalid`, each with its file and line or the command line in `errors`), 1 a failed build or replay (`build.failed`; `replay.failed`, a parameter outside its bounds included).

## Runs

A run trades the program through algotradelib's live runner on a mock market built from the settings' data, read as a backtest reads them; the operator's file limits every order it places, and no setting changes it.
To test a program, backtest it; start a run only when the task asks for a run.
The starter spec cannot run as written: its `[orders]` places market orders and it names no market.
Replace its `[instruments]`, `[replay]` and `[orders]` tables with:

```toml
[instruments]
stock = "005930" # a symbol in the operator's limits.symbols

[replay]
cash = 100000000
commission = 0.0005
market = "kr" # the market a run trades

[orders]
type = "limit" # a run places limit orders only
offset = 0.01 # a buy's limit 1 % above the price, a sell's 1 % below, inside the operator's limits.collar
```

- A run also needs bars from a file and the operator's armed file; each missing condition is refused before anything is built, with what to do in `error.next`.
  The armed file is the operator's: never create, remove or edit it or the operator's file; on `refused.not_armed`, ask.
- A run refuses a program with `on_open` or `on_close` before the build (exit 5, `program.live_unsupported`): backtest such a program instead.
- `on_open` and `on_close` follow the market's session on any bars (Korean bars in a CSV: `market = "kr"` under `[replay]`), with `open_point = true` under `[bars]` on daily bars; a backtest in which no bar opened or closed a session fails.
- `fill = "decision-close"` under `[replay]` fills a backtest's market orders at the price the program decided on (the textbook close), instead of the next bar's open; a run refuses it.
- `fill = "next-close"` under `[replay]` fills them at the close of the bar they fill against (zipline's price basis); an order it would fill on a weekly or monthly bar halts the backtest; a run refuses it too.
- A market-price rule of your own (`fill_header` and `fill_function` under `[replay]`) is C++ cmdatl compiles and runs: only in the spec file, never on the command line.
- After the build, a run refuses a program with `on_fill` or a warm-up above 10,000 bars (exit 5, `program.live_unsupported`), and bars that are not daily (`unsupported.interval`).
- Daily bars from date-only rows end at 09:00 KST, in `open`; bars stamped at the 15:30 close fall in `post_close`, so a run of those trades only when the operator permits `post_close`.
- `cmdatl run NAME.toml` runs in the foreground and prints its events, then its reply (`executions`, `commission_paid`, `closing_equity`, `holdings_at_end`, `refused`) as the last line.
  Stopping runs, and every exit code: the agent guide (`cmdatl guide agent`).
- On a KIS account (the operator's file says `broker = "kis"`) a run spec names `[instruments]` and no `[bars]`: the broker gives daily bars, and the run lasts in real time until stopped.

When a run's fills differ from the backtest's, this rule says why:
The run starts at the first bar and steps through the data 15 market minutes at a time, as fast as the machine allows; at the end it withdraws whatever rests and confirms it before it reports.
A run's fills are those of `cmdatl strategy backtest` on the same spec when the program decides from bars alone, the bars end on that step (date-only rows do), and none of the operator's limits, the cash or the order budget refuses an order the backtest makes, since the backtest applies none of them.

## A session

```sh
cmdatl strategy new dip --template mean-reversion
cmdatl strategy reference builtins
atlc check dip.atls
cmdatl strategy backtest dip.toml params.lookback=30
cmdatl run dip.toml
```

Between `strategy new` and the backtest, put `bars.csv` beside the spec and replace its three tables with the block above; the run command only when the task asks for a run.
