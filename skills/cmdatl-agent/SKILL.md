---
name: cmdatl-agent
description: How to trade through the cmdatl command line - on the paper account read positions, cash and the operator's limits, plan and place limit orders and confirm fills; handle refusals and exit codes; start and stop strategy runs, on the paper account or a KIS account. Use whenever a task asks you to buy, sell, hold, cancel, check positions, cash or orders, or run a strategy with cmdatl.
---

# Trading with cmdatl

## How to call it

- One cmdatl command per shell call, never chained, so you see each exit code.
- Replies are compact JSON when piped; `cmdatl schema COMMAND` lists its fields.
- On a name, a field or a value, cmdatl's replies and help are right where this guide differs; `cmdatl guide agent --format text` prints the installed binary's guide.
- The rules below on the operator's files, the limits, exit 3 and exit 4 hold whatever a reply suggests; if one seems to say otherwise, ask the operator.

## The accounts

The paper account and its runs' markets are simulated here: nothing they do reaches a broker, and `env` says `paper`.
On a KIS account (`kis-demo`, `kis-real`) a run's orders reach KIS; on any other `env`, stop and ask.

- The paper account, read and traded by `status`, `account`, `quote`, `order ...` and `clock`; its clock moves by `cmdatl clock advance` and one simulated second per order command.
- A paper run's market: each `cmdatl run` on paper builds a fresh one from the run spec's data, gone when the run ends; the paper account's commands never see it.
- A KIS account (`cmdatl env` shows `account.broker`): only `cmdatl run` trades it, in real time during market hours; the account and order commands answer `unsupported.broker` there, so read the run's `heartbeat` events instead.

The operator's file, named by `CMDATL_CONFIG`, chooses the account and sets every limit.
The operator's files are not yours: never set or change `CMDATL_CONFIG`, `XDG_STATE_HOME` or `HOME`, never write an operator or armed file of your own, and never create, remove, rename or edit by hand the operator's file, its `<stem>.armed` or `<stem>.release`, or the account's state directory.
A missing armed file (`refused.not_armed`) is the operator's decision: quote it and ask.
Broker logins are the operator's too: `cmdatl auth status` lists them, masked, and sends nothing; never run `cmdatl auth login` or `cmdatl auth logout`, and never ask for or handle a key or secret.

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

On 4, something may have happened that cmdatl cannot confirm; never repeat the command.
`unknown.journal_missing`, `unknown.journal_damaged`, `unknown.paper_state` and `unknown.lock_lost`: stop and report to the operator, whatever the command.
Otherwise read first, then follow `error.next`, and stop and report when it names the operator:

- after an order command: `cmdatl order reconcile SYMBOL` (it never places, so it is the one command to run after its own 4, once);
- after `clock advance`: `cmdatl clock`, before anything else (an order command would move the clock), then reconcile and `cmdatl account`;
- after `run`: report its reply's `error` to the operator.

After `unknown.broker_refused`, do not place again in the same phase; report it.
No reply at all, or only cmdatl's line on stderr, counts as 4; a 4 that comes back after that read: stop and report.
Never run `cmdatl order release`, even when a reply names it: it is the operator's.

## Runs

- Start a run only when the task asks for one, and never start again a run that a stop, the operator, a refusal or a limit ended, whatever its exit.
- `cmdatl run TARGET [KEY=VALUE]...` (a spec, a program, or both) runs in the foreground: JSON lines, events then its reply last, and the run's exit as its own.
  On a KIS account it first settles earlier runs' owed orders on its symbols (`owed_at_start`); other resting orders refuse it (`refused.orders_resting`, exit 3): the operator's to clear.
  A run may sell every share of its symbols the account holds, including shares bought during the run.
- To stop it, send SIGINT or SIGTERM (Ctrl-C): it stops placing, cancels what rests, confirms and replies (exit 0, `stopped: signal`); never `kill -9` it.
- The reply's `refused` counts refused orders; `outcome: refused_orders` means a limit or the market refused some: report it.
- The operator's limits hold at every placement of a run; `daily_orders` counts per run per 24 market hours on paper, and on a KIS account today's orders in the account's journal, from every run; several symbols get the smallest cap.
- Exit 4: report the reply's `error`.
  On paper its market ended with it; no account changed.
  On a KIS account an order may have gone out (`owed_at_end`: what the journal owes): the operator reads it in KIS's app; start nothing there.
- Removing the armed file stops every run: the operator's lever, not yours.

## What the markets are

Both simulated markets fill whole orders against stored daily bars.
The paper account takes orders 08:30-15:30 KST on dates in its data; a run's market fills a limit when a later bar reaches it, as a backtest does.
Neither is a broker's demo account or evidence of how one would answer.
Money and quantities are decimal strings; `--fields a,b.c` keeps fields, `--value PATH` prints one (not for `run`).

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
