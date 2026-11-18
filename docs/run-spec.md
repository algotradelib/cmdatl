# Run settings

```sh
cmdatl strategy backtest TARGET [KEY=VALUE]... [--fills FILE] [--equity FILE]
cmdatl strategy evaluate TARGET [KEY=VALUE]...
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
cmdatl strategy backtest dip.atls bars.file=bars.csv bars.source=csv bars.symbol=005930
cmdatl strategy backtest dip.atls daily.toml instruments.stock=005930
```

A relative path is read against the directory of the file that sets it, and against the working directory on the command line (as `--fills` and `--equity`).
The program's path is made lexically normal (`a/../b.atls` is `b.atls`), while data paths keep their `..` for the system to resolve, through any link.
A run spec given with a program may still name it with `strategy`, and then only the same file: the two paths are compared once both are lexically normal, so a link or a name in another case is another file.
The shell does not expand a `~` inside a `KEY=VALUE` word: write `bars.file=$HOME/bars.csv` or an absolute path.
The operator's file is never a setting: no setting changes the operator's limits.

## The format

A run spec is TOML, read as TOML 1.0 reads it, with these limits:
- keys are bare (letters, digits, `_`, `-`); dotted keys, inline tables and `[table]` headers flatten to the same names (`[bars]` with `file = "x"` is `bars.file`);
- values are strings, decimal numbers (`100_000`, `0.00015`, `1.5e-4`), `true`, `false`, and arrays of strings or numbers;
- a number is read from its text, so cash and costs stay exact decimals: written out as a plain decimal, at most 18 digits without the leading zeros and at most 255 decimal places (`1e18` and `1e-256` are refused); hexadecimal, octal and binary integers, `inf` and `nan` are refused;
- a value that must be whole (an `int` parameter, `slippage_ticks`) is written as an integer, since TOML reads `2.0` as a float;
- a date or a time is written as a string (`"2024-01-02"`), never bare;
- no string holds a control character once its escapes are read: `"C:\temp\new.csv"` holds a tab and a new line, so write a Windows path in single quotes (`'C:\temp\new.csv'`) or with forward slashes.

Errors, each with the file and line:
- an unknown key (including a misspelled one under `[build]` or `[orders]`), with the tables a spec has: `[bars]`, `[data.NAME]`, `[instruments]`, `[replay]`, `[params]`, `[orders]`, `[build]`, and for `strategy evaluate` `[evaluate]`, `[grid]`, `[windows]`, `[walk_forward]` and `[bootstrap]` ("Many replays");
- a key set twice, a table defined twice, or a name used both as a value and as a table, as TOML refuses them;
- a value of the wrong type or outside its range, or a key that does not apply to the run (a CSV column mapping for wide tables, say).

A syntax error stops reading the file; past that, every error found is reported, not only the first.

## The command line

A value on the command line is typed by its key, never read as TOML:
- a text, path, choice, offset or instant key takes the text as it is: `instruments.stock=005930` and `bars.symbol=true` are strings;
- a number, count or fraction key takes the text as a decimal number, with the file's rules: `replay.cash=1e8`;
- a boolean key takes `true` or `false`;
- a date takes `YYYY-MM-DD` or `yyyymmdd`;
- a list key takes one item (`bars.symbols=AAA`) or a one-line TOML array of strings (`bars.symbols=["AAA","BBB"]`, quoted for the shell);
- `params.NAME` takes a number, or `true` or `false`; then the program's type decides (`params.flag=1` is refused for a `bool`).
- `grid.NAME` takes comma-separated numbers (`grid.lookback=10,20,30`) or a one-line TOML array of numbers.

`strategy`, `header` and `measures` are not settings on the command line: the program is the `.atls` word, or else the run spec's `strategy`, and its header and measures are the run spec's.
A key given twice on the command line is an error.

## Keys

### The program

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `strategy` | path | required unless a program is given | the program |
| `header` | path | none | the C++ header that defines the program's external functions and indicators |
| `measures` | a C++ name or a list of them, in the spec file only | none | functions of `header` whose values a backtest's report adds (below) |

A program that declares `extern` functions or indicators has its bodies in C++ of your own ([atlc's reference](https://github.com/algotradelib/atlc/blob/main/docs/reference/functions-and-indicators.md#external-c), "External C++"), and `header` names the header that defines them:

```toml
strategy = "pairs.atls"
header = "include/statistics.hpp"
```

- The path is read against the run spec's directory, as `strategy` is; it names one header, which may include others.
- cmdatl includes it in the driver before the generated strategy, in a backtest and in a run alike, on the mock market or an account.
- A path that is not a regular file, or that holds `"`, `\`, `??` (which starts a trigraph) or a line break, is refused at its line before anything is built.
- Without it, such a program fails its build (`build.failed`), and the reply's message starts with the compiler's first error, which names the class or function to define.
- The header is native code compiled into the process that runs the program, and trades in a run: name only code you trust.
- When `evaluate.jobs` is above 1 ("Many replays"), its functions run on several threads at once and must be thread-safe: no mutable static or global state, no unsynchronized cache, and no change of the locale.

#### Measures of your own

`measures` names functions of `header` that compute a figure of the finished replay, which the report's `measures` gives under each name, in order:

```cpp
#include <algotradelib/backtest>

inline double final_level(const algotradelib::backtest::MeasureInput& in) {
  return in.levels_.size() == 0 ? 0 : in.levels_[in.levels_.size() - 1];
}
```

- Each is defined in the header (`inline`), callable as `double NAME(const algotradelib::backtest::MeasureInput&)`, and reads the replay, its report and the curve's levels and returns from the library's input; the header includes `<algotradelib/backtest>` itself.
- Each name is a C++ identifier of at most 64 characters, not starting with `cmdatl_`, at most 64 names, none twice; a namespaced function is named through a `using` in the header.
- A name that cannot be called so fails the build (`build.failed`), and the compiler's first error in the message usually says `measures: NAME must be callable as double(const algotradelib::backtest::MeasureInput&)`; for a C++ keyword or a namespace in scope (`std`, `algotradelib`, `atls`) it is the compiler's own.
- A value that is not finite is null; a measure that throws fails the backtest (`replay.failed`, naming it for a `std::exception`), and no `--fills` or `--equity` file is written.
- A measure computes from its input and returns: it never writes to stdout or changes process-wide state (the locale, the floating-point environment); when `evaluate.jobs` is above 1 ("Many replays"), measures run on several threads at once and must also be thread-safe.
- Only a backtest computes them: `cmdatl run` accepts the key and computes none.
- A program's own `measures {}` (atls) come first in the report, in its order, then these; a name the program's block holds cannot be named here too (`spec.invalid`, at the key's line).

### `[bars]`: what is loaded

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.source` | `"wide"`, `"csv"`, `"store"`, `"stdin"` or a broker folder's name | `"store"` when `bars.store` is set, else `"wide"` | where the bars come from |

`source = "wide"`: tables of one field each, a header and then rows of a yyyymmdd date and one cell per symbol (an empty cell, `NaN` or `nan` is missing), as Chan's book publishes them.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.file`, `bars.close` | path | one is required | the closes (give one of the two) |
| `bars.open`, `bars.high`, `bars.low` | path | none | the other fields, with the closes' dates and columns; `bars.open_point` needs all three |
| `bars.names` | path | the tables' headers | a file whose second line names the columns (Chan's `*_stocks.csv`) |

A row is a bar with open, high and low equal to its close, unless `bars.open_point` reads the other tables.

`source = "csv"`: OHLCV rows, split on commas without quoting.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.file` or `bars.files` | path, or a list of paths | one is required | one file, or one file per symbol |
| `bars.symbol` or `bars.symbols` | string, or a list of strings | | the symbol of `bars.file`, or of each of `bars.files` |
| `bars.symbol_column` | string | | instead: the column naming each row's symbol |
| `bars.layout` | `"long"` or `"wide"` | `"long"` | `long`: rows of OHLCV; `wide`: a time column, then a column of closes per symbol (its header names the symbols) |
| `bars.time` | string | `"ts"` | the time column |
| `bars.time_format` | `"unix"`, `"unix_ms"`, `"yyyymmdd"` or `"iso"` | `"unix"` | seconds or milliseconds since 1970, a yyyymmdd date, or `YYYY-MM-DD` with an optional `HH:MM[:SS]` after `T` or a space and an optional `Z` or `+HH:MM`; a date that does not exist, a field out of range or anything after the time is refused |
| `bars.timezone` | `"UTC"`, `"Z"` or `"+HH:MM"` | `"UTC"` | the fixed offset of `yyyymmdd` times and of `iso` times that name none (a time's own `Z` or `+HH:MM` wins); a named zone (`Asia/Seoul`) is not read |
| `bars.stamp` | string | the `stamp` column if there is one, else `"close"` | `"open"` or `"close"` (bars stamped at their start or end), or a column holding one of them per row; a symbol's rows must share one stamp |
| `bars.interval` | `"1m"`, `"5m"`, `"15m"`, `"30m"`, `"1h"`, `"1d"`, `"1w"` or `"1mo"` | `"1d"` | the bars' interval |
| `bars.columns.open`, `bars.columns.high`, `bars.columns.low`, `bars.columns.close`, `bars.columns.volume` | string | `"open"`, `"high"`, `"low"`, `"close"`, `"volume"` | the long layout's columns |

A long file names its symbols one way: `bars.symbol` with `bars.file`, `bars.symbols` with `bars.files` (one per file), or `bars.symbol_column`.
Every row has as many cells as the header.
A time must lie in the years 1 to 9999 and inside what the platform's clock holds, less two days: about the years 1678 to 2262 where it counts nanoseconds, as on Linux; a time outside is refused, never wrapped.
Rows outside `replay.from` and `replay.to` are dropped, a row with an empty close is skipped, a price that is not positive is refused with its line, and times must increase within a symbol.

`source = "stdin"`: the long layout above, read from standard input (a header line, then rows), for an evaluation only: `strategy backtest` and `cmdatl run` refuse it.
`bars.file`, `bars.files`, `bars.symbols` and `bars.layout = "wide"` do not apply; the symbols are `bars.symbol` or `bars.symbol_column`.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.path_column` | string | none: the stream is one path | the column naming each row's path |

- A path ends when the cell changes: a later run of rows with an earlier cell is a new path, numbered in the order read; a cell is 1 to 64 bytes.
- Every path carries the first path's symbols; `replay.from` and `replay.to` filter each path, and a path left with no bar stops the evaluation.
- A malformed row, a line over 1 MiB or a path that breaks these rules stops the evaluation with its line (`stdin:LINE`), after the lines already printed.
- A generator that dies between rows leaves a stream that looks complete: run the pipeline with `set -o pipefail` and read the generator's own exit.

`source = "store"`: a Parquet store, laid out as `<store_root>/bars/source=SOURCE/interval=INTERVAL/symbol=SYMBOL/bars.parquet` with the columns `ts`, `stamp`, `open`, `high`, `low`, `close`, `volume` and `fetched_at`.
The store keeps every fetched version of a bar; the replay reads, of each bar, the latest version.
A stamp is `open` or `close`, and a series' bars share one stamp.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.store` | `"SOURCE/INTERVAL/SYMBOL"`, or a list of them | required | the series read |
| `bars.store_root` | path | required | the store's directory |
| `bars.as_of` | instant: `"YYYY-MM-DD"`, optionally `THH:MM[:SS[.ffffff]]` and `Z` or `+HH:MM` | the latest | read the latest version fetched at or before this instant |

`source = "NAME"`, a broker folder's name (`docs/brokers.md`, "Your own broker or data vendor"): the backtest fetches each symbol's last bars through the folder's adapter, with the login `cmdatl auth login --broker NAME --adapter PATH` stored (`NAME/real`, bound to that folder), writes them into the store as one version, then replays that version.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.adapter` | path | required | the broker folder, relative to the run spec; in the spec file only, never on the command line |
| `bars.count` | whole number, 1 to 100000 | required | the last bars of each symbol fetched (an adapter may refuse more than its broker serves; fewer may come back) |
| `bars.interval` | as for `"csv"` | `"1d"` | the bars' interval |
| `bars.store_root` | path | required | the store the bars are written into, as for `"store"` |
| `bars.options.NAME` | as the folder's `broker.toml` declares | required if declared so | an option the adapter takes, in `[bars.options]`; on the command line typed by its declaration (`docs/brokers.md`, "Options") |

- `replay.market` is required: it gives the instruments' country, which the adapter is asked for.
- The symbols come from `[instruments]`; a symbol holding `/`, `\`, `=`, `,`, a glob character (`*?[]{}`), a control character or a space at either end, or being `.` or `..`, is refused before anything is fetched.
- The store is written in its own layout and by its own rule: `source=NAME`, every version kept, a bar appended only when it is new or changed, under the series directory's `.lock`.
- The replay is exactly the bars this fetch received (a later fetch by another writer and an older history already in the store are left out); `replay.from` and `replay.to` narrow it further, and a symbol answered with no bars is refused (`fetch.failed`).
- The reply's `fetched` gives `fetched_at` (to the microsecond) and, per symbol, the bars fetched, those new to the store and the first and last date.
  `bars.source = "store"` with `bars.as_of = fetched_at` reads the store as it stood after the fetch: every bar's latest version up to that instant, which can include bars this fetch did not return (an older history, or bars an earlier fetch had and this one did not); it is the same history only when the store held nothing else for those series.
- Prices are raw (not adjusted), each within `DECIMAL(18,4)`, and the history is the vendor's as of the fetch, revisions included: it is not what was known on each day.
  An adapter's option cannot change that: an answer marked adjusted is refused.
- An option the folder does not declare, of another type, outside its choices, or a required one missing, is refused before anything is built (`spec.invalid`); the adapter receives the options as a JSON object's text.
- An adapter's error is reported by its category, HTTP status and code (`fetch.failed`), never by its text, which could carry a credential.
- A login bound to another folder, or to none, is refused before anything is built (`auth.adapter_mismatch`).

A backtest of a store or of a broker folder needs DuckDB (`[build]` below); a CSV or wide run does not.
A run (`cmdatl run`) reads files, not a store or a broker folder.
A spec names data only: credentials never appear in it.

### `[data.NAME]`: the program's data values

A program with `data {}` reads values from outside the bars; each name it declares is mapped to a file in its own table, `[data.NAME]`, and a name the program does not declare is refused.

```toml
[data.announced]
file = "data/earnannFile.csv"
layout = "wide"
time = "Date"
time_format = "yyyymmdd"
timezone = "UTC"
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data.NAME.file` | path | required | the file |
| `data.NAME.layout` | `"long"` or `"wide"` | `"long"` | `long`: a time column and a value column; `wide`: a time column, then a column per symbol (its header names the symbols) |
| `data.NAME.column` | string | `NAME` | the long layout's value column |
| `data.NAME.symbol_column` | string | | the long layout's symbol column; without it a value is for every instrument |
| `data.NAME.time` | string | `"ts"` | the time column |
| `data.NAME.time_format`, `data.NAME.timezone` | as `bars.time_format` and `bars.timezone` | `"unix"`, `"UTC"` | how its times read |

- **The time is when the value became known**, not the date it describes: the program reads a value only from then on, and at a bar the latest known by its end.
  A date alone is known at its midnight in `timezone`: that is your statement that the value was available then, which cmdatl cannot check.
- A value is a number; an empty cell or `NaN` is undefined, anything else is refused with its file and line.
- Symbols the run does not trade are skipped; a symbol it trades that a wide file or a symbol column lacks is refused, and so is a blank symbol.
  A program with `instruments {}` reads one value for the program: a wide file or a symbol column is refused.
- Rows at one instant keep their file's order: the later row is the newer value.
- Values through `replay.to`'s day are read; values before `replay.from` are kept, so the first bar reads the latest known.
- The report counts the values fed (`records`), for a program with `data {}` only.
- A run reads each value at its first poll at or after its time.

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
| `replay.market` | `"us"` or `"kr"` | `"kr"` when the first store coordinate's source starts with `kis`, `kiwoom` or `toss` or contains `krx`, else `"us"` | `us`: USD, any price; `kr`: KRW, prices on the KRX equity tick table, also for ETFs and ETNs, whose live ladder is finer (1 KRW below 2,000 KRW, 5 KRW from 2,000 KRW), so a replay's limits and `slippage_ticks` are coarser for them and can fill differently from a live run |
| `replay.cash` | number above 0 | 100000 (`us`), 100000000 (`kr`) | the opening cash |
| `replay.commission` | number, 0 or more | 0 | the commission, a rate of each fill's value |
| `replay.min_commission` | number, 0 or more | 0 | the least commission of a fill |
| `replay.sell_tax` | number, 0 or more | 0 | the tax, a rate of each sell's value |
| `replay.slippage_ticks` | whole number, 0 to 1e9 | 0 | slippage, in ticks |
| `replay.fill` | `"next-open"`, `"decision-close"` or `"next-close"` | `"next-open"` | the price of a market order (below) |
| `replay.fill_header` | path, in the spec file only | none | a C++ header declaring a market-price rule of your own (below); with `replay.fill_function` |
| `replay.fill_function` | a C++ name, such as `decision_close` or `rules::decision_close`, in the spec file only | none | the rule the header declares; with `replay.fill_header` |
| `replay.shorts` | boolean | `false` | whether a position may go below zero; no borrow is modelled |
| `replay.periods_per_year` | number above 0 | 252 | bars a year, for the annualized Sharpe ratio |

A market order fills against the bar after its decision, at the price `replay.fill` names:

| `fill` | A market order fills at | As in |
| --- | --- | --- |
| `next-open` | that bar's open | backtrader's and backtesting.py's default |
| `decision-close` | the price it was decided on: the close of the bar the program decided on (the open in `on_open` with `bars.open_point`), or the quote; with no price yet, that bar's open | backtrader's cheat-on-close, backtesting.py's `trade_on_close` |
| `next-close` | that bar's close | zipline's price basis, backtrader's `Order.Close` |

`decision-close` is the textbook convention, and optimistic: the program trades at a price it has already seen, which a market cannot give, since a closing-auction order is sent before the closing price is known.
The price is the instrument's last one when the program decided, so it can be days old after a gap in that instrument's data.
It is the library's `MarketPrice::DECISION_CLOSE`; the report's `caveats` name it (`market_price_rule`) and count its fills (`market_fills_by_rule`).
`next-close` is a price convention, not a closing-order simulation: the fill keeps the bar's timing (stamped at the bar's end, charged on its trading date, a `DAY` order expiring with its session).
That timing holds for intraday bars inside one session and for daily trading-date bars; a close-stamped daily bar from a date alone is taken at its midnight in `bars.timezone` (by default UTC, 09:00 KST), so without `bars.open_point` its close is credited at an instant before the session closed.
On weekly or monthly bars (`bars.interval = "1w"` or `"1mo"`) an order it would price halts the replay: the report's `halted` names `next_close_bar_too_long`, and fills before it stay.
It is the library's `MarketPrice::NEXT_CLOSE`, named and counted in `caveats` as `decision-close` is.
A limit order fills when that bar reaches its limit, whatever `fill` says.
`fill` is a driver argument: changing it builds nothing, and it may be given on the command line (`replay.fill=decision-close`).
Other than `next-open`, it needs market orders (with `orders.type = "limit"` it is refused) and no rule of your own.

A market-price rule of your own chooses any other price: a C++ function in a header of your own, compiled into the driver with the program, such as the fill bar's typical price (`examples/typical-price.hpp`):

```toml
[replay]
fill_header = "typical-price.hpp"
fill_function = "typical_price"
```

The function gets the order (`order_`), the price it was decided on (`decision_price_`: the close of the bar, or the quote, the program decided on; empty when it had none), when it was decided (`decided_at_`), the bar the order fills against (`next_`) and the price grid (`rule_`), and returns a price, or nothing to leave a day order for a later bar of its session.
It is the library's `NextBarFillModel::with_market_price`; slippage, commission and tax apply to its price, limit orders are untouched, nothing checks the price against the bar, and a price that is not positive stops the replay.
The report's `caveats` name the rule (`market_price_rule`) and count its fills (`market_fills_by_rule`).
A rule needs market orders: with `orders.type = "limit"` it is refused.
Both keys come from the spec file: given on the command line they are refused, since the header is C++ that cmdatl compiles and runs.
A function of the wrong signature fails the build with one line naming the expected one.
When `evaluate.jobs` is above 1, the rule runs on several threads at once and must be thread-safe, as the program's `header`.

`on_open` and `on_close` run on the bars that open and close each session, whatever the interval, by the market's session (`kr`: 09:00-15:30 KST; `us`: 09:30-16:00 at UTC-5, daylight saving not modelled):

| Data | `on_open` runs on | `on_close` runs on |
| --- | --- | --- |
| intraday bars (`1m` to `1h`) | the session's first bar, at its end; the program reads only its open | the first bar whose period holds the close: of start-stamped KRX minute bars the 15:29 bar, of end-stamped ones the bar stamped 15:30 |
| daily bars with `bars.open_point = true` | a point at the session's open: the day's open alone, stamped at the open | the day's bar, stamped at the close |

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bars.open_point` | boolean | `false` | each daily bar replayed as a point at the session's open and the day's bar at its close, on the market's date of its stamp (a midnight in any zone, or a session time); only for a program with `on_open` or `on_close`, and only on daily bars (wide tables need `bars.open`, `bars.high` and `bars.low`); the US session is a fixed UTC-5, daylight saving not modelled |

A program with `on_open` or `on_close` follows the market's session, stated by `replay.market` or inferred as for any replay, and on daily bars needs `bars.open_point = true`: a daily bar is a whole session, and `on_open`'s targets would be replaced by `on_close`'s on the same bar.
On US intraday bars, the market stated or inferred, it is refused until the session follows daylight saving (Korean bars in a CSV need `replay.market = "kr"`, since a CSV's market is `us` by default).
A replay in which such a program met no session's open or close fails, naming the market and the interval; the report gives `sessions_opened` and `sessions_closed`, and `caveats.open_points` when `bars.open_point` is set.
With `fill = "decision-close"`, a target stated in `on_open` fills at the open it saw and one in `on_close` at the close it saw; with `next-open`, at the next bar's open.
With `next-close`, at the next bar's close: with `bars.open_point`, a target stated in `on_open` fills at that day's close, and one in `on_close` at the next open point, whose close is the next day's open.
A fill is dated at the bar it fills against: an `on_close` exit at the next session's first bar, so the last session's exit does not fill.
With `bars.open_point` a day is two bars and two equity points, so set `replay.periods_per_year` to twice the trading days (504) for the annualized ratios.
A `DAY` order still resting at an open point (a rule that named no price there) expires with it, since a daily bar ends its session.

A run (`cmdatl run`) takes neither a program with `on_open` or `on_close`, nor `replay.fill` other than `next-open`, nor a market-price rule of your own, nor `bars.open_point`: a run's orders fill at the market.

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
The program's `header` is compiled with it: its path and its bytes are part of the build's key, so an edited header is built again, and CMake tracks the headers it includes.
A backtest's `measures`, and the names in the program's `measures {}`, are part of the key too, by name and order, so a changed list builds once.
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
| `caveats` | object | `gaps` (bars that did not follow their predecessor), `unfilled` (orders still queued at the end), `expired` (`DAY` orders withdrawn unfilled when their session ended, as they would expire live; on daily bars, after one bar), `unvalued_bars`, `multiple_instruments`, `cost_totals_incomplete`, `volume_ignored` (always true: fills are not capped by volume), `market_price_rule` and `market_fills_by_rule` (the rule named by `replay.fill_function` and the fills it priced, only when one is set), `open_points` (true with `bars.open_point`), `lowest_cash` (the lowest cash a fill left, as a decimal string, when below zero) and `shorts_borrow_nothing` (always true) |
| `first_bar`, `last_bar` | string | the first and last recorded times, ISO 8601 in UTC; absent when nothing was replayed |
| `measures` | object | each of the program's `measures {}`, then each of the run spec's `measures`, with its value, a number or null, in order; only when there is any |
| `warning` | string | only when `--fills` and `--equity` turned out to be one file, which then holds the equity curve |

A number is printed with ten significant digits; a figure the library leaves undefined (a return on zero equity, a ratio without variance) is null.

`--fills FILE` writes one line per fill, after the header `at,symbol,side,quantity,price,commission,tax`: the time (ISO 8601, UTC), the symbol, `buy` or `sell`, and decimals.

`--equity FILE` writes the equity curve the report's figures are computed from, one line per recorded point, after the header `at,equity,period_return`: the time (ISO 8601, UTC), the equity as a decimal, and the return since the previous point as a number printed as the report's.
The return is empty where the library leaves it undefined: on the first point, after a bar whose equity could not be valued, and after a point whose equity was zero or negative.
Both are written after the replay's own checks, so a replay those checks fail writes neither; the reply names each file by its absolute path (`fills`, `equity`).
Give `--fills` and `--equity` different files: the same file by its spelling or an existing link is refused before anything runs.
Two names that become one file only when created (a case variant on a case-insensitive volume, a dangling link) are not refused: the equity file then replaces the fills file, and the report's `warning` says so.

## Many replays

```sh
cmdatl strategy evaluate dip.toml grid.lookback=10,20,30
cmdatl strategy evaluate dip.toml grid.lookback.from=10 grid.lookback.to=60 grid.lookback.step=10 evaluate.objective=sharpe_annualized evaluate.jobs=4
```

`cmdatl strategy evaluate` reads its target and settings as `strategy backtest` does, builds the same driver (a backtest's build serves it), loads the bars once, and replays them once per parameter set of `[grid]`.
It takes no `--fills` or `--equity`, and does not fetch: a spec whose `bars.source` names an adapter folder is refused (fetch with `strategy backtest`, then evaluate on `bars.source = "store"`).
`strategy backtest` and `cmdatl run` refuse the keys below.
With `bars.source = "stdin"` it replays the paths a generator pipes in (`GENERATOR | cmdatl strategy evaluate spec.toml bars.source=stdin bars.path_column=path`; `[bars]` describes the stream); stdin may not be a terminal.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `evaluate.method` | `"grid"`, `"windows"`, `"walk-forward"` or `"bootstrap"` | `"grid"` | how the replays are chosen: a grid of parameter sets, one set over windows of the history ("Windows"), a set chosen on each training window and tested on the next ("Walk-forward"), or one set over bootstrap paths of the history ("Bootstrap paths") |
| `evaluate.jobs` | whole number, 1 to 256 | 1 | replays run at once, each on its own thread |
| `evaluate.objective` | `"total_return"`, `"sharpe_per_period"`, `"sharpe_annualized"`, `"sortino_per_period"`, `"max_drawdown"`, `"turnover"`, `"executions"`, or `"measures.NAME"` for one of the program's `measures {}` or the spec's `measures` | none | the report figure the reply's `best` compares; without it `best` is null |
| `evaluate.direction` | `"max"` or `"min"` | `"max"` | which end is best (`max_drawdown` is a positive fraction: use `min`) |
| `grid.NAME` | a list of numbers | none | the values of parameter NAME, in the order written |
| `grid.NAME.from`, `.to`, `.step` | numbers | none | the same as a range: `from`, `from + step`, ... up to `to` inclusive |

- NAME is one of the program's `int` or `real` parameters (a `bool` is refused), typed as `params.NAME` is; for a name also in `[params]`, the grid's values win.
- The command line wins as for any key: its `grid.NAME`, a list or a range, replaces the run spec's for that name; one layer giving both a list and a range of a name is refused.
- A range is computed in exact decimals (`from = 0.1, to = 0.3, step = 0.1` is exactly 0.1, 0.2, 0.3), at most 18 decimal places; `step` is above 0 and `from` at most `to`.
- A value listed twice is refused, also two values the replay reads as one number (`1` and `1.000000000000000001`, `0` and `-0`).
- Each parameter's values take at most 4,096 bytes, and the grid makes at most 100,000 parameter sets.
- The order: names in byte order, values in the order written, the last name varying fastest; replay `i` is set `i`.

Each replay is one JSON line, in index order, whatever `evaluate.jobs` is, with `event: "replay"`:

```json
{"env":"offline","event":"replay","index":4,"params":{"fast":10,"slow":60},"window":null,"path":null,"report":{...}}
```

- `report` is exactly the report `strategy backtest` gives for these settings and parameters ("The report"), its `measures` included; a measure that throws fails that replay only, and one that runs out of memory stops the evaluation.
- A replay without a report has `"report":null` and `failure`: `{"kind":"params","message":...}` when the program's bounds refused the set, `{"kind":"replay",...}` for any other error of that replay; the others go on.
- `window` is null in a grid; `windows` and `walk-forward` fill it ("Windows", "Walk-forward").
- `path` is null in a grid, or with `bars.source = "stdin"` (`[bars]`) the path's `number` (from 0, in the order read), `seed` (null) and `id` (its `bars.path_column` cell, or null): each path is replayed once per parameter set, replay `p * G + g` being set `g` of `G` on path `p`; `bootstrap` fills it with its own paths ("Bootstrap paths").

The final reply gives `method`, `jobs`, `param_sets`, `invalid` (sets the bounds refused), `tried` (`param_sets - invalid`), `replays` (the lines printed), `paths` (the paths read from stdin, else null), `failed` (replays that failed otherwise), `objective`, `direction`, and `best`: the index, parameters and value of the best replay with a report, not halted and with a defined figure, a tie going to the lower index (null with stdin paths, an aggregate this version does not make).
It exits 0 when at least one replay has a report; 1 (`replay.failed`) when every replay failed or was refused, or when the driver stopped (it ran out of memory, or the program threw something that is not a `std::exception`): the lines already printed stand, and the reply's `replays` counts them.

With `evaluate.jobs` above 1 the program's own C++ (its `header` and a market-price rule) runs on several threads at once and must be thread-safe; a race in it gives wrong numbers, not an error.
Each running replay holds its own engine and result, so memory grows with `evaluate.jobs`.

### Lengths: periods or calendar spans

A length is a whole number of **periods**, the market dates holding at least one bar of the replay (after `replay.from` and `replay.to`), or a string with a unit, a **calendar span**: `"10d"` (days), `"2w"` (weeks of seven days), `"6mo"` (months), `"3y"` (years of twelve months), 1 to 99999 units.

- A daily (weekly, monthly) bar's date is its market date, as `bars.open_point` dates it; an intraday bar's is the market-local date of its start, so a window never starts or ends inside a session; with `bars.open_point` a day is one period; a universe's symbols share the periods.
- Spans are arithmetic on those dates: months first, the day kept or clamped to the month's last (2024-01-31 plus `1mo` is 2024-02-29), then days.
- Every boundary is the anchor plus a sum of lengths, never one boundary plus another, so a clamp never drifts: from 2024-01-31 by `1mo`, the windows start on Jan-31, Feb-29, Mar-31, Apr-30, and each ends where the next starts.
- The anchor is the `replay.from` date when given, else the first period's date.
- A window is replayed only when it ends at most a day after `replay.to` (or the last period's date): whole windows only, and a holiday at the end does not drop the last window when `replay.to` says the history reaches it.
- A window of one span can hold more periods than another (a March against a February); each line's `window.periods` says how many.

### Windows

```sh
cmdatl strategy evaluate dip.toml evaluate.method=windows windows.length=252 windows.step=63
cmdatl strategy evaluate dip.toml evaluate.method=windows windows.length=1y windows.step=3mo windows.warmup=3mo
```

One parameter set, replayed on each window of the history.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `windows.scheme` | `"rolling"` or `"expanding"` | `"rolling"` | rolling: windows of `length` moving by `step`; expanding: every window starts at the first and its end moves by `step` |
| `windows.length` | periods (2 or more) or a span | required | the length traded (the first window's when expanding) |
| `windows.step` | periods (1 or more) or a span | `length` | between window starts (rolling) or ends (expanding) |
| `windows.warmup` | periods (0 or more) or a span | on daily or coarser bars the program's warm-up in periods; on intraday bars required | the history fed before each window without trading |
| `windows.summary` | a list of statistic names: `mean`, `min`, `max`, `median`, `q00` to `q100` | none | the statistics the reply gives of each figure over the windows |

- `windows.length` and `windows.step` are both periods or both spans; a span warm-up goes with span lengths only. A warm-up in periods with span lengths starts the first window at the date of that period.
- The warm-up is history: every handler runs and series and states warm up, but what the program asks for is dropped (not counted as rejected), nothing fills and no equity point is recorded; a target stated during the warm-up is not held at the window's start, and the program trades on its next statement, with the `state` the warm-up left. Session and pricing counts in the report are the window's own.
- A warm-up that holds fewer bars than the program's warm-up is refused, naming the window; so is a window of fewer than 2 periods, and a history no window fits. A warm-up of 0 replays the window as a backtest of its dates would.
- `[data.NAME]` values known before a window's trading start are fed as history; values after its last bar are not fed.
- A `[grid]`, and `bars.source = "stdin"`, are refused: windows replay one set on loaded bars.
- Each line's `window` is `{"fold": k, "role": "window", "warm_from": ..., "from": ..., "to": ..., "periods": N}` (instants of the first and last bars, `warm_from` null without a warm-up); replay `k` is window `k`.
- The reply adds `scheme`, `length`, `step`, `warmup` (as written, or the default used), `windows`, `uncovered_from` (the date of the first period after the last window, or null) and `halted`.
- With `windows.summary`, it adds `summary`: for `total_return`, `sharpe_annualized`, `sortino_per_period`, `max_drawdown`, `turnover`, `executions` and each measure (under `measures`), the replays with a report that did not halt give `defined`, `null` (undefined values, not used), then the statistics named, in their order.
  `qNN` is the NN percent quantile, written in two digits (`q05`, `q95`) or `q100`; `min`, `median` and `max` are `q00`, `q50` and `q100`.
  A name of another form, or one named twice, is refused (`spec.invalid`).
- Without it there is no `summary`: the lines, one per window in order, are the series of every figure and measure, for analyses of your own.
- Quantiles are Hyndman and Fan's type 7 (numpy's default): of `m` sorted values, `h = (m - 1) p`, `j = floor(h)`, then `x[j] + (h - j)(x[j + 1] - x[j])`, or `x[j]` at the last; with no value every statistic is null.
- Windows overlap or grow, so they are not chained into one curve.

### Walk-forward

```sh
cmdatl strategy evaluate dip.toml evaluate.method=walk-forward walk_forward.train=504 walk_forward.test=126 grid.lookback=10,20,30 evaluate.objective=sharpe_annualized
cmdatl strategy evaluate dip.toml evaluate.method=walk-forward walk_forward.train=3y walk_forward.test=1y grid.lookback=10,20,30 evaluate.objective=total_return
```

On each fold, every parameter set of `[grid]` is replayed on the training window, the best by `evaluate.objective` is replayed on the test window that follows, and the tests are chained into one report.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `walk_forward.scheme` | `"rolling"` or `"expanding"` | `"rolling"` | rolling: each training window is the last `train` before its test; expanding: every training window starts at the first period |
| `walk_forward.train` | periods (2 or more) or a span | required | the training length (the first fold's when expanding) |
| `walk_forward.test` | periods (2 or more) or a span | required | each test's length; the folds advance by `test`, so the tests tile |

- `walk_forward.train` and `walk_forward.test` are both periods or both spans.
- With spans ("Lengths"), fold `k` tests from the anchor plus `train` and `k` times `test` to the anchor plus `train` and `k + 1` times `test`; a rolling training window starts at the anchor plus `k` times `test`.
- A `[grid]` of two parameter sets or more and `evaluate.objective` are required; `evaluate.direction` is as above.
- `bars.source = "stdin"` is refused: walk-forward replays loaded bars.
- There is no step: another step would leave gaps or overlaps between the tests, and the chain would not be one strategy's history.
- Training: each set on each training window, from its first bar with no history (the program's warm-up spends the window's first bars, as a backtest of those dates would); with `S` sets, replay `k * S + g` is set `g` on fold `k`.
- The choice: per fold, the set with the best objective among the replays with a report, not halted and with a defined value, a tie going to the lower set; a fold with no such set has `chosen: null` and no test.
- Testing: the chosen set on the test window, its training window fed first as a warm-up (as in "Windows"); with `F` folds, the tests are replays `F * S` on, one per fold with a choice, in fold order.
- A training window holding fewer bars than a set's warm-up plus 2, or a test of fewer than 2 periods, is refused before any replay, naming the set and the fold; so is a history no fold fits.
- Each line's `window` is `{"fold": k, "role": "train" or "test", "warm_from": ..., "from": ..., "to": ..., "periods": N}`.
- The chain: each test starts flat with `replay.cash`, and positions open at its end are valued at its last close, not sold (no exit cost).
- Each test's curve is scaled to start where the chain stands, so `chained.total_return` is the product of the tests' `1 + total_return`, minus 1, and the period across a fold boundary has no return.
- Sharpe, Sortino and the drawdown are over the chained curve, turnover over the chain's opening equity; costs, counts and executions are summed, and measures are computed on the chain.
- `chained` is null, and `chained_null_reason` says why, when a fold had no choice (`"unchosen"`), a test failed or halted (`"failed"`, `"halted"`), a test recorded no equity point (`"empty"`), a test's equity is zero or below where the next fold would follow it (`"insolvent"`), or the chain could not be made or measured (`"chain_unavailable"`, with the message in `warnings`).
- The reply's `best` is null (the choices are per fold); `param_sets`, `invalid` and `tried` count the grid's sets, and `failed` the training and test replays that failed.
- It adds `scheme`, `train` and `test` (as written), `train_replays` (the folds times `tried`, the trials a selection-bias correction counts), `uncovered_from` (the date of the first period after the last test, or null), `warnings`, `folds`, `chained` and `chained_null_reason`.
- Each `folds` entry gives `fold`, `train_from`, `train_to`, `test_from`, `test_to` (the first and last bars' instants), `train_periods`, `test_periods`, `eligible` (the sets the choice could take), `chosen` (the grid set, or null), `params`, `objective_value`, `test_index`, `test_total_return`, `test_max_drawdown` and `test_halted`.
- No benchmark is computed.

### Bootstrap paths

```sh
cmdatl strategy evaluate dip.toml evaluate.method=bootstrap bootstrap.block=10 bootstrap.paths=500 'bootstrap.intervals=["total_return", "max_drawdown"]'
```

One parameter set, replayed on the history (path 0) and on `paths` block-bootstrap paths of it.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `bootstrap.scheme` | `"circular"`, `"moving"` or `"stationary"` | `"circular"` | circular blocks wrap from the history's end to its start; moving blocks stay inside it; stationary blocks have geometric lengths of mean `block` and wrap |
| `bootstrap.join` | `"returns"`, `"differences"` or `"levels"` | `"returns"` | how drawn blocks are joined: chained ratios; chained changes; the drawn prices as they are (for a stationary series: trending prices jump at each block boundary) |
| `bootstrap.block` | whole number, 1 or more | required | the block length in periods (the mean length when stationary); at most the history's returns for circular and moving |
| `bootstrap.paths` | whole number, 1 to 10000 | 100 | the paths drawn |
| `bootstrap.seed` | whole number, 0 to 2^53 - 1 | drawn, and echoed | the seed |
| `bootstrap.intervals` | a list of the names `evaluate.objective` takes | none | the metrics given an interval over the paths |
| `bootstrap.level` | a number above 0 and below 1 | 0.95 | the intervals' coverage |

- A path redraws the history's days in blocks, the same days for every symbol, and rebuilds each drawn day's whole bar joined to the path's previous close (by ratios, by changes, or the bar as it is), on the symbol's price grid; every path has the history's dates. An ordered bar stays ordered (its low at most its open and close, its high at least both). Path 0 is the history itself, so its report is `strategy backtest`'s.
- Path `p` depends only on the seed, `p`, the scheme, the block, the join and the history: not on `paths` or `evaluate.jobs`; the reply's `seed` given back reproduces every path.
- The input: daily bars, every symbol a bar on every date, from a CSV, a store or wide tables, with `bars.open_point` for a program with `on_open` or `on_close` (each open point takes the path's rebuilt open). Refused: `bars.source = "stdin"`, a program with `data {}`, and a `[grid]`. Allowed: `replay.fill`, `replay.fill_header`, limit orders (filled against the path's high and low), shorts, costs, `replay.from` and `replay.to` (the history drawn from), measures.
- Each path bar takes the drawn day's volume, which the replay never caps fills by; a table of closes, and wide tables with `bars.open`, `bars.high` and `bars.low` but no `bars.open_point` (which load as flat bars, as their backtest replays them), still replay flat bars (open, high and low are the close).
- On whole bars `next-open` fills at the path's open: under `returns` and `differences` the drawn day's overnight gap enters fills; under `levels` the open is the drawn day's own, so at a block boundary it jumps from the path's previous close. On flat bars both next-bar rules fill at the next close.
- `levels` suits a series that is already stationary: on trending prices each block boundary is a jump. Each path keeps the history's first bar and draws from the second on, so a circular block wraps from the last bar to the second.
- A history with a price at or below zero is refused before any replay: wide tables by the evaluation (`evaluate.refused`), and a CSV or a store already when loading (`replay.failed`, "prices must be positive"). A path whose rebuilt prices reach zero or below, lows included, fails its line before it is replayed (counted in `failed` and in each interval's `null`), whatever the join (a falling path under `differences` can reach it), so the intervals describe the paths whose every price stayed above zero.
- A drawn bar carries its slot's timestamp and stamp (the history's date at that place), the drawn day's rebuilt open, high, low and close, the drawn day's volume, and no traded value; the first bar of each path is the history's, rounded to the price grid (the same unless the history lies off a stated grid), and every bar of path 0 is the history's own. A market-price rule of your own (`replay.fill_function`) reads these bars as they are.
- `warnings` holds `{"code": "block_below_lookback", "block": b, "lookback": w}` when the block is shorter than the program's warm-up: an indicator longer than a block reads joined, unrelated days; and `{"code": "fill_rule_on_paths"}` when a market-price rule of your own prices the paths' market orders. Neither refuses.
- Each line's `path` is `{"number": p, "seed": S, "id": null, "scheme": ..., "block": b, "join": ...}`; replay `p` is path `p`.
- The reply adds `paths` (the paths replayed, path 0 included), `scheme`, `block`, `join`, `seed`, `history` (path 0's `total_return`, `sharpe_annualized`, `sortino_per_period`, `max_drawdown`, `turnover`, `executions` and measures), `warnings`, `halted`, and `intervals`: for each name of `bootstrap.intervals`, over paths 1 to N, `{"level", "lower", "median", "upper", "defined", "null"}`, the type-7 quantiles at `(1 - level) / 2`, 0.5 and `(1 + level) / 2` of the paths where the metric is defined; a failed, halted or undefined path counts as null, so `defined + null` is N.

A refusal of the settings before any replay (no window or fold fits, a warm-up too short, a missing bar, a price at or below zero on a bootstrap, a block above the returns) is exit 2, `evaluate.refused`, with a `next` naming the key to change.

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
