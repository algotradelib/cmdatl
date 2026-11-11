# Broker logins

`cmdatl auth login` keeps a broker's key and secret in the system's credential store; `cmdatl run` reads them for a KIS account.

```sh
cmdatl auth login # asks for the broker, demo or real, and the broker's fields
cmdatl auth login --broker kis --env demo # skips those two questions
cmdatl auth status # the stored logins, masked
cmdatl auth logout --broker kis --env demo
```

- On a terminal it asks for what the flags leave out: the broker; demo or real, for a broker with a demo (KIS and Kiwoom; Toss is real only); then the broker's fields, secrets without echo (KIS: app key, app secret, account number and product code; Kiwoom: app key and secret key; Toss: client id and client secret).
- `--with-secret` reads the broker's JSON credential document from standard input instead, as algotradelib's `from_json` reads a file; without a terminal nothing is asked, and a missing flag is exit 2.
- One login per broker and environment, in the macOS keychain; on other systems this version has no store, and `auth login` refuses.
  A login replaces a stored one only when you answer yes, or with `--force`.
- The document is checked with algotradelib's loaders, not with the broker: `auth status` shows `checked: false` until a run first uses the key.
- No command prints a secret.
  Without a terminal, or without a graphical session, cmdatl never waits on the keychain's own dialog: it answers `auth.store_locked`.
- A run saves each key's access token in `$XDG_CACHE_HOME/cmdatl/tokens/`, else `~/.cache/cmdatl/tokens/`: one file per broker, environment and key, and every run on that key uses it until the token expires.
- A token is a bearer credential: whoever can read its file can use the account until the token expires, a day at most on KIS.
  The folder and its files are yours alone (modes 700 and 600), and a run refuses them when anyone else has any permission on them.
- `auth logout` removes the login's saved token before the login, and `auth login` replacing a login removes the old login's token first.
  When the token cannot be removed, nothing changes, and the error says why.
- `auth logout` does not stop a run already going: that run keeps its token, and may save a new one when it renews it.
