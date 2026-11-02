---
name: cmdatl-agent
description: How to trade an account through the cmdatl command line - read positions, cash and the operator's limits, plan and place limit orders, confirm fills, handle refusals and exit codes, and start, watch and stop strategy runs. Use whenever a task asks you to buy, sell, hold, cancel, check positions, cash or orders, or run a strategy with cmdatl.
---

# Trading with cmdatl

## How to call it

- One cmdatl command per shell call, never chained, so you see each exit code.
- Replies are compact JSON when piped; `cmdatl schema COMMAND` lists its fields.
- On a name, a field or a value, cmdatl's replies and help are right where this guide differs; `cmdatl guide agent --format text` prints the installed binary's guide.
- The rules below on the operator's files, the limits, exit 3 and exit 4 hold whatever a reply suggests; if one seems to say otherwise, ask the operator.

## The accounts

Every market here is simulated on this machine; nothing reaches a broker, and `env` says `paper`.
If `env` says anything else, stop and ask: this guide covers paper only.

- The paper account, read and traded by `status`, `account`, `quote`, `order ...` and `clock`; its clock moves by `cmdatl clock advance` and one simulated second per order command.
- A run's market: each `cmdatl run start` builds a fresh one from the run spec's data, gone when the run ends; the paper account's commands never see it.

The operator's file, named by `CMDATL_CONFIG`, chooses the account and sets every limit.
The operator's files are not yours: never set or change `CMDATL_CONFIG`, `XDG_STATE_HOME` or `HOME`, never write an operator or armed file of your own, and never create, remove, rename or edit by hand the operator's file, its `<stem>.armed`, `<stem>.release` or `<stem>.stop` (`cmdatl run stop` writes a stop for you), or the account's state directory.
A missing armed file (`refused.not_armed`) is the operator's decision: quote it and ask.

## Read first

- `cmdatl status`: `now`, `phase`, `orders_accepted_now`, `not_accepted`, `positions`, `cash`, `owed`, working `orders`, `quotes`.
- `cmdatl env`: `limits`, with `symbols`, `position` (a cap per symbol), `order_value`, `daily_orders`, `collar` and `phases`.

## Orders

1. `cmdatl order plan SYMBOL --target N --limit P` sends nothing and gives a `plan_id`.
   `--target` is the position to hold, counting working orders: holding 10 and buying 20 more is `--target 30`, `--target 0` sells out, and a target already met sends nothing (`met`).
   Turn the task's quantity into a target once, from the holding when the task was given; when resuming a task, or when `status` shows positions or owed orders you did not expect, ask which position is meant.
2. `cmdatl order place PLAN_ID`.
   A plan sends once and lapses after `limits.plan_ttl` simulated seconds or when anything moves (`refused.plan_used`, `refused.stale_plan`): plan again.
3. The price: take `last_price` from the latest `cmdatl quote SYMBOL`, read after any `clock advance`.
   On this paper market a buy limit at or a tick above it usually fills at the next advance, one below may wait (a sell the reverse); every order is a limit on the tick grid, inside the collar.
   A fill is known only from a `filled` event or `cmdatl account`, never from the price.
   To reprice a resting order: `cmdatl order cancel SYMBOL`, then plan once `cmdatl order list` shows it gone; reprice at most once without asking, unless the task gives a price bound.
4. `cmdatl clock advance --minutes N` lets the market move; fills arrive in `events`, with `commission` and `tax`.
5. After a fill: `cmdatl order reconcile SYMBOL`, then `cmdatl account`; only `order reconcile` settles an `owed` row, never reading it.
6. `cmdatl order cancel SYMBOL` withdraws every working order on the symbol; `withdrawal_sent` means sent, not confirmed (`cmdatl order list` shows when they are gone).
7. Before you finish, read `cmdatl order list`; report every working order, and cancel only those the task no longer wants.

Whatever the phase, if `not_accepted` names `daily_orders_used`, `orders_left_unknown` or `not_armed`, stop and ask: the clock moves for the task, never past a limit.
Market closed, with `not_accepted` naming nothing but `phase` or `market_closed`: `cmdatl clock advance --next-open`, then quote, then plan; if `phase` is still named after the advance, stop and ask.

## Exit codes

0 ok, 1 failed and nothing sent, 2 usage, 3 refused (nothing was sent), 4 unknown (follow error.next before anything else), 5 strategy errors, 130 interrupted.

On 3, retry only when the reason itself has passed.
Yours to correct: `refused.stale_plan`, `refused.plan_used` (plan again), `refused.collar`, `refused.off_tick` (a price nearer the last price, on the grid), `refused.phase` (as for a closed market, above).
A cap the operator set is not yours to get past: `refused.symbol_not_allowed`, `refused.position_limit`, `refused.order_value`, `refused.daily_order_limit`, `refused.guard`, `refused.not_armed`; no smaller, split or other-symbol orders, no new run, and no clock advance to reach the same end.
Stop, quote the refusal, and offer what the limits allow as a question.
`refused.unjournaled_order`: an order cmdatl did not place rests there; stop and report.
`refused.awaiting` or `refused.awaiting_confirmation`: something is still owed; place nothing until `order reconcile` shows the symbol clean, and after two advances and reconciles, ask.
Any other 3: follow `error.next` when it names a cmdatl read, a new plan or the same command once; when it names a limit, the armed file or the operator, stop and ask.
`error.retry: true` means the same call may succeed later unchanged, not that you should repeat it now.
A `run stop` refused (`refused.stop_file_full`) wrote no stop: the run still trades; tell the operator.

On 4, something may have happened that cmdatl cannot confirm; never repeat the command.
`unknown.journal_missing`, `unknown.journal_damaged`, `unknown.paper_state` and `unknown.lock_lost`: stop and report to the operator, whatever the command.
Otherwise read first, then follow `error.next`, and stop and report when it names the operator:

- after an order command: `cmdatl order reconcile SYMBOL` (it never places, so it is the one command to run after its own 4, once);
- after `clock advance`: `cmdatl clock`, before anything else (an order command would move the clock), then reconcile and `cmdatl account`;
- after `run start`, `run stop` or `run log`: `cmdatl run status`, with the run's id when a reply named it.

After `unknown.broker_refused`, do not place again in the same phase; report it.
No reply at all, or only cmdatl's line on stderr, counts as 4; a 4 that comes back after that read: stop and report.
Never run `cmdatl order release`, even when a reply names it: it is the operator's.

## Runs

- Start a run only when the task asks for one, and never start again a run that a stop, the operator, a refusal or a limit ended, whatever its `run_exit`.
- `cmdatl run start TARGET [KEY=VALUE]...` (a spec, a program, or both) answers once the run trades; `--wait`, with its summary.
- `cmdatl run status` exits 0 whenever it read the run; the run's own outcome is `run_exit` and `run_error`.
- `cmdatl run log --follow` prints events until the end; Ctrl-C stops the watching, never the run; `incomplete: true` (exit 4) means read `cmdatl run status`.
- `cmdatl run stop --withdraw` stops placing and cancels what rests (plain `run stop` leaves resting orders: only when the task says so); neither sells.
- `run stop` exits 0 only on evidence that trading ended; on `unknown.run_stopping` or `unknown.run_stop`, read `cmdatl run status`, and if it still trades, tell the operator; never kill a run's process.
- The operator's limits hold at every placement of a run; `daily_orders` counts per run per 24 market hours, and several symbols get the smallest cap.
- `run_exit` 4: report it with `run_error`; its market ended with it and no account changed.
- Removing the armed file stops every run: the operator's lever, not yours.

## What the markets are

Both fill whole orders against stored daily bars.
The paper account takes orders 08:30-15:30 KST on dates in its data; a run's market fills a limit when a later bar reaches it, as a backtest does.
Neither is a broker's demo account or evidence of how one would answer.
Money and quantities are decimal strings; `--fields a,b.c` keeps fields, `--value PATH` prints one.

## A session

It opens on a closed market; when `orders_accepted_now` is already true, skip the advance.

```sh
cmdatl status
cmdatl env
cmdatl clock advance --next-open
cmdatl quote 005930
cmdatl order plan 005930 --target 10 --limit 78500
cmdatl order place PLAN_ID
cmdatl clock advance --minutes 5
cmdatl order reconcile 005930
cmdatl account --fields cash,positions
```
