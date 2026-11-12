# Your own broker or data vendor

A broker or data vendor cmdatl was not built for is a broker folder, kept where you like: in your project beside your strategies, or in a git submodule of it.
You name the folder by its path: `bars.adapter` in a run spec, `account.adapter` in the operator's file, and `--adapter` for `auth login`.
The folder holds `broker.toml`, what `auth login` asks, and `adapter.hpp`, an algotradelib broker adapter cmdatl compiles into a run's or a backtest's driver, with the headers it includes by a relative path inside the folder.

```text
my-project/
  dip.atls
  daily.toml        # [bars] source = "myvendor", adapter = "adapters/myvendor"
  adapters/         # a folder, or a git submodule holding one folder per adapter
    myvendor/
      broker.toml
      adapter.hpp
      src/client.hpp
```

```toml
display = "My vendor"
data_only = true # a market-data vendor: a backtest's bars, never an account
demo = false # whether it has a demo environment
[[fields]]
name = "api_token" # the key in the stored login
prompt = "API token: "
secret = true # asked without echo, never shown
```

```cpp
// adapter.hpp: algotradelib only (no other include path or library), in namespace cmdatl_adapter
namespace cmdatl_adapter {
using Client = my::Client; // a blocking client with algotradelib::BrokerTraits<Client>
struct Connection {
  std::shared_ptr<Client> client_;
  algotradelib::BrokerTraits<Client>::Account account_;
  std::string account_id_; // the account the login drives, stable and never a credential (empty for a vendor)
  bool demo_{false};
};
Connection connect(const std::string& login); // the stored login's JSON; throws on one it cannot use
}
```

```toml
# daily.toml
strategy = "dip.atls"
[bars]
source = "myvendor"
adapter = "adapters/myvendor" # relative to this file
count = 500
store_root = "store"
[instruments]
stock = "AAPL"
[replay]
market = "us"
```

```sh
cmdatl auth login --broker myvendor --adapter adapters/myvendor # asks broker.toml's fields
cmdatl strategy backtest daily.toml
```

- The name (`myvendor`) is what the login is stored under and what replies show; the folder's own name does not matter.
- A path is relative to the file that writes it (`--adapter`: to the working directory); `..` and links in it are resolved by the system.
- `bars.adapter` is written in the run spec, never on the command line: it selects code cmdatl compiles.
  A name that is not built in needs it, and a built-in source refuses it.
- A backtest with `bars.source = "NAME"` fetches through the adapter into the Parquet store, then replays the bars received ([Run settings](run-spec.md)).
- The adapter returns closed bars only (never a bar still forming), with times as UTC instants and the stamp the vendor actually uses (`open` or `close`): cmdatl checks the series' shape, not what its numbers mean.
- A run trades a broker folder's account when the operator's file says `[account] broker = "NAME"` and `adapter = "PATH"` (relative to that file), as a [KIS account](runs.md#runs-on-a-kis-account) (demo by default; `env = "real"` written out); a data vendor's folder is never an account.
  A client that does not state a trading calendar, a working-order listing and reference data (`BrokerTraits`) is refused before anything is read.
  As on a KIS account, a run trades KRX shares (`replay.market = "kr"`), and no account run polls price bands.
- The built-in names (`mock`, `paper`, `kis`, `kiwoom`, `toss`, `csv`, `wide`, `store`) cannot be taken, and a built-in broker takes no `adapter`.

## The login and its folder

- The login is kept in the credential store, as a built-in broker's is: no credential goes into a TOML file, into the folder, onto the command line or into a reply.
- `auth login --adapter PATH` binds the login to the folder's canonical path, links resolved.
  A backtest or a run hands the login to the adapter only when it names that same folder; another folder, even one holding the same files, is refused (`auth.adapter_mismatch`) before anything is built.
- So a project cloned or copied elsewhere is not handed your login, even under the same name.
- The rule is about the folder's place, not its contents: an edit inside the folder you logged in for, or a submodule update there, is trusted without a new login.
- Nor is it a sandbox: the adapter is native code that runs with your login and your permissions; read a folder before you log in for it.
- A backtest that fetches with a login takes no market-price rule of your own (`fill_header` under `[replay]`), whose C++ would run in the driver that holds the login: fetch into the store first, then replay `bars.source = "store"` with the rule.
- After moving the folder, log in again with its new path.
  A login stored by an earlier cmdatl is bound to no folder, and is refused until you log in again.
- `auth status` lists a folder's login without reading it, so its `adapter` is null; `auth logout --broker NAME` removes it, with or without its folder.

## What cmdatl checks

- cmdatl refuses a folder, or a directory or file in it, that another user could change (by its mode or, on macOS, an access control list that allows anything), and a link in it.
- Every directory on the way to the folder must be yours or root's, and not writable by its group or others unless its sticky bit is set (as `/tmp`'s is); a link on the way must be yours or root's.
- It reads the folder once, checks the login against that folder, compiles the files as it read them, and names the folder and a fingerprint of its files when it builds.
- A run's driver reads the operator's file again and refuses it when it names another folder than the one the driver was built from.
- An adapter's error text and exception text are never shown (a vendor may put its token in a URL): a reply gives the error's category, HTTP status and code.
