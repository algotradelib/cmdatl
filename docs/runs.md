# Runs

A run trades a strategy through algotradelib's live runner, the code that would trade a broker's account, on a mock market built from the run spec's data, or on a [KIS account](#runs-on-a-kis-account) the operator's file names.

```sh
cmdatl run dip.toml # builds the program once, then runs it in the foreground: events as JSON lines, the reply last
cmdatl run dip.toml params.lookback=30 > run.jsonl # the same, into a file
```

- A run's market is the library's mock broker, inside the run's own process: each run is a fresh market with its own journal, and nothing is shared with the [mock account](mock-account.md) or with another run.
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

`cmdatl run` trades a KIS account when the operator's file names one; of the account and order commands, `status`, `account`, `quote` and `order list` read it (see [Reads on a KIS account](#reads-on-a-kis-account)), and the others answer `unsupported.broker`.
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
  The calendar KIS gave that day in Seoul is kept in `$XDG_CACHE_HOME/cmdatl/calendars` (else `~/.cache/cmdatl/calendars`), so a later run or read that day makes no holiday inquiry of its own; two that start together can each make one.
- A read before the start that KIS refuses for its request rate is read again twice at most, a few seconds apart.
  A broker that asks for more than 61 seconds before the next read is not waited for: the run fails at once, and `next` names the wait.
  When KIS refuses the run's access token because it issues one a minute, the run waits 61 seconds once for each login and reads again.
  Each wait is written to stderr when it begins, and Ctrl-C during it stops the run with nothing sent (exit 130, `interrupted`).
  When the reads still fail, the error's `next` names the wait.
- Every run on a key shares its access token through a file (see [Broker logins](logins.md)), so a second start within a minute takes no token: it uses the one the first took.
  When that file's folder cannot be used, the run refuses before anything is sent, and the error gives the reason; its `next` names `chmod` when the folder or a file in it is not private.
- The run holds the account for its life and keeps the account's journal across runs, so the next run sees what an earlier one could not confirm and today's `daily_orders` count the earlier runs' orders.
- A run started after one that died (a kill, a crash, a sleeping Mac) settles what the journal left owed on its own symbols before it places anything there.
  The `start` event's `owed_at_start` lists those orders (`symbol`, `side`, `quantity`, `journal_id`, `order_id`, empty while the broker has not named it, and `state`).
  One still working is withdrawn at the stop; one filled or cancelled is confirmed from the holdings and the order history.
  One the broker never shows, or one the journal holds for the operator (`state: operator`), stays owed: the run places nothing on that symbol and ends with exit 4.
- An order owed on a symbol the run does not trade refuses the start (`unknown.owed_at_start`); a run on that symbol settles it.
- It starts only with no order resting on the account but an earlier run's own: one the journal shows sent and KIS accepted today, with the same number, symbol, side, quantity and price.
  Any other refuses the start before anything is sent (`refused.orders_resting`), since the stop's withdraw cancels every working order on the account, other symbols' included; nobody else trades the account while it runs.
- With today's `daily_orders` spent, a run that inherits owed orders still starts, to settle them, and places nothing (`orders_left_today` is 0).
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
  A signal is taken when the broker request in flight answers: the `stop` event follows it, and the withdraw starts once the iteration's remaining reads are done.
  Removing the armed file stops placing and leaves what rests.
- On macOS a sleeping Mac stops the run with its orders resting: start it with `caffeinate -i cmdatl run probe.toml > run.jsonl`, or from a launchd job.
- The reply's `holdings_at_end` is the account's holdings of the run's symbols, read once more at the end; a read KIS refuses for its request rate is read again twice at most, a few seconds apart.
  When it still fails, each symbol is `null` and `holdings_at_end_error` names why: KIS's code (`EGW00201`, the per-second limit), else the error's kind (`transport`); it is `null` when the holdings were read.
- An unconfirmed ending (exit 4: an order not confirmed, the journal or the lock damaged, the broker unreadable at the end) needs the account read in KIS's app before it is traded again; `owed_at_end` lists the orders the journal still waits on.

### Reads on a KIS account

On the KIS account the operator's file names, `status`, `account`, `quote` and `order list` read the account with the same logins a run reads, on the same code path for demo and real; they place, cancel and change nothing.

- The other order commands and `clock` answer `unsupported.broker` (exit 1, nothing read or sent) before any login is read.
- `cash` and `data_end` are `null`: the broker reports holdings and buying power, not a cash figure, and its data has no end.
- `now` is the instant the reply is as of, taken after the calendar is read and before the account's figures, which are later by the seconds the reads take; the phase, `orders_left_today` and every quote row's acceptance are at that instant.
- The phase is the library's KRX window, 09:00 to 15:30 KST, on the calendar's trading days; the calendar gives days, not hours or halts, so a special session shows as an ordinary one, and `orders_accepted_now` stays the preliminary gates.
- A symbol the broker does not have shows as `no_price` in its row, not `quote.no_data`.
- An owed row whose order the broker does not list now may have filled, been canceled or never been accepted: its `next` sends the operator to the broker's app.
- A holdings, buying-power or order-list read the broker does not answer fails the command with `account.unreadable` (exit 1, `retry: true`), naming the broker's code and never its text.
- They take the account's lock as a run does: while a run holds it (its owner record names the run) they answer `busy.locked` at once; otherwise they wait up to 10 seconds.
- Ctrl-C stops them before the next broker request (exit 130, `interrupted`); a request in flight is not cut short, and can take up to its 30-second timeout.
- They share the run's access token, and the trading calendar read that day, through files in `$XDG_CACHE_HOME/cmdatl` (else `~/.cache/cmdatl`).
