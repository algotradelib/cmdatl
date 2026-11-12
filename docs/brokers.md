# Your own broker or data vendor

A broker or data vendor cmdatl was not built for is a folder you install, found by its name as gh finds its configuration: `$XDG_CONFIG_HOME/cmdatl/brokers/NAME/`, else `~/.config/cmdatl/brokers/NAME/`.
The folder holds `broker.toml`, what `auth login` asks, and `adapter.hpp`, an algotradelib broker adapter cmdatl compiles into a run's or a backtest's driver.

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

```sh
cmdatl auth login --broker myvendor # asks broker.toml's fields; stored unchecked, read by the adapter
cmdatl strategy backtest dip.toml bars.source=myvendor bars.count=500 bars.store_root=store replay.market=us
```

- A backtest with `bars.source = "NAME"` fetches through the adapter into the Parquet store, then replays the bars received ([Run settings](run-spec.md)).
- The adapter returns closed bars only (never a bar still forming), with times as UTC instants and the stamp the vendor actually uses (`open` or `close`): cmdatl checks the series' shape, not what its numbers mean.
- A run trades a broker folder's account when the operator's file says `[account] broker = "NAME"`, as a [KIS account](runs.md#runs-on-a-kis-account) (demo by default; `env = "real"` written out); a data vendor's folder is never an account.
  A client that does not state a trading calendar, a working-order listing and reference data (`BrokerTraits`) is refused before anything is read.
  As on a KIS account, a run trades KRX shares (`replay.market = "kr"`), and no account run polls price bands.
- The built-in names (`mock`, `paper`, `kis`, `kiwoom`, `toss`, `csv`, `wide`, `store`) cannot be taken, as a gh extension cannot override a core command.
- The adapter is native code you trust, as a gh extension is: cmdatl refuses a folder, a directory in it or above it (`cmdatl/`, `brokers/`) or a file that another user could change (by its mode or, on macOS, an access control list that allows anything), and a link; it compiles the files as it read them, and names the folder and a fingerprint of its files when it builds.
  Nothing confines the code itself.
- An adapter's error text and exception text are never shown (a vendor may put its token in a URL): a reply gives the error's category, HTTP status and code.
