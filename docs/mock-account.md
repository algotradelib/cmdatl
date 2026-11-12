# The mock account

A mock account trades stored daily bars, not a broker: nothing reaches a network or a real account.
The operator describes it in a TOML file named by `CMDATL_CONFIG`, never by an argument; one account per file.

```toml
atlc = "/opt/atlc/bin/atlc" # optional: atlc, between CMDATL_ATLC and PATH

[account]
broker = "mock" # or "kis", for cmdatl run on a KIS account (runs.md)
number = "mock-0001" # names the account's state directory

[limits]
symbols = ["005930", "000660"]
position = { "005930" = 100, "000660" = 20 }
order_value = 5000000 # KRW, one order
daily_orders = 3 # placements per KST date
collar = 0.05 # optional, at most 0.30
phases = ["open"] # optional: pre_open, open, post_close

[mock]
bars = "bars.csv" # the columns symbol,date,open,high,low,close,volume in any order, beside this file
cash = 100000000
start = "2024-03-25T09:30" # KST, at or after the first date's close
```

- Every key is checked: an unknown key, a value of another type or out of range is refused with the file, the line and the column (`config.invalid`); `cmdatl env` shows what was read.
- Optional keys, with their defaults:
  - `atlc`: none (`CMDATL_ATLC`, then `PATH`).
  - `account.env`: `"mock"`, the only value with the mock broker.
  - `limits.order_type`: `"limit"`, the only order type.
  - `limits.collar`: 0.05, how far a limit may sit from the last price, as a fraction (above 0, at most 0.30).
  - `limits.phases`: `["open"]`.
  - `limits.plan_ttl`: 60, the seconds a plan id stays good (1 to 3600).
  - `limits.portfolio_age`: 30, how old, in seconds, holdings and the order list may be when an order is decided (1 to 86400).
  - `mock.listing_lag`, `mock.holdings_lag`: 0, the seconds an accepted order stays out of the order list and a fill out of holdings, as a broker's may (up to 86400).
- The account's state is tied to the bars file's bytes, `cash`, `start` and the two lags: changing any of them on a used account is refused (exit 4, not this account's).
  To start an account again, move its state directory aside.
- The armed file is `<stem>.armed` beside the operator's file (`mock.toml` gives `mock.armed`); while it is missing nothing may be placed, and `orders_accepted_now` says so.
- The account's state lives in `$XDG_STATE_HOME/cmdatl/accounts/<broker>-<env>-<number>` (else `~/.local/state/cmdatl`): its lock, its order journal and `paper.jsonl`, the mock market's log.
  Another `XDG_STATE_HOME` is another mock account.
- Keep the operator's file out of the state directory.
  cmdatl applies the operator's file to every command, but the agent runs as the operator's own user: cmdatl cannot stop a process of that user from changing the operator's file, the armed file, the release approval or the account's state directory.
- The commands on this page and in [Orders](orders.md) use the sample file's symbols; on the example account, use `AAA` and `BBB`.

```sh
cmdatl status # time, phase, readiness, positions, cash, owed and working orders, quotes
cmdatl quote 005930 000660 # last price, band, and whether an order would pass the first gates now
cmdatl account --symbol 005930 # positions, cash, buying power, owed orders, orders left today
cmdatl order list # working orders
cmdatl clock advance --next-open # the mock clock moves only when told; resting orders fill or expire
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
