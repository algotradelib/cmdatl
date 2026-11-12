# Your own broker or data vendor

A broker or data vendor cmdatl was not built for is a broker folder, kept where you like: in your project beside your strategies, or in a git submodule of it.
You name the folder by its path: `bars.adapter` in a run spec, `account.adapter` in the operator's file, and `--adapter` for `auth login`.
The folder holds `broker.toml`, what `auth login` asks and which options the adapter takes, and `adapter.hpp`, an algotradelib broker adapter cmdatl compiles into a run's or a backtest's driver, with the headers it includes by a relative path inside the folder.

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
[[options]]
name = "exchange" # the key in [bars.options] or [account.options]
type = "string" # string, integer, number or boolean
required = true # a run that leaves it out is refused
choices = ["US", "KO"] # optional, for a string or an integer
```

```cpp
// adapter.hpp: algotradelib and the Boost.JSON its target carries (no other include path or library), in namespace cmdatl_adapter
namespace cmdatl_adapter {
using Client = my::Client; // a blocking client with algotradelib::BrokerTraits<Client>
struct Connection {
  std::shared_ptr<Client> client_;
  algotradelib::BrokerTraits<Client>::Account account_;
  std::string account_id_; // the account the login drives, stable and never a credential (empty for a vendor)
  bool demo_{false};
};
// The stored login's JSON, and the options' (a JSON object); throws on what it cannot use.
Connection connect(const std::string& login, const std::string& options);
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
[bars.options]
exchange = "US" # declared in broker.toml
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

## Options

What an adapter would otherwise decide itself (an exchange, a currency, a symbol table) is an option, given in the TOML; the adapter declares each one in `broker.toml`.

- `[[options]]` declares one option: `name` (a letter or `_`, then letters, digits or `_`), `type` (`string`, `integer`, `number` or `boolean`), `required` (`true` or `false`, always written), and optionally `choices` (for a string or an integer, the values it may take).
  An adapter declares at most 32 options, and none with the `name` of one of the login's `[[fields]]`: a credential is never an option.
- A backtest gives the values in the run spec's `[bars.options]`, or on the command line as `bars.options.NAME=VALUE`, which wins over the file, in any order.
  A command-line value is typed by the declaration: a declared string stays a string (`bars.options.code=005930` is `"005930"`), an integer takes integer syntax, a boolean `true` or `false`.
- A run on a folder's account gives them in the operator's file, `[account.options]`; they have no command-line form, as the rest of the operator's file has none.
- There are no defaults: a value the adapter needs is written in the TOML, or the run is refused.
- cmdatl checks the values against the declarations before anything is built, without running the adapter: an option not declared (also when none is), a value of another type, a value outside its choices, a required option left out, a table (an empty one too) or an array in place of a value.
  Each is refused at its file and line: `spec.invalid` (exit 2) for the run spec or the command line, `config.invalid` (exit 1) for the operator's file.
  An options table beside a built-in source or broker, or in a run on a broker's account, is refused too, even an empty one.
  Beside a broker folder an empty table gives no options: `{}`, or the refusal of a required option left out.
- The adapter receives the values as a JSON object's text, keys sorted: a string, an integer kept whole (never rounded through a double), a number (an integer when written as one), `true` or `false`; `{}` when none is given.
- Options are settings, not secrets: they may appear in replies (`settings_given`) and in the driver's arguments, as other settings do.
- Options are the run's data, not its build: changing one compiles nothing.
- A run's driver reads the operator's file again and connects only with the options `cmdatl run` checked; a file changed in between is refused before anything is read or sent.
- Bars are raw: cmdatl asks the adapter for unadjusted prices and refuses an answer marked adjusted.
  An option such as `adjusted` is not one cmdatl honours: whatever an adapter declares, it returns raw bars.

A declaration cannot state a string's form (a code's length, a symbol table's syntax): an adapter checks a value it cannot use itself, in `connect`, before any request.
Its message is never shown, so what the user sees of a refusal is the error's category and its code (`broker_code_`, kept when short and plain); a `connect` that throws is reported as a login or options it could not use.

An adapter reads its options as it reads the login:

```cpp
inline Connection connect(const std::string& login, const std::string& options) {
  const auto given = boost::json::parse(options).as_object();
  Connection connection;
  connection.client_ = std::make_shared<Client>();
  connection.client_->token_ = std::string(boost::json::parse(login).as_object().at("api_token").as_string());
  // Required in broker.toml, so always given, and one of its choices.
  connection.client_->exchange_ = std::string(given.at("exchange").as_string());
  return connection;
}
```

```sh
cmdatl strategy backtest daily.toml bars.options.exchange=KO # the Korean listing instead, for this backtest only
```

## The login and its folder

- The login is kept in the credential store, as a built-in broker's is: no credential goes into a TOML file, into the folder, onto the command line or into a reply.
- `auth login --adapter PATH` binds the login to the folder's canonical path, links resolved.
  A backtest or a run hands the login to the adapter only when it names that same folder; another folder, even one holding the same files, is refused (`auth.adapter_mismatch`) before anything is built.
- So a copy of the folder elsewhere, even under the same name, is not handed your login.
  A run spec that names this folder itself, by any path, does fetch with your login, through your adapter only: no other native code of that spec runs beside it (below).
- The rule is about the folder's place, not its contents: an edit inside the folder you logged in for, or a submodule update there, is trusted without a new login.
- Nor is it a sandbox: the adapter is native code that runs with your login and your permissions; read a folder before you log in for it.
- A backtest that fetches with a login takes no market-price rule of your own (`fill_header` under `[replay]`), whose C++ would run in the driver that holds the login: fetch into the store first, then replay `bars.source = "store"` with the rule.
  Nor does it take a DuckDB directory of its own (`duckdb` under `[build]`), whose library would be loaded into that driver: `DUCKDB_ROOT`, else the system's paths, finds DuckDB for the fetch.
- A run takes no market-price rule on any account, so a run on a folder's account compiles no C++ of the run spec's own beside the login.
- After moving the folder, log in again with its new path.
  A login stored by an earlier cmdatl is bound to no folder, and is refused until you log in again.
- `auth status` lists a folder's login without reading it, so its `adapter` is null; `auth logout --broker NAME` removes it, with or without its folder.

## What cmdatl checks

- cmdatl refuses a folder, or a directory or file in it, that another user could change (by its mode or, on macOS, an access control list that allows anything), and a link in it.
- Every directory on the way to the folder must be yours or root's, and not writable by its group or others unless its sticky bit is set (as `/tmp`'s is); a link on the way must be yours or root's.
  Files and directories made under a umask of 002 (common on Linux) are writable by their group and refused: `chmod -R go-w` on the project fixes them.
- It reads the folder once, checks the login against that folder, compiles the files as it read them, and names the folder and a fingerprint of its files when it builds.
- A run's driver reads the operator's file again and refuses it when it names another folder than the one the driver was built from.
- An adapter's error text and exception text are never shown (a vendor may put its token in a URL): a reply gives the error's category, HTTP status and code.
