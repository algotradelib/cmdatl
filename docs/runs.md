# Runs

A run trades a strategy through algotradelib's live runner, the code that would trade a broker's account, on a mock market built from the run spec's data, or on a [KIS account](#runs-on-a-kis-account) the operator's file names.
The account and order commands also work on an [Alpaca account](#an-alpaca-account), where a run and a buy need a [run budget](#a-run-budget).

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
  `cmdatl cache clean` clears the builds no other cmdatl is building, and the next run builds again; after a build was killed with SIGKILL, let its compiler end before cleaning.

### Runs on a KIS account

`cmdatl run` trades a KIS account when the operator's file names one; every account and order command but `clock` and `clock advance` works on it too, and of those only `order place` and `order cancel` send (see [Commands on a KIS account](#commands-on-a-kis-account)).
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
- It starts only with no order resting on the account but the journal's own: one the journal shows sent, by an earlier run or an order command, and KIS accepted today, with the same number, symbol, side, quantity and price; the run takes it as its own, and its stop withdraws it.
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

### Commands on a KIS account

On the KIS account the operator's file names, `status`, `account`, `quote`, `order list`, `order plan`, `order place`, `order cancel`, `order reconcile` and `order release` work with the same logins a run reads, on the same code path for demo and real (only `env` differs); only `order place` and `order cancel` send.

- On `env = "real"` they are real orders with real money: the operator's file bounds them, and for a placement the armed file.
- `clock` and `clock advance` answer `unsupported.broker` (exit 1, nothing read or sent) before any login is read: a KIS account's time is the market's own.
- `cash` and `data_end` are `null`: the broker reports holdings and buying power, not a cash figure, and its data has no end.
- `now` is the instant the reply is as of, taken after the calendar is read and before the account's figures, which are later by the seconds the reads take; the phase, `orders_left_today` and every quote row's acceptance are at that instant.
- The phase is the library's KRX window, 09:00 to 15:30 KST, on the calendar's trading days; the calendar gives days, not hours or halts, so a special session shows as an ordinary one, and `orders_accepted_now` stays the preliminary gates.
- A symbol the broker does not have shows as `no_price` in its row, not `quote.no_data`.
- `order plan` makes the checks it makes on the mock account and gives a plan id; the plan is issued, and lapses after `limits.plan_ttl` seconds, on the computer's clock, and `events` is `null`: no simulated second passes.
- `order place` sends the plan to KIS through the final checks a run's orders pass: the armed file, the lock, a limit order inside `limits.collar` of the last price, and a permitted phase; `order cancel` withdraws an order this account owns (below), with no armed file and in any phase.
- The phase, the calendar and the clock are observations made before the request leaves: a token request, KIS's pace and the network come after, so an order can reach KIS after the phase it was checked in, and a plan's life bounds when cmdatl admits it, not when KIS receives it.
- Exit 3 from `order place` or `order cancel` says that call sent no order and no cancellation, not that nothing is owed; exit 4 is answered by `cmdatl order reconcile SYMBOL`, never by planning or placing again.
  KIS's refusal of an order that reached it is exit 4 `unknown.broker_refused`, as on the mock account.
- Reference data, quotes, price bands and the calendar come from the market-data session (on a demo account, the real login's); holdings, buying power, the order list, the order history, placements and cancellations from the account's.
- `order reconcile` is the library's recovery: it records what the order list, the dated order history and holdings prove, and sends nothing.
  KIS's list holds open orders only, so an order missing from it is never read as filled or canceled; on a demo account an order just placed can be missing from it for a while.
- `order release` settles, on the armed file and the operator's approval, the rows of a symbol that the broker never named or that are held for the operator; it refuses a named row whatever its age (`refused.named`).
  No command clears a named row that neither holdings nor history ever accounts for: after ten minutes it is `unknown.unconfirmed` and holds its symbol, for the operator to read in KIS's app.
- The first command that may write the journal starts it, under the account's lock; a read never does.
- A working order is this account's only when the journal shows it sent and KIS acknowledged it today, with the same number, symbol, side, quantity and price, as for a run; any other order on the symbol answers `refused.unjournaled_order` (exit 3), and withdrawing it is the operator's, in KIS's app.
  So an order whose acknowledgement was lost can be read and reconciled, never withdrawn by cmdatl.
- An owed row says what to do next: one KIS lists can be withdrawn by `cmdatl order cancel SYMBOL` only when this account placed it and KIS acknowledged it, else `refused.unjournaled_order` leaves it to the operator, in KIS's app; one it does not list may have filled, been canceled or not be listed yet, and `cmdatl order reconcile SYMBOL` records it once holdings or history show it.
- A holdings, buying-power or order-list read the broker does not answer fails a read with `account.unreadable` (exit 1, `retry: true`), naming the broker's code and never its text; once an order command has its result, a failed read of the owed rows after it leaves `owed` `null` and the result as it is.
- They take the account's lock as a run does: while a run holds it (its owner record names the run) they answer `busy.locked` at once; otherwise they wait up to 10 seconds.
- A command holds the lock while it talks to KIS: a few seconds at one request a second, and up to about twenty more for the day's first command, which asks KIS for the calendar (up to 20 pages).
  A run waits up to 10 seconds for a command that holds the account, then ends with `busy.locked` (exit 1) and nothing sent: issue no commands around a run's start, and start the run again if it happens.
- Ctrl-C stops a read before its next broker request, and an order command before its decision starts (exit 130, `interrupted`); once an order command has its result, a stop leaves it as it is, with `owed` `null`; a request in flight is not cut short: each of its HTTP stages may take 30 seconds, a read is tried again once, an order list may take several pages, and the shared token's lock may be waited for 30 seconds.
- They share the run's access token, and the trading calendar read that day, through files in `$XDG_CACHE_HOME/cmdatl` (else `~/.cache/cmdatl`).
- Naming a KIS account for them, the operator accepts, as cmdatl cannot enforce it: one writer per account (no other app or machine trades it while the journal owes anything), and that matching by attributes cannot prove an identical order placed by hand at the same time was not cmdatl's.
- Every command and run on the account uses one state directory, on every day and even when nothing is owed: the lock, the journal, today's count and the plans live in it.
  Its root is `$XDG_STATE_HOME/cmdatl` when that variable names an absolute directory, else `$HOME/.local/state/cmdatl`, and `cmdatl status` shows the account's directory as `state_dir`.
  A command or run started with another value is a second writer on the same account: it needs no new armed file, since that sits beside the operator's file, and it takes its own lock and starts with an empty journal, no orders counted today and no plans.
  Each such directory has the whole of `limits.daily_orders`.
  Between a fill and the holdings showing it, or between KIS's acknowledgement and its list showing the order, a second directory can place the same order again: a target of 100 under a `limits.position` of 100 can end with 200 held, more directories can go further, and only KIS's own checks of buying power and sellable quantity bound it.
  Before trading from a new terminal, service or agent harness, compare `state_dir` in `cmdatl status` with the directory the account has used so far.
  These variables are the operator's to set and to keep the same everywhere, never an agent's.

### An Alpaca account

`[account] broker = "alpaca"` names an Alpaca account: its paper trading is `env = "demo"` (the default), and `env = "real"` is Alpaca's live host, on the same code path.
Live trading is supported; the library's adapter has been checked on paper only.

```toml
[account]
broker = "alpaca"
env = "demo" # Alpaca's paper trading; "real" is the live host
# number = "PA0000000000" # optional: the login names the account; written, it must match the login

[limits]
symbols = ["SPY"]
position = { SPY = 2 }
order_value = "1000" # in USD, as every amount on this account
daily_orders = 3 # per US trading day, counted from 00:00 at UTC-5
```

```sh
cmdatl auth login --broker alpaca --env demo # the paper keys: the API key ID and the secret key
```

- Alpaca states no buying power (its account is a margin account), so without a [run budget](#a-run-budget) cmdatl does not buy on it: `cmdatl run` is refused before anything is read (`config.invalid`, exit 1), and an `order plan` or `order place` that would buy is refused (`refused.no_buying_power`, exit 3, nothing sent).
  With `limits.budget` the run and the buys go ahead within the run's cash. Sells, `order cancel`, `order reconcile`, `order release`, `status`, `account`, `quote` and `order list` work as on a KIS account (see [Commands on a KIS account](#commands-on-a-kis-account)).
- `cmdatl auth login --broker alpaca` asks Alpaca which account the keys trade (one read, which changes nothing) and stores its number with the login, as KIS's login carries its own; a paper key given for `--env real`, or the reverse, or keys Alpaca refuses, are refused `auth.account_unread` and nothing is stored. `auth status` shows the number masked.
- At every start the number is the login's; a `number` written in `[account]` must equal it, or the start is refused `auth.account_mismatch`; Alpaca is asked once more (one listing, before the account's directory is made) that the keys still trade that account.
  A login stored before cmdatl learned the number is refused `auth.account_mismatch` with the `auth login` to run again.
- The phase comes from Alpaca's stated session hours, so `open` is exactly Alpaca's regular session, daylight saving and early closes included; times in replies stay KST with their offset.
- `buying_power` and `cash` are `null`.
- An order cmdatl lets through is also refused by Alpaca's adapter while Alpaca's clock does not say the regular session (Alpaca's `market_closed`: nothing sent, exit 3 `refused.withheld`); a sell takes three reads (the asset, the position, the clock) before it is sent, at one request a second per host, so a few seconds.

### A run budget

`limits.budget` in the operator's file is a run's starting cash, in the account's currency: buys spend it, sells return their proceeds, and the run never borrows; a run sells only the shares it bought.
The library keeps the run's cash and shares in the account's journal (algotradelib's run budget).

```toml
[limits]
budget = "5000" # the run's starting cash, written as order_value is
fees = { commission_rate = 0, commission_minimum = 0 } # required with budget; Alpaca charges none
# fees = { commission_rate = "0.00015", commission_minimum = 0, sell_tax_rate = "0.0018" } # a Korean broker's, as its fee page states

[[limits.transfer]] # optional: cash added to the current run, or taken from it with withdrawal = true, once by its id
id = "2026-11-22-a"
amount = "1000"
```

- `[limits.fees]` takes `commission_rate` and `commission_minimum` (required, 0 where the broker charges none), `institution_rate` and `sell_tax_rate` (optional); rates are fractions (`"0.00015"` is 0.015 %), written as strings.
  A fee the broker states on a fill is booked as stated; otherwise it is estimated from these, rounded up.
- `cmdatl run` opens a new run: the old run's open orders are cancelled first and booked to it (`--wait-open-orders` waits for them instead), then the new run starts with `limits.budget` and no shares.
  `cmdatl run --resume` takes up the account's current run, its cash and shares, and applies the `[[limits.transfer]]` entries it has not applied yet; a new run refuses a file that lists transfers.
- The order commands always take up the account's current run; with no run on the account they answer `refused.no_run` (exit 3, nothing sent); they never apply `[[limits.transfer]]`, which only `cmdatl run --resume` applies.
  On the mock account the order commands therefore need a run that only a broker account's `cmdatl run` opens: a mock run keeps its own market and journal, and opens its own run.
- A buy must fit the run's cash less what its open buys reserve, and a sell the run's own shares less its open sells; the guard's reasons are `run_budget_exceeded`, `run_buy_needs_limit` (a market buy; cmdatl's orders carry a limit), `run_holding_exceeded` and `run_ledger_unsettled` (the run's figures unknown, as while an order's outcome is unknown), in an order command's refusal (`refused.guard`, its `detail`); a run counts them under its reply's `refused.guard`.
- `status` and `account` show `run_budget` (the run's `id` and whether it is `opened`, with a `next` when there is none), `run_cash` and `run_holding` by symbol; a run's `start` event and its heartbeats show them too, with `closing` while a new run waits for the old run's orders (and a `next`).
- Without `--wait-open-orders` each of the old run's open orders is cancelled once per process: an order the broker refused to cancel, or one waiting to be booked, keeps the new run closing until the operator settles it (the broker's app, `cmdatl order release`).
- On a broker that states no buying power (Alpaca) a budget is what lets a run start and a command buy; on one that does (KIS), both the broker's buying power and the run's cash bound a buy.
