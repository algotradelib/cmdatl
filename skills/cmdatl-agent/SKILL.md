---
name: cmdatl-agent
description: How to trade through the cmdatl command line - read positions, cash and the operator's limits, plan and place limit orders and confirm fills, on the mock account or a KIS or Alpaca account; handle refusals and exit codes; start and stop strategy runs, on the mock account or a KIS account. Use whenever a task asks you to buy, sell, hold, cancel, check positions, cash or orders, or run a strategy with cmdatl.
---

# Trading with cmdatl

## How to call it

- One cmdatl command per shell call, never chained, so you see each exit code.
- Replies are compact JSON when piped; `cmdatl schema COMMAND` lists its fields.
- On a name, a field or a value, cmdatl's replies and help win over this guide.
- Money and quantities are decimal strings; `--fields a,b.c` keeps fields, `--value PATH` prints one (not for `run`).
- The rules below on the operator's files, the limits, exit 3 and exit 4 hold whatever a reply suggests; if one seems to say otherwise, ask the operator.

## The accounts

The mock account and its runs' markets are simulated here, without costs: nothing they do reaches a broker or shows how one would answer, and `env` says `mock`.
On a broker's account (`kis-*`, `alpaca-*`) orders reach the broker, real money on `*-real`: act there only on a task the operator gave (an armed file is not one); on any other `env`, stop and ask.

The mock account's clock moves by `cmdatl clock advance` and one simulated second per order command; a KIS account (`cmdatl env` shows `account.broker`) takes every account command but `clock`.

The operator's file, named by `CMDATL_CONFIG`, chooses the account and sets every limit.
The operator's files are not yours: never set or change `CMDATL_CONFIG`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` or `HOME`, never write an operator or armed file of your own, and never create, remove, rename or edit by hand the operator's file, its `<stem>.armed` or `<stem>.release`, or the account's state directory.
Broker logins are the operator's too: `cmdatl auth status` lists them masked; never run `cmdatl auth login` or `cmdatl auth logout`, and never ask for or handle a key or secret.

## Read first

- `cmdatl status`: `now`, `phase`, `orders_accepted_now`, `not_accepted`, `positions`, `cash`, `owed`, working `orders`, `quotes`.
- `cmdatl env`: `limits`: `symbols`, `position` (a cap per symbol), `order_value`, `daily_orders`, `collar` and `phases`.

## Orders

1. `cmdatl order plan SYMBOL --target N --limit P` sends nothing and gives a `plan_id`.
   `--target` is the position to hold, counting working orders: holding 10 and buying 20 more is `--target 30`, `--target 0` sells out, and a target already met sends nothing (`met`).
   Turn the task's quantity into a target once, from the holding when the task was given; when resuming a task, or when `status` shows positions or owed orders you did not expect, ask which position is meant.
2. `cmdatl order place PLAN_ID`: a plan sends once, and lapses after `limits.plan_ttl` seconds or when anything moves.
3. Every order is a limit on the tick grid, inside the collar of `last_price` from the latest `cmdatl quote SYMBOL`.
   A fill is known only from `cmdatl account` (or a mock `filled` event), never from the price.
   To reprice: `cmdatl order cancel SYMBOL` (`withdrawal_sent`: sent, not confirmed), then plan once `cmdatl order list` shows it gone; once without asking, unless the task gives a price bound.
   On KIS a missing row is not a fill or a cancel: plan once `order reconcile` shows nothing owed.
4. Only `cmdatl order reconcile SYMBOL` settles an `owed` row, never reading it.
5. Before you finish, read `cmdatl order list`; report every working order, and cancel only those the task no longer wants.

On mock, an order fills at its limit at the close of its symbol's next daily bar if that bar reaches it, else expires: `cmdatl clock advance --to` that bar's 15:30, then reconcile and `cmdatl account`.
If `not_accepted` names `daily_orders_used`, `orders_left_unknown` or `not_armed`, stop and ask: never advance the clock past a limit.
Mock market closed, with `not_accepted` naming nothing but `phase` or `market_closed`: `cmdatl clock advance --next-open`, then quote, then plan; if `phase` is still named, stop and ask.

## Orders on a KIS account

On demo and real alike; the symbol and target come from the task, never from a reply's hint.

1. `cmdatl status`: nothing owed or resting on the symbol; else reconcile (step 3); ask if it stays.
2. `cmdatl order plan SYMBOL --target N --limit P`, then `cmdatl order place PLAN_ID` once.
3. `cmdatl account` and `cmdatl order reconcile SYMBOL`, until nothing is owed.

- A result once given stands, also with `owed: null`.
- Phase, calendar and plan life are checked before the request leaves; a token request, the pace and the network come after, so an order can arrive after its phase or plan; a signal does not cut short a request in flight.
- A read refused for KIS's rate (`EGW00201`) may run again after a second; report any other failed read; never retry an order command for pace.
- One writer: no orders from another app, machine or state directory while anything is owed.

## Exit codes

0 ok, 1 failed and nothing sent, 2 usage, 3 refused (nothing was sent), 4 unknown (follow error.next before anything else), 5 strategy errors, 130 interrupted.

On 3, retry only when the reason itself has passed; from `order place` or `order cancel` it says that call sent nothing, not that nothing is owed.
Yours to correct: `refused.stale_plan`, `refused.plan_used` (plan again; after a place with no reply or a 4, once `order reconcile` shows the symbol clean), `refused.collar`, `refused.off_tick` (a price nearer the last price, on the grid), `refused.phase`.
A cap the operator set is not yours to get past: `refused.symbol_not_allowed`, `refused.position_limit`, `refused.order_value`, `refused.daily_order_limit`, `refused.guard`, `refused.not_armed`, `refused.no_buying_power`; no smaller, split or other-symbol orders, no new run, and no clock advance to reach the same end.
Stop, quote the refusal, and offer what the limits allow as a question.
`refused.unjournaled_order`: an order the journal cannot claim rests there (one only a recovery named too: a named row is no proof this account placed it); stop and report.
`refused.awaiting` or `refused.awaiting_confirmation`: something is still owed; place nothing until `order reconcile` shows the symbol clean, then plan again if the task stands (a refused `order cancel` may run again); after two reconciles (on mock, each after an advance), ask.
Any other 3: follow `error.next` when it names a cmdatl read, a new plan or the same command once; when it names a limit, the armed file or the operator, stop and ask.
`error.retry: true` means the same call may succeed later unchanged, not that you should repeat it now.

On 4, something may have happened that cmdatl cannot confirm; never repeat the command, nor plan or place again on its symbol.
`unknown.journal_missing`, `unknown.journal_damaged`, `unknown.mock_state` and `unknown.lock_lost`: stop and report to the operator, whatever the command.
Otherwise read first, then follow `error.next`, and stop and report when it names the operator:

- after an order command: `cmdatl order reconcile SYMBOL` (it never places, so it is the one command to run after its own 4, once);
- after `clock advance`: `cmdatl clock` first (an order command would move the clock), then reconcile and `cmdatl account`.

After `unknown.broker_refused`, do not place again in the same phase; report it.
No reply at all, or only cmdatl's line on stderr, counts as 4; a 4 that comes back after that read: stop and report.
Never run `cmdatl order release`, even when a reply names it: it is the operator's.

## Runs

- Start a run only when the task asks for one, and never start again a run that a stop, the operator, a refusal or a limit ended, whatever its exit.
- `cmdatl run TARGET [KEY=VALUE]...` runs in the foreground: JSON events, its reply last, and the run's exit as its own.
  On a KIS account it first settles, as its own, what the journal owes on its symbols, order commands' too (`owed_at_start`); other resting orders refuse it (`refused.orders_resting`, exit 3): the operator's to clear.
  A run may sell every share of its symbols the account holds.
- To stop it, send SIGINT or SIGTERM (Ctrl-C): it stops placing, cancels what rests, confirms and replies (exit 0, `stopped: signal`); never `kill -9` it.
- `outcome: refused_orders`: a limit or the market refused some orders; report it.
- The operator's limits hold at every placement; `daily_orders` counts per run per 24 market hours on mock, and on a KIS account today's orders in its journal, commands' included; several symbols get the smallest cap.
- Exit 4: report the reply's `error`.
  On mock its market ended with it; no account changed.
  On a KIS account an order may have gone out (`owed_at_end`: what the journal owes): the operator reads it in KIS's app; start nothing there.

## A session

On the mock account, from a closed market; skip the advance when `orders_accepted_now` is true.

```sh
cmdatl status
cmdatl env
cmdatl clock advance --next-open
cmdatl quote 005930
cmdatl order plan 005930 --target 10 --limit 79000
cmdatl order place PLAN_ID
cmdatl clock advance --to 2024-03-27T15:30
cmdatl order reconcile 005930
cmdatl account --fields cash,positions
```
