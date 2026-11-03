# Run settings

```sh
cmdatl strategy backtest TARGET [KEY=VALUE]... [--fills FILE]
cmdatl run TARGET [KEY=VALUE]...
```

`TARGET` is a program (`.atls`), a run spec (a TOML file), or both: at most one of each, and at least one.
A run spec given with a program applies to that program, so one spec can serve several programs.
A run spec given alone names its program with `strategy`.
Settings come from the run spec and from `KEY=VALUE` words on the command line, and the command line wins.
Both are checked by the same rules, against the program's facts (its parameters, instruments and whether it has `on_open` or `on_close`).

A word is a setting when the text before its first `=` is a dotted key: bare parts (letters, digits, `_`, `-`) joined by dots, such as `replay.cash`, `params.lookback` or `instruments.y`.
Of the other words, one ending in `.atls` is the program and any other the run spec.
The words may come in any order.

```sh
cmdatl strategy backtest dip.toml params.lookback=30 replay.cash=1e8
cmdatl strategy backtest dip.atls data.file=bars.csv data.source=csv data.symbol=005930
cmdatl strategy backtest dip.atls daily.toml instruments.stock=005930
```

A relative path is read against the directory of the file that sets it, and against the working directory on the command line (as `--fills`).
The program's path is made lexically normal (`a/../b.atls` is `b.atls`), while data paths keep their `..` for the system to resolve, through any link.
A run spec given with a program may still name it with `strategy`, and then only the same file: the two paths are compared once both are lexically normal, so a link or a name in another case is another file.
The shell does not expand a `~` inside a `KEY=VALUE` word: write `data.file=$HOME/bars.csv` or an absolute path.
The operator's file is never a setting: no setting changes the operator's limits.

## The format

A run spec is TOML, read as TOML 1.0 reads it, with these limits:
- keys are bare (letters, digits, `_`, `-`); dotted keys, inline tables and `[table]` headers flatten to the same names (`[data]` with `file = "x"` is `data.file`);
- values are strings, decimal numbers (`100_000`, `0.00015`, `1.5e-4`), `true`, `false`, and arrays of strings or numbers;
- a number is read from its text, so cash and costs stay exact decimals: written out as a plain decimal, at most 18 digits without the leading zeros and at most 255 decimal places (`1e18` and `1e-256` are refused); hexadecimal, octal and binary integers, `inf` and `nan` are refused;
- a value that must be whole (an `int` parameter, `slippage_ticks`) is written as an integer, since TOML reads `2.0` as a float;
- a date or a time is written as a string (`"2024-01-02"`), never bare;
- no string holds a control character once its escapes are read: `"C:\temp\new.csv"` holds a tab and a new line, so write a Windows path in single quotes (`'C:\temp\new.csv'`) or with forward slashes.

Errors, each with the file and line:
- an unknown key (including a misspelled one under `[build]` or `[orders]`), with the tables a spec has: `[data]`, `[instruments]`, `[replay]`, `[params]`, `[orders]`, `[build]`;
- a key set twice, a table defined twice, or a name used both as a value and as a table, as TOML refuses them;
- a value of the wrong type or outside its range, or a key that does not apply to the run (a CSV column mapping for wide tables, say).

A syntax error stops reading the file; past that, every error found is reported, not only the first.

## The command line

A value on the command line is typed by its key, never read as TOML:
- a text, path, choice, offset or instant key takes the text as it is: `instruments.stock=005930` and `data.symbol=true` are strings;
- a number, count or fraction key takes the text as a decimal number, with the file's rules: `replay.cash=1e8`;
- a boolean key takes `true` or `false`;
- a date takes `YYYY-MM-DD` or `yyyymmdd`;
- a list key takes one item (`data.symbols=AAA`) or a one-line TOML array of strings (`data.symbols=["AAA","BBB"]`, quoted for the shell);
- `params.NAME` takes a number, or `true` or `false`; then the program's type decides (`params.flag=1` is refused for a `bool`).

`strategy` is not a setting on the command line: the program is the `.atls` word, or else the run spec's `strategy`.
A key given twice on the command line is an error.

## Keys

### The program

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `strategy` | path | required unless a program is given | the program |

### `[data]`: what is loaded

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data.source` | `"wide"`, `"csv"` or `"store"` | `"store"` when `data.store` is set, else `"wide"` | where the bars come from |

`source = "wide"`: tables of one field each, a header and then rows of a yyyymmdd date and one cell per symbol (an empty cell, `NaN` or `nan` is missing), as Chan's book publishes them.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data.file`, `data.close` | path | one is required | the closes (give one of the two) |
| `data.open`, `data.high`, `data.low` | path | none | the other fields, with the closes' dates and columns; `data.open_point` needs all three |
| `data.names` | path | the tables' headers | a file whose second line names the columns (Chan's `*_stocks.csv`) |

A row is a bar with open, high and low equal to its close, unless `data.open_point` reads the other tables.

`source = "csv"`: OHLCV rows, split on commas without quoting.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data.file` or `data.files` | path, or a list of paths | one is required | one file, or one file per symbol |
| `data.symbol` or `data.symbols` | string, or a list of strings | | the symbol of `data.file`, or of each of `data.files` |
| `data.symbol_column` | string | | instead: the column naming each row's symbol |
| `data.layout` | `"long"` or `"wide"` | `"long"` | `long`: rows of OHLCV; `wide`: a time column, then a column of closes per symbol (its header names the symbols) |
| `data.time` | string | `"ts"` | the time column |
| `data.time_format` | `"unix"`, `"unix_ms"`, `"yyyymmdd"` or `"iso"` | `"unix"` | seconds or milliseconds since 1970, a yyyymmdd date, or `YYYY-MM-DD` with an optional `HH:MM[:SS]` after `T` or a space and an optional `Z` or `+HH:MM`; a date that does not exist, a field out of range or anything after the time is refused |
| `data.timezone` | `"UTC"`, `"Z"` or `"+HH:MM"` | `"UTC"` | the fixed offset of `yyyymmdd` times and of `iso` times that name none (a time's own `Z` or `+HH:MM` wins); a named zone (`Asia/Seoul`) is not read |
| `data.stamp` | string | the `stamp` column if there is one, else `"close"` | `"open"` or `"close"` (bars stamped at their start or end), or a column holding one of them per row; a symbol's rows must share one stamp |
| `data.interval` | `"1m"`, `"5m"`, `"15m"`, `"30m"`, `"1h"`, `"1d"`, `"1w"` or `"1mo"` | `"1d"` | the bars' interval |
| `data.columns.open`, `data.columns.high`, `data.columns.low`, `data.columns.close`, `data.columns.volume` | string | `"open"`, `"high"`, `"low"`, `"close"`, `"volume"` | the long layout's columns |

A long file names its symbols one way: `data.symbol` with `data.file`, `data.symbols` with `data.files` (one per file), or `data.symbol_column`.
Every row has as many cells as the header.
A time must lie in the years 1 to 9999 and inside what the platform's clock holds, less two days: about the years 1678 to 2262 where it counts nanoseconds, as on Linux; a time outside is refused, never wrapped.
Rows outside `replay.from` and `replay.to` are dropped, a row with an empty close is skipped, a price that is not positive is refused with its line, and times must increase within a symbol.

`source = "store"`: a Parquet store, laid out as `<store_root>/bars/source=SOURCE/interval=INTERVAL/symbol=SYMBOL/bars.parquet` with the columns `ts`, `stamp`, `open`, `high`, `low`, `close`, `volume` and `fetched_at`.
The store keeps every fetched version of a bar; the replay reads, of each bar, the latest version.
A stamp is `open` or `close`, and a series' bars share one stamp.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data.store` | `"SOURCE/INTERVAL/SYMBOL"`, or a list of them | required | the series read |
| `data.store_root` | path | required | the store's directory |
| `data.as_of` | instant: `"YYYY-MM-DD"`, optionally `THH:MM[:SS]` and `Z` or `+HH:MM` | the latest | read the latest version fetched at or before this instant |

A backtest of a store needs DuckDB (`[build]` below); a CSV or wide run does not.
A run (`cmdatl run`) reads files, not a store.
A spec names data only: credentials never appear in it.

### `[instruments]`: names to symbols

| Key | Type | Meaning |
| --- | --- | --- |
| `instruments.NAME` | string | the symbol (a column or a symbol of the data) for a name |

- A program with `instruments {}` needs one entry per name and no other.
- A program with one instrument takes at most one entry, which picks its symbol; without one, the data must hold a single symbol.
- A universe takes the symbols of every entry (their names are free), or every symbol of the data without any.

### `[replay]`

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `replay.from`, `replay.to` | date: `"YYYY-MM-DD"`, or yyyymmdd as a number or string, from 1970 | the first and last bar | the dates replayed, inclusive, in UTC |
| `replay.market` | `"us"` or `"kr"` | `"kr"` when the first store coordinate's source starts with `kis`, `kiwoom` or `toss` or contains `krx`, else `"us"` | `us`: USD, any price; `kr`: KRW, prices on the KRX tick table |
| `replay.cash` | number above 0 | 100000 (`us`), 100000000 (`kr`) | the opening cash |
| `replay.commission` | number, 0 or more | 0 | the commission, a rate of each fill's value |
| `replay.min_commission` | number, 0 or more | 0 | the least commission of a fill |
| `replay.sell_tax` | number, 0 or more | 0 | the tax, a rate of each sell's value |
| `replay.slippage_ticks` | whole number, 0 to 1e9 | 0 | slippage, in ticks |
| `replay.fill_header` | path, in the spec file only | none | a C++ header declaring a market-price rule (below); with `replay.fill_function` |
| `replay.fill_function` | a C++ name, such as `decision_close` or `rules::decision_close`, in the spec file only | none | the rule the header declares; with `replay.fill_header` |
| `replay.shorts` | boolean | `false` | whether a position may go below zero; no borrow is modelled |
| `replay.periods_per_year` | number above 0 | 252 | bars a year, for the annualized Sharpe ratio |

A market order fills against the bar after its decision, at that bar's open, unless the run names a market-price rule.
A limit order fills when that bar reaches its limit, whichever is set.
`replay.fill` was removed: a spec naming it is refused, with a pointer to the rule and to `data.open_point`.

A market-price rule chooses the price of a market order, in place of the next bar's open.
It is a C++ function in a header of your own, compiled into the driver with the program:

```cpp
// decision-close.hpp: the textbook convention, a market order fills at the close it was decided on.
#pragma once
#include <algotradelib/backtest>
inline std::optional<algotradelib::Decimal64> decision_close(const algotradelib::backtest::MarketPriceInput& in) {
  return in.decision_price_ ? in.decision_price_ : in.next_.bar_.open_;
}
```

```toml
[replay]
fill_header = "decision-close.hpp"
fill_function = "decision_close"
```

The function gets the order (`order_`), the price it was decided on (`decision_price_`: the close of the bar, or the quote, the program decided on; empty when it had none), when it was decided (`decided_at_`), the bar the order fills against (`next_`) and the price grid (`rule_`), and returns a price, or nothing to leave a day order for a later bar of its session.
It is the library's `NextBarFillModel::with_market_price`; slippage, commission and tax apply to its price, limit orders are untouched, nothing checks the price against the bar, and a price that is not positive stops the replay.
The report's `caveats` name the rule (`market_price_rule`) and count its fills (`market_fills_by_rule`).
A rule needs market orders: with `orders.type = "limit"` it is refused.
Both keys come from the spec file: given on the command line they are refused, since the header is C++ that cmdatl compiles and runs.
A function of the wrong signature fails the build with one line naming the expected one.

`on_open` and `on_close` run on the bars that open and close each session, whatever the interval, by the market's session (`kr`: 09:00-15:30 KST; `us`: 09:30-16:00 at UTC-5, daylight saving not modelled):

| Data | `on_open` runs on | `on_close` runs on |
| --- | --- | --- |
| intraday bars (`1m` to `1h`) | the session's first bar, at its end; the program reads only its open | the bar whose period holds the close (on KRX minute data, the 15:30 closing print) |
| daily bars with `data.open_point = true` | a point at the session's open: the day's open alone, stamped at the open | the day's bar, stamped at the close |

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data.open_point` | boolean | `false` | each daily bar replayed as a point at the session's open and the day's bar at its close; only for a program with `on_open` or `on_close`, and only on daily bars (wide tables need `data.open`, `data.high` and `data.low`) |

A program with `on_open` or `on_close` needs `replay.market`, and on daily bars `data.open_point = true`: a daily bar is a whole session, and `on_open`'s targets would be replaced by `on_close`'s on the same bar.
On US intraday bars it is refused until the session follows daylight saving.
A replay in which such a program met no session's open or close fails, naming the market and the interval; the report gives `sessions_opened` and `sessions_closed`, and `caveats.open_points` when `data.open_point` is set.
With the decision-close rule, a target stated in `on_open` fills at the open it saw and one in `on_close` at the close it saw; without a rule, at the next bar's open.
A fill is dated at the bar it fills against: an `on_close` exit at the next session's first bar, so the last session's exit does not fill.

A run (`cmdatl run`) takes neither a program with `on_open` or `on_close`, nor a market-price rule, nor `data.open_point`.

### `[params]`

| Key | Type | Meaning |
| --- | --- | --- |
| `params.NAME` | number, or `true`/`false` for a `bool` | a parameter's value (also `params.NAME=VALUE` on the command line) |

The name must be the program's; an `int` takes a whole number of magnitude at most 9e15, a `real` a number, a `bool` `true` or `false`.
Bounds are checked when the strategy is built, against the final values whatever order they were given in (a bound may name another parameter), and a value outside its bounds stops the replay.

### `[orders]`: how a target becomes an order

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `orders.type` | `"market"` or `"limit"` | `"market"` | a market order, or a limit priced from the price of the event that ran the handler |
| `orders.offset` | a plain decimal, at least 0 and below 0.3 | 0 | how far a limit lies past that price, as a fraction; only with `orders.type = "limit"` |

The offset is written as a plain decimal of at most 18 fractional digits (`0.002`, `0.299999999999999999`), which the replay reads exactly; an exponent (`2e-3`) is refused, never converted through a binary number.
A run takes an offset of at most 8 decimals.
The file and the command line are checked together, so an offset without `orders.type = "limit"` is refused whichever sets either.
Changing `[orders]` does not rebuild: the pricing is an argument, like the parameters.

### `[build]`

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `build.duckdb` | path | `DUCKDB_ROOT`, then the system's paths | the directory of DuckDB's `duckdb.h` and shared library, for a store backtest |
| `build.indicators` | `"incremental"`, `"naive"` or `"exact"` | `"incremental"` | how windows are summed (the language reference's types chapter, "Arithmetic") |

`build.duckdb` holding `"`, `;` or `$` cannot be passed to CMake and is refused at its setting for a store backtest, before anything is built.
`build.algotradelib` is refused: cmdatl builds against the algotradelib it was built with, which `cmdatl env` shows.

## The build

cmdatl compiles each program once per shape: `atlc build` translates the program, and CMake builds it with a C++17 compiler against the algotradelib cmdatl was built with, in Release, with floating-point contraction off (`-ffp-contract=off` on GCC, the runtime's pragmas on Clang).
A store backtest also links DuckDB's prebuilt C library (`duckdb.h` and the shared library), from `build.duckdb` alone when it is set, else `DUCKDB_ROOT`, then the system's library paths; the executable finds the library by an rpath to its directory.
Builds are cached under `$XDG_CACHE_HOME/cmdatl/drivers` (else `~/.cache/cmdatl/drivers`), and CMake decides what to rebuild.
Parameters, data and replay settings are arguments of the executable, so changing them never rebuilds.
Two backtests of one program at once share one build and do not wait on each other's replays.
Each backtest runs a copy of the build from `$XDG_CACHE_HOME/cmdatl/backtests/` and removes it; a killed backtest leaves its copy there, and the directory is safe to delete while no backtest runs.

## The report

One JSON object, as `cmdatl strategy backtest` returns it in `report`:

| Field | Type | Meaning |
| --- | --- | --- |
| `bars` | integer | bar events replayed (each instrument's bar counts once; a day split at its open counts two) |
| `executions` | integer | fills |
| `opening_equity`, `closing_equity` | decimal string | equity at the first and last recorded point |
| `total_return` | number or null | over the replay, as a fraction |
| `sharpe_per_period`, `sharpe_annualized` | number or null | the Sharpe ratio per bar, and annualized with `replay.periods_per_year` |
| `sortino_per_period` | number or null | the Sortino ratio per bar |
| `max_drawdown` | number or null | the worst peak-to-trough fall of equity, as a fraction |
| `turnover` | number or null | one-way turnover summed over the replay, as a fraction of equity |
| `commission_paid`, `slippage_paid`, `tax_paid` | decimal string or null | costs paid; null when the library has no total (nothing executed, no tax applies, the executions span currencies, or a sum did not fit) |
| `rejected` | integer | instructions not placed (reconciliation could not build them, or the replay cannot model them) |
| `refused` | integer | instructions a risk guard refused |
| `halted` | string or null | why the replay stopped early, if it did; read it before any figure |
| `aborted` | boolean | whether a halt cleared queued orders |
| `rejection_reasons` | object | each rejection and refusal reason, with its count (refusals as `guard:REASON`) |
| `pricing_refused` | integer | targets limit pricing did not submit ("Limit orders"); 0 with market orders |
| `pricing_refusal_reasons` | object | each pricing refusal reason with its count, when nonzero: `no_basis_price`, `no_price_grid`, `price_not_representable`, `price_not_positive`, `net_not_representable` |
| `caveats` | object | `gaps` (bars that did not follow their predecessor), `unfilled` (orders still queued at the end), `expired` (`DAY` orders withdrawn unfilled when their session ended, as they would expire live; on daily bars, after one bar), `unvalued_bars`, `multiple_instruments`, `cost_totals_incomplete`, `volume_ignored` (always true: fills are not capped by volume), `market_price_rule` and `market_fills_by_rule` (the rule named by `replay.fill_function` and the fills it priced, only when one is set), `open_points` (true with `data.open_point`), `lowest_cash` (the lowest cash a fill left, as a decimal string, when below zero) and `shorts_borrow_nothing` (always true) |
| `first_bar`, `last_bar` | string | the first and last recorded times, ISO 8601 in UTC; absent when nothing was replayed |

A number is printed with ten significant digits; a figure the library leaves undefined (a return on zero equity, a ratio without variance) is null.

`--fills FILE` writes one line per fill, after the header `at,symbol,side,quantity,price,commission,tax`: the time (ISO 8601, UTC), the symbol, `buy` or `sell`, and decimals.

## Limit orders

With `orders.type = "limit"`, the generated strategy gives each target it submits a limit price; the same code runs in backtests and in runs.
- The price is that of the event that ran the handler, taken exactly from the bar (its close): in `on_bar`, the decision bar's close; in `on_open`, the opening price (today's close is not visible there); in `on_close`, the close.
  Named instruments use each instrument's own bar; a universe keeps each instrument's last event price, so no instrument is priced from another's.
- The side compares the target in whole shares (truncated toward zero) with the net position: above it, a buy; below it, a sell.
- The limit is `price × (1 + offset)` for a buy and `price × (1 − offset)` for a sell, computed exactly, then put on the instrument's tick grid on the safe side (a buy rounded down, a sell up).
- A target that cannot be priced is not submitted, and is counted by reason in the report's `pricing_refused` and `pricing_refusal_reasons`; a requested limit never becomes a market order.

The backtester fills a limit when the next bar's range reaches it: a buy when the bar's low is at or below the limit, a sell when its high is at or above it.
The fill is credited at the limit, bounded into the bar, before and after slippage, so slippage never takes it through the limit.
A limit that does not fill is a `DAY` order: it is withdrawn when its session ends (on daily bars, after the next bar) and counted in `caveats.expired`; one still queued at the end is counted in `caveats.unfilled`.
The replay supplies no daily price limits, so a limit is never clamped in a backtest; what a runner that knows the day's band does is in atlc's [generated code reference](https://github.com/algotradelib/atlc/blob/main/docs/reference/generated-code.md).
