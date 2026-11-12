# Orders

An agent moves a position to a target in two steps, a plan and its placement, and nothing places or cancels except through them.

```sh
cmdatl order plan 005930 --target 10 --limit 78200 # what would be sent; a plan id
cmdatl order place p9cc4b94da4 # sends that plan, if nothing moved since
cmdatl order cancel 005930 # withdraws every working order on 005930; never places
cmdatl order reconcile # records fills from holdings for every owed order; sends nothing
cmdatl order reconcile 005930
cmdatl order release 005930 --reason "read the order list and holdings"
```

- A target is a position, not a quantity: `--target 0` sells out, and a target the holding and the working orders already meet sends nothing (`met`).
- Every limit comes from the operator's file at the moment of sending; no option of an order command names a limit, phase, collar or order type.
  Every placement is a limit order inside `limits.collar` of the last price and on the tick grid.
- A plan id is bound to what the plan saw: the account, the request, the holding and working orders of the symbol, the cash and the journal's size.
  It is used once, it lapses after `limits.plan_ttl` simulated seconds (60 by default), and anything moving makes it stale (`refused.stale_plan`): plan again.
- Each order command takes one simulated second, as a broker call takes time; `events` is what the market did in that second, which can fill or expire an order already resting, even on a plan or a refusal.
- Exit 3 means nothing reached the market in this call: plan again only once its reason has passed (a lapsed plan, a closed market); a limit the operator set is not retried around.
  Exit 4 means an order may have gone out, or the journal cannot say: run `cmdatl order reconcile SYMBOL` before anything else, and read `error.next`.
  A request the market itself refused is exit 4 `unknown.broker_refused`, since it reached the market; `cmdatl order reconcile SYMBOL` then shows nothing owed.
- No reply at all (a killed process): treat it as exit 4.
- `cmdatl order cancel SYMBOL` withdraws by symbol, needs no armed file and runs in any phase; `withdrawal_sent` means sent, not confirmed, and `cmdatl order list` shows when the orders are gone.
- `cmdatl order reconcile` reports each symbol as `clean`, `working` (an order rests), `holding_moved` (holdings changed between two reads) or a refusal's reason; anything still owed afterwards is exit 3 `refused.awaiting`.
- `owed` rows say what to do next; reading them never settles them, only an order command writes the journal.
- An order the broker never named stays owed until the listing settles it or the operator releases it.
  `cmdatl order release SYMBOL --reason TEXT` needs the armed file and the operator's approval `<stem>.release` beside the operator's file (`mock.toml` gives `mock.release`): one line per owed order, `SYMBOL JOURNAL_ID` with one space, naming exactly the journal ids `cmdatl account` shows as owed on that symbol.
  The approval is renamed after one use.
- A damaged journal has no cmdatl remedy: on the mock account, the operator keeps the account's directory for inspection and moves it aside, which abandons that mock market with it.
