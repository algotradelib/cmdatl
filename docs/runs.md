# Runs

A run trades a strategy through algotradelib's live runner, the code that would trade a broker's account, on a mock market built from the run spec's data.

```sh
cmdatl run dip.toml # builds the program once, then runs it in the foreground: events as JSON lines, the reply last
cmdatl run dip.toml params.lookback=30 > run.jsonl # the same, into a file
```

- A run's market is the library's mock broker, inside the run's own process: each run is a fresh market with its own journal, and nothing is shared with the [paper account](paper-account.md) or with another run.
  The agent commands do not see a run's orders, and a run's holdings at its end are a result, not an account anything can trade.
- The run's settings describe the market, read exactly as `cmdatl strategy backtest` reads them: the bars, `[replay] cash`, the costs, the instruments.
  The operator's file describes what may be placed: `limits.symbols`, the position caps (a run with several symbols gets the smallest of their caps), `order_value`, `daily_orders` (per run, per 24 hours of market time), `order_type`, `collar` and `phases`; no setting loosens them.
- A run needs `[replay] market = "kr"`, daily bars from a file (not a store), `[orders] type = "limit"` with an `offset` within `limits.collar`, symbols in `limits.symbols`, and the armed file: each missing one is refused before anything is built.
  It also needs a program without `on_fill`, a warm-up of at most 10,000 bars, and daily bars: those are refused by the run itself, after the program is built and before anything is placed.
  A program with `on_open` or `on_close` is refused too: `run` learns it from `atlc check --info` and answers `program.live_unsupported` before anything is built.
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

### Runs on a KIS account

`cmdatl run` trades a KIS account when the operator's file names one; the account and order commands stay on the paper account.
A demo account (모의투자) is the default, and the code is the same for a real one: only `env` differs.

```toml
[account]
broker = "kis"
env = "demo" # the default; "real" only when written out
# number = "12345678-01" # optional: the login names the account; written, it must match the login
```

```sh
cmdatl auth login --broker kis --env demo # the account's login
cmdatl auth login --broker kis --env real # on a demo account, read for market data only
cmdatl run probe.toml > run.jsonl # in the foreground: tmux, nohup or a launchd job keep it running
```

- The run is real time: it reads the account on the wall clock during the market's hours, as a person's program would, and lasts until it is stopped.
- A KIS demo host refuses reference data and the holiday calendar, so a demo run reads market data with the real login, through a session with no account, which cannot place an order.
  Without a real login a demo run refuses to start.
- The account is the login's (one login per broker and environment); a `number` in `[account]` pins it, and a login for another account is then refused before anything connects.
- The logins are read from the keychain by `cmdatl` and handed to the run on a pipe, never on its command line or in its environment.
- The run spec names the symbols in `[instruments]` and takes no `[bars]`: the bars come from the broker, daily, with one more than the program's warm-up.
- The run reads KRX's prices, not KRX and NXT combined: a daily bar closes at KRX's 15:30 close, as KRX-close histories (Yahoo's, for one) do, and NXT's trades before 09:00 and after 15:30 are in neither its bars nor its quotes.
  Orders go where they always did; the `start` event's `market_data` names the tape (`KRX`).
- KIS has once restated the previous day's bar between midnight and 06:00 KST, and a run takes each day's bar at its first read after midnight and keeps it: start a run after 06:00 KST, and expect a run kept overnight to decide on the bar as first read.
- It reads the trading calendar before it starts and refuses when it cannot (nothing is sent).
- A read before the start that KIS refuses for its request rate is read again twice at most, a few seconds apart.
  A broker that asks for more than 61 seconds before the next read is not waited for: the run fails at once, and `next` names the wait.
  When KIS refuses the run's access token because it issues one a minute, the run waits 61 seconds once for each login and reads again.
  Each wait is written to stderr when it begins, and Ctrl-C during it stops the run with nothing sent (exit 130, `interrupted`).
  When the reads still fail, the error's `next` names the wait.
- A run keeps no token, so a command or another run started right after a run can still meet KIS's one-token-a-minute refusal.
- The run holds the account for its life and keeps the account's journal across runs, so the next run sees what an earlier one could not confirm and today's `daily_orders` count the earlier runs' orders.
- It starts only with no order resting anywhere on the account, since the stop's withdraw cancels every working order on it, other symbols' included; nobody else trades the account while it runs.
- The account's holdings of the run's symbols are the run's position from its first decision.
- The run may sell every sellable share of its symbols, held before the start or bought after it, whoever bought it; the start line's `holdings_at_start` shows what it found, not a limit.
- A run places orders only for its own symbols, and leaves other symbols' holdings alone.
- The run's cap is the smallest `position` cap among its symbols, and it counts every share held: the guard refuses a buy over it and a sell that leaves more than it, and never splits an order.
  So a holding above the cap leaves only by one order that brings it within the cap and fits `order_value`; when none fits, the operator changes the limits or sells in KIS's app.
- Shares a run leaves at its end stay on the account, and the next run on that symbol starts with them.
- A program's state is computed again at every start, not restored: the history bars read at the start go through its handlers, and on each of them `held` is the account's holding now.
  So a `state` set from `held` is rebuilt from today's holding.
  This program, restarted holding shares, takes the first history bar's close as `entry`, and sells them at once if today's close is 5 % below it:

  ```atls
  state {
    real entry = 0;
  }
  on_bar {
    if (held > 0 and entry == 0) {
      entry = close;
    }
    if (held > 0 and close < entry * 0.95) {
      target(0);
    }
  }
  ```
- Every 60 seconds it writes a `heartbeat` with the holdings of its symbols, the cash and the working orders as the run last read them, each with its `read_at` (a failed read leaves the old time showing), and the phase computed now from the calendar.
- Ctrl-C, SIGTERM, SIGHUP or a closed output withdraw, as above; the end waits up to a minute for the broker to confirm, longer when a request is in flight.
  Removing the armed file stops placing and leaves what rests.
- On macOS a sleeping Mac stops the run with its orders resting: start it with `caffeinate -i cmdatl run probe.toml > run.jsonl`, or from a launchd job.
- An unconfirmed ending (exit 4: an order not confirmed, the journal or the lock damaged, the broker unreadable at the end) needs the account read in KIS's app before it is traded again; `owed_at_end` lists the orders the journal still waits on.
