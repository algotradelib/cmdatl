# Output

- Text on a terminal, compact JSON otherwise; `--json` or `--format json|jsonl|text` chooses.
- `--fields a,b.c` keeps only those fields; `--value PATH` prints one value bare, the one reply without `env`.
- Every other reply carries `env`; a failure carries one `error` object with a stable code, such as `refused.position_limit`.
- A field name no reply of the command has is refused before the command runs; one missing from a reply the command already produced comes back with the whole reply and a `warning`.
- Exit codes: 0 ok, 1 failed and nothing sent, 2 usage, 3 refused (nothing was sent), 4 unknown (follow error.next before anything else), 5 strategy errors, 130 interrupted (Ctrl-C, SIGTERM or SIGHUP stopped a command before anything could be sent; it still replies, and once an order command has begun to send it answers with its own outcome instead).
- On a terminal a failure is written to stderr; in JSON it is the reply, on stdout.
- `cmdatl help COMMAND` (or `--help`) shows a command's usage and examples; `cmdatl schema` describes every command, its fields and its error codes for a program.
