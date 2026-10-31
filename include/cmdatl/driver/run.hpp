#pragma once

// A run: the driver `run start` builds for one program shape and runs detached.
//
// The run's market is the library's mock broker, built in this process from the run spec's data as `atlc run` reads
// it, and gone when the process ends: each run is its own market, with its own journal and its own lock, and shares
// nothing with the agent commands' paper account or with another run. The operator's file decides what may be placed.
//
// The driver checks the start refusals in order, builds the market, trades the program through the live wiring
// (`live.hpp`) behind the placement boundary (`session.hpp`) on the market's clock (`clock.hpp`), writes its events to
// `events.jsonl` in the run's directory (the start line first), and writes one JSON reply on stdout before it releases
// its lock.
//
// Nothing on its command line names a limit, a phase, a path or a clock: `run.id`, then atlc's own `args`, which the
// generated `main.cpp` turns into the market's inputs (`load`) and the strategy (`make`) through atlc's own code.

#include "../account/account.hpp"
#include "../account/operator.hpp"
#include "../core/errors.hpp"
#include "../core/reply.hpp"
#include "../orders/rails.hpp"
#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "../system/process.hpp"
#include "./clock.hpp"
#include "./events.hpp"
#include "./live.hpp"
#include "./session.hpp"

#include <algotradelib/backtest>
#include <algotradelib/broker>
#include <algotradelib/mock>
#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cmdatl {
namespace driver {

/** @brief How far the market moves at each step of the loop. */
inline constexpr std::chrono::minutes STEP{15};
/** @brief How far past the last bar the market may be moved, for the closing pass and the finalization stage. */
inline constexpr std::chrono::hours BOUND_AFTER_LAST{24};
/** @brief The longest warm-up a run accepts, in bars. */
inline constexpr std::size_t MAX_WARMUP_BARS = 10000;

#if defined(CMDATL_RUN_TEST_HOOKS)
/**
 * @brief Test-only: called with the mock client before trading, so a test scripts the broker's faults. Only the run
 *   tests' executable defines `CMDATL_RUN_TEST_HOOKS`; never define it elsewhere.
 * @return The hook.
 */
inline std::function<void(atl::mock::MockClient&)>& client_hook() {
  static std::function<void(atl::mock::MockClient&)> hook;
  return hook;
}
#endif

/** @brief What `load` gives: the market's inputs, read by atlc's own code. */
struct Loaded {
  /** @brief The symbols traded, in feed order. */
  std::vector<std::string> symbols_;
  /** @brief One series per symbol, with atlc's interval, stamp and instrument id. */
  std::vector<atl::BarSeries> series_;
  /** @brief The instruments, as atlc's replay has them. */
  std::vector<atl::Instrument> instruments_;
  /** @brief The fill model and its costs. */
  atl::backtest::NextBarFillModel fills_;
  /** @brief The opening cash, in won. */
  atl::Decimal64 cash_;
};

/**
 * @brief Whether a text is a run id: `r` and 12 lowercase hex digits.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_run_id(const std::string& text) {
  return text.size() == 13 && text[0] == 'r' &&
    std::all_of(text.begin() + 1, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/** @brief The driver's own argument, and atlc's, as given. */
struct RunArgs {
  /** @brief The run's id. */
  std::string id_;
  /** @brief atlc's `key=value` arguments, in order. */
  std::vector<std::string> atlc_;
};

/**
 * @brief Reads the command line: `run.id=ID` exactly once, then atlc's arguments; a `paper.*` key, another `run.*`
 *   key, a missing id, or an argument without `=` is refused.
 * @param argc The count.
 * @param argv The arguments.
 * @param args Set to what was read.
 * @return Why it is refused, or nothing.
 */
inline std::optional<std::string> read_args(int argc, char** argv, RunArgs& args) {
  bool id_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    if (eq == std::string::npos || eq == 0) {
      return "expected key=value, got '" + arg + "'";
    }
    const auto key = arg.substr(0, eq);
    const auto value = arg.substr(eq + 1);
    if (key.rfind("paper.", 0) == 0) {
      return "no paper.* key is a run argument (" + key + ")";
    }
    if (key == "run.id") {
      if (id_seen || !valid_run_id(value)) {
        return "run.id is given once, as r and 12 lowercase hex digits";
      }
      id_seen = true;
      args.id_ = value;
    } else if (key.rfind("run.", 0) == 0) {
      return "the only run.* key is run.id (" + key + ")";
    } else {
      args.atlc_.push_back(arg);
    }
  }
  if (!id_seen) {
    return "run.id is missing";
  }
  return std::nullopt;
}

/**
 * @brief The last value of an atlc argument.
 * @param args atlc's arguments.
 * @param key The key.
 * @return Its value, if given.
 */
inline std::optional<std::string> atlc_value(const std::vector<std::string>& args, const std::string& key) {
  std::optional<std::string> found;
  for (const auto& arg : args) {
    if (arg.size() > key.size() && arg.compare(0, key.size(), key) == 0 && arg[key.size()] == '=') {
      found = arg.substr(key.size() + 1);
    }
  }
  return found;
}

/**
 * @brief Why a run spec's market settings are refused before anything is loaded, or nothing: the market `kr`, no store,
 *   no short sales, no session legs.
 * @param args atlc's arguments.
 * @return The key at fault and why, or nothing.
 */
inline std::optional<std::pair<std::string, std::string>> spec_refusal(const std::vector<std::string>& args) {
  const auto market = atlc_value(args, "replay.market");
  if (!market || *market != "kr") {
    return std::make_pair(
      std::string("replay.market"),
      std::string("a run trades KRX shares in won, under the operator's limits: set [replay] market = \"kr\"")
    );
  }
  if (atlc_value(args, "data.store")) {
    return std::make_pair(std::string("data.store"), std::string("a run reads files, not a store: give [data] a file"));
  }
  if (atlc_value(args, "replay.shorts").value_or("false") == "true") {
    return std::make_pair(std::string("replay.shorts"), std::string("a run keeps the sellable check on: remove [replay] shorts"));
  }
  if (atlc_value(args, "replay.fill").value_or("next-open") == "legs") {
    return std::make_pair(std::string("replay.fill"), std::string("a run has no session legs: remove [replay] fill = \"legs\""));
  }
  return std::nullopt;
}

/** @brief A sum that does not fit an exact decimal: a run's money totals never fall back to a guess. */
struct SumOverflow : std::runtime_error {
  SumOverflow() : std::runtime_error("a money total of the run does not fit an exact decimal") {}
};

/**
 * @brief An exact sum, or `SumOverflow`.
 * @param a One.
 * @param b The other.
 * @return The sum.
 */
inline atl::Decimal64 add_exact(const atl::Decimal64& a, const atl::Decimal64& b) {
  const auto sum = atl::decimal_add(a, b);
  if (!sum) {
    throw SumOverflow();
  }
  return *sum;
}

/**
 * @brief An exact difference, or `SumOverflow`.
 * @param a The minuend.
 * @param b The subtrahend.
 * @return The difference.
 */
inline atl::Decimal64 subtract_exact(const atl::Decimal64& a, const atl::Decimal64& b) {
  const auto difference = atl::decimal_subtract(a, b);
  if (!difference) {
    throw SumOverflow();
  }
  return *difference;
}

/**
 * @brief An exact product, or `SumOverflow`.
 * @param a One.
 * @param b The other.
 * @return The product.
 */
inline atl::Decimal64 multiply_exact(const atl::Decimal64& a, const atl::Decimal64& b) {
  const auto product = atl::decimal_multiply(a, b);
  if (!product) {
    throw SumOverflow();
  }
  return *product;
}

/** @brief What the run's orders did, from the market's history: the order events and the totals. */
struct Ledger {
  /** @brief Orders placed and not yet seen finished, by id. */
  std::set<std::string> open_;
  /** @brief Fills. */
  std::int64_t executions_{0};
  /** @brief Commission paid. */
  atl::Decimal64 commission_{atl::Decimal64::parse("0")};
  /** @brief Tax paid. */
  atl::Decimal64 tax_{atl::Decimal64::parse("0")};
  /** @brief Cash: the opening cash, less what buys paid, plus what sales received, less costs. */
  atl::Decimal64 cash_{atl::Decimal64::parse("0")};
};

/**
 * @brief An order event of the event log.
 * @param kind `filled`, `canceled` or `expired`.
 * @param order The order, as the history shows it.
 * @param at When it was seen.
 * @return The event.
 */
inline json::object order_event(const char* kind, const atl::OrderSnapshot& order, std::chrono::system_clock::time_point at) {
  json::object event{
    {"event", kind},
    {"at", at_text(at)},
    {"order_id", order.order_id_},
    {"symbol", order.instrument_.symbol_},
    {"side", order.side_ == atl::OrderSide::BUY ? "buy" : "sell"},
    {"quantity", order.quantity_.str()},
  };
  if (order.price_) {
    event["price"] = order.price_->str();
  }
  const auto& execution = order.execution_;
  if (kind == std::string("filled")) {
    event["quantity"] = execution.filled_quantity_.str();
    if (execution.average_filled_price_) {
      event["price"] = execution.average_filled_price_->str();
    }
    event["commission"] = execution.commission_ ? execution.commission_->amount_.str() : "0";
    event["tax"] = execution.tax_ ? execution.tax_->amount_.str() : "0";
  }
  return event;
}

#if !defined(_WIN32)
/**
 * @brief Reads the market's history for the run's open orders: each newly finished one is an event, and fills move the
 *   totals.
 * @tparam Cancelled A callable telling whether the boundary cancelled an order id.
 * @param market The market.
 * @param ledger The totals.
 * @param cancelled Whether the boundary cancelled an order (`canceled`), or not (`expired`).
 * @param guard Where the events go.
 */
template <typename Cancelled>
void scan_orders(const atl::mock::Market& market, Ledger& ledger, Cancelled&& cancelled, Guard& guard) {
  if (ledger.open_.empty()) {
    return;
  }
  const auto page = market.order_history({});
  for (const auto& order : page.orders_) {
    if (ledger.open_.count(order.order_id_) == 0) {
      continue;
    }
    if (order.status_ == atl::OrderStatus::FILLED) {
      const auto& execution = order.execution_;
      if (!execution.filled_amount_) {
        throw SumOverflow();
      }
      const auto commission = execution.commission_ ? execution.commission_->amount_ : atl::Decimal64::parse("0");
      const auto tax = execution.tax_ ? execution.tax_->amount_ : atl::Decimal64::parse("0");
      ++ledger.executions_;
      ledger.commission_ = add_exact(ledger.commission_, commission);
      ledger.tax_ = add_exact(ledger.tax_, tax);
      const auto& amount = execution.filled_amount_->amount_;
      ledger.cash_ = order.side_ == atl::OrderSide::BUY ?
        subtract_exact(ledger.cash_, amount) :
        add_exact(ledger.cash_, amount);
      ledger.cash_ = subtract_exact(subtract_exact(ledger.cash_, commission), tax);
      guard.emit(order_event("filled", order, market.now()));
      ledger.open_.erase(order.order_id_);
    } else if (order.status_ == atl::OrderStatus::CANCELED) {
      guard.emit(order_event(cancelled(order.order_id_) ? "canceled" : "expired", order, market.now()));
      ledger.open_.erase(order.order_id_);
    }
  }
}
#endif

/**
 * @brief Runs a program on its own mock market; see the file comment.
 * @tparam Program The generated strategy.
 * @tparam Load A callable `(const std::vector<std::string>& args) -> Loaded`, throwing `std::invalid_argument` or
 *   `std::out_of_range` for data it refuses; in the generated `main.cpp` it is atlc's own loader and replay settings.
 * @tparam Make A callable `(const std::vector<std::string>& args, const std::vector<atl::InstrumentId>& ids) -> Program`,
 *   throwing for a parameter, bound or pricing it refuses; in the generated `main.cpp` it applies atlc's rules.
 * @param argc The count.
 * @param argv The arguments.
 * @param load Reads the market's inputs.
 * @param make Builds the strategy.
 * @return The process exit.
 */
template <typename Program, typename Load, typename Make>
int run_main(int argc, char** argv, Load load, Make make) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  (void)load;
  (void)make;
  return 1;
#else
  // Detached, the run has no cmdatl main: SIGINT and SIGTERM are noted, and the guard stops the run at its next step.
  handle_signals();
  json::object base{{"env", std::string(atl::mock::environment)}};
  // Row 1: the arguments.
  RunArgs args;
  if (const auto problem = read_args(argc, argv, args)) {
    return run_error(base, Exit::USAGE, "usage.bad_value", *problem);
  }
  base["run"] = args.id_;
  // Row 2: session legs have no live source.
  if (Program::atls_legs) {
    return run_error(base, Exit::PROGRAM, "program.live_unsupported",
                     "the program has on_open/on_close (session legs), which no live source delivers",
                     "run it with cmdatl strategy backtest; a run takes programs without session legs", {{"unsupported", "legs"}});
  }
  // Row 3: the market's settings.
  if (const auto refusal = spec_refusal(args.atlc_)) {
    return run_error(base, Exit::USAGE, "spec.invalid", refusal->second, "", {{"key", refusal->first}});
  }
  // Rows 4 and 7: the data, through atlc's own loader.
  Loaded loaded;
  try {
    loaded = load(args.atlc_);
  } catch (const std::invalid_argument& error) {
    return run_error(base, Exit::USAGE, "spec.invalid", error.what());
  } catch (const std::out_of_range& error) {
    return run_error(base, Exit::USAGE, "spec.invalid", error.what());
  }
  if (loaded.symbols_.empty() || loaded.series_.size() != loaded.symbols_.size() ||
      loaded.instruments_.size() != loaded.symbols_.size()) {
    return run_error(base, Exit::USAGE, "spec.no_instruments", "the run's data names no instrument to trade",
                     "name the symbols in the run spec's [instruments], as the data names them");
  }
  std::optional<std::chrono::system_clock::time_point> first_end;
  std::optional<std::chrono::system_clock::time_point> last_end;
  for (const auto& series : loaded.series_) {
    if (series.interval_ != atl::BarInterval::ONE_DAY) {
      return run_error(base, Exit::FAILED, "unsupported.interval",
                       series.instrument_.symbol_ + "'s bars are not daily; a run trades daily bars");
    }
    for (const auto& bar : series.bars_) {
      const auto end = atl::strategy::bar_period_end(bar, series.interval_, series.stamp_);
      first_end = first_end ? std::min(*first_end, end) : end;
      last_end = last_end ? std::max(*last_end, end) : end;
    }
  }
  if (!first_end) {
    return run_error(base, Exit::USAGE, "spec.invalid", "the run's data holds no bar");
  }
  // Row 5: the strategy, through atlc's rules.
  std::vector<atl::InstrumentId> ids;
  for (const auto& instrument : loaded.instruments_) {
    ids.push_back(instrument.id_);
  }
  std::optional<Program> strategy;
  try {
    strategy.emplace(make(args.atlc_, ids));
  } catch (const std::invalid_argument& error) {
    return run_error(base, Exit::PROGRAM, "program.invalid", error.what());
  } catch (const std::out_of_range& error) {
    return run_error(base, Exit::PROGRAM, "program.invalid", error.what());
  }
  // Row 6: the warm-up, compared before any arithmetic.
  const std::size_t warmup = strategy->warmup_bars();
  if (warmup > MAX_WARMUP_BARS) {
    return run_error(base, Exit::PROGRAM, "program.live_unsupported",
                     "the program warms up on " + std::to_string(warmup) + " bars, more than a run reads (" +
                       std::to_string(MAX_WARMUP_BARS) + ")",
                     "shorten the program's warm-up", {{"unsupported", "warmup"}});
  }
  // Row 8: the operator's file, one snapshot for the whole run.
  const auto snapshot = load_operator_config();
  if (snapshot.state_ != ConfigSnapshot::State::OK) {
    return relay(base, config_reply(snapshot));
  }
  const auto& config = *snapshot.config_;
  // Row 9: the operator's ceilings against the spec.
  for (const auto& symbol : loaded.symbols_) {
    if (!config.allows(symbol)) {
      return run_error(base, Exit::REFUSED, "refused.symbol_not_allowed", symbol + " is not in the operator's limits.symbols",
                       "", {{"symbol", symbol}});
    }
  }
  const auto order_type = atlc_value(args.atlc_, "orders.type").value_or("market");
  if (config.limits_.order_type_ == "limit" && order_type != "limit") {
    return run_error(base, Exit::REFUSED, "refused.order_type",
                     "the run spec's [orders] type is " + order_type + "; the operator allows limit orders only",
                     "set [orders] type = \"limit\" and an offset within the operator's collar");
  }
  if (const auto offset = atlc_value(args.atlc_, "orders.offset")) {
    double value = 0;
    try {
      value = std::stod(*offset);
    } catch (const std::exception&) {
      return run_error(base, Exit::USAGE, "spec.invalid", "orders.offset " + *offset + " is not a number");
    }
    if (!(value <= config.limits_.collar_)) {
      return run_error(base, Exit::REFUSED, "refused.collar",
                       "the run spec's [orders] offset " + *offset + " is above the operator's collar " + decimal_text(config.limits_.collar_));
    }
  }
  // Row 10: the run's own lock, in the directory `run start` made.
  const auto dir = run_dir(config, args.id_);
  std::error_code ec;
  if (!dir || !std::filesystem::is_directory(*dir, ec)) {
    return run_error(base, Exit::FAILED, "file.unwritable", "the run's directory does not exist; run start makes it");
  }
  OwnerLock lock(*dir);
  const auto taken = lock.acquire(std::chrono::milliseconds(0), [] { return interrupted(); }, [](const std::string&) {});
  if (taken == OwnerLock::Outcome::BUSY) {
    return run_error(base, Exit::FAILED, "busy.locked", "another driver holds this run's lock", "cmdatl run status " + args.id_);
  }
  if (taken != OwnerLock::Outcome::HELD) {
    return run_error(base, Exit::FAILED, "file.unwritable", "the run's lock could not be taken: " + lock.error());
  }
  bool trading = false;
  try {
    if (!write_owner_record(lock, "run " + args.id_)) {
      return relay(base, lock_lost_reply());
    }
    // Row 11: the armed file.
    if (!std::filesystem::exists(config.armed_path(), ec)) {
      return run_error(base, Exit::REFUSED, "refused.not_armed", "the operator's armed file does not exist");
    }
    // Row 12, first part: the run's journal and event log, both new.
    const auto journal_path = *dir + "/journal.jsonl";
    if (std::filesystem::exists(journal_path, ec) || std::filesystem::exists(*dir + "/events.jsonl", ec)) {
      return run_error(base, Exit::FAILED, "file.unwritable", "the run's directory already holds a journal or an event log");
    }
    const auto scope = config.broker_ + ":" + config.number_ + ":run:" + args.id_;
    atl::runner::FileJournal journal(journal_path, scope);
    if (!journal.open()) {
      return run_error(base, Exit::FAILED, "file.unwritable", "the run's journal could not be created: " + journal.rejection());
    }
    EventLog events(*dir + "/events.jsonl");
    if (!events.ok()) {
      return run_error(base, Exit::FAILED, "file.unwritable", "the run's event log could not be created in " + *dir);
    }
    // The market: the data, the instruments, the cash and the costs, starting at the first bar's end.
    atl::mock::MarketSettings settings;
    settings.cash_ = atl::Money{loaded.cash_, atl::Currency::krw()};
    settings.start_ = *first_end;
    settings.fills_ = loaded.fills_;
    atl::mock::Market market(loaded.series_, settings, loaded.instruments_);
    auto client = std::make_shared<atl::mock::MockClient>(market);
#if defined(CMDATL_RUN_TEST_HOOKS)
    if (client_hook()) {
      client_hook()(*client);
    }
#endif
    atl::BlockingSession<atl::mock::MockClient> inner(client, atl::mock::MockAccount{});
    atl::runner::StopSource stop;
    Guard guard;
    guard.armed_ = config.armed_path();
    guard.stop_file_ = config.stop_path();
    guard.run_ = args.id_;
    guard.lock_ = &lock;
    guard.stop_ = &stop;
    guard.events_ = &events;
    guard.end_ = *last_end + STEP;
    Ledger ledger;
    ledger.cash_ = loaded.cash_;
    PlacementPolicy policy;
    policy.armed_ = config.armed_path();
    policy.lock_ = &lock;
    policy.limit_only_ = config.limits_.order_type_ == "limit";
    policy.collar_ = config.limits_.collar_;
    for (const auto& name : config.limits_.phases_) {
      if (const auto phase = orders_detail::phase_named(name)) {
        policy.phases_.push_back(*phase);
      }
    }
    policy.window_ = settings.session_;
    GuardedSession<atl::BlockingSession<atl::mock::MockClient>> session(
      inner,
      policy,
      [&market]() { return market.now(); },
      [&guard, &ledger](const PlacementEvent& placed) {
        json::object event{
          {"event", placed.kind_},
          {"at", at_text(placed.at_)},
          {"symbol", placed.order_.instrument_.symbol_},
          {"side", placed.order_.side_ == atl::OrderSide::BUY ? "buy" : "sell"},
          {"quantity", placed.order_.quantity_.str()},
        };
        if (placed.order_.price_) {
          event["price"] = placed.order_.price_->str();
        }
        if (placed.kind_ == "placed") {
          event["order_id"] = placed.order_id_;
          ledger.open_.insert(placed.order_id_);
        } else {
          event["reason"] = placed.reason_;
        }
        guard.emit(std::move(event));
      }
    );
    std::optional<std::chrono::steady_clock::time_point> heartbeat;
    const auto after_step = [&]() {
      scan_orders(market, ledger, [&session](const std::string& id) { return session.cancelled(id); }, guard);
      const auto wall = std::chrono::steady_clock::now();
      if (!heartbeat || wall - *heartbeat >= std::chrono::seconds(1)) {
        heartbeat = wall;
        guard.emit(json::object{{"event", "clock"}, {"at", at_text(market.now())}});
      }
      guard.check(market.now());
    };
    RunClock clock(market, *last_end + BOUND_AFTER_LAST, after_step);
    Wiring wiring;
    wiring.universe_ = ids;
    wiring.limits_ = risk_limits(config, loaded.symbols_, config.limits_.daily_orders_);
    wiring.schedule_.tick_ = STEP;
    wiring.schedule_.calendar_ = STEP;
    wiring.schedule_.bars_ = STEP;
    wiring.schedule_.holdings_ = STEP;
    wiring.schedule_.working_orders_ = STEP;
    wiring.schedule_.buying_power_ = STEP;
    wiring.bar_count_ = static_cast<int>(std::max<std::size_t>(warmup, 1));
    wiring.maximum_portfolio_age_ = std::chrono::seconds(config.limits_.portfolio_age_);
    wiring.window_ = settings.session_;
    const auto from = market.now();
    const auto on_ready = [&]() {
      // The guard once before the first pass; its events are held until after the start line.
      guard.check(market.now());
      json::object start{
        {"event", "start"},
        {"v", EVENT_FORMAT},
        {"at", at_text(market.now())},
        {"env", base["env"]},
        {"run", args.id_},
        {"cap", wiring.limits_.max_position_quantity_ ? wiring.limits_.max_position_quantity_->str() : "0"},
        {"orders_per_day", config.limits_.daily_orders_},
        {"warmup_bars", warmup},
        {"from", at_text(*first_end).substr(0, 10)},
        {"to", at_text(*last_end).substr(0, 10)},
        {"cash", loaded.cash_.str()},
      };
      json::array listed;
      for (const auto& symbol : loaded.symbols_) {
        listed.push_back(json::value(symbol));
      }
      start["symbols"] = listed;
      const auto identity = lock.identity();
      start["lock"] = json::object{{"device", static_cast<std::int64_t>(identity.dev_)}, {"inode", static_cast<std::int64_t>(identity.ino_)}};
      if (!events.write(start)) {
        return false;
      }
      trading = true;
      guard.release_held();
      return true;
    };
    Traded traded;
    std::string thrown;
    std::string thrown_code;
    try {
      traded = trade_on_rails(
        session, journal, std::move(*strategy), wiring, stop, clock, on_ready,
        [&market]() { return market.working_orders().orders_.empty(); }, std::cerr
      );
    } catch (const RunBound& error) {
      thrown = error.what();
      thrown_code = "unknown.closing_incomplete";
    } catch (const SumOverflow& error) {
      thrown = error.what();
      thrown_code = "unknown.internal";
    } catch (const std::runtime_error& error) {
      if (!market.halted()) {
        throw;
      }
      thrown = std::string("the market halted: ") + *market.halted();
      thrown_code = "unknown.market_halted";
    }
    if (!thrown.empty() && !trading) {
      return run_error(base, Exit::FAILED, "internal.error", thrown);
    }
    switch (traded.outcome_) {
      case Outcome::ON_FILL_UNSUPPORTED:
        if (thrown.empty()) {
          return run_error(base, Exit::PROGRAM, "program.live_unsupported", traded.detail_,
                           "a run takes programs without on_fill", {{"unsupported", "on_fill"}});
        }
        break;
      case Outcome::RECOVERY_HALTED:
        if (thrown.empty()) {
          return run_error(base, Exit::UNKNOWN, "unknown.recovery_halted", traded.detail_);
        }
        break;
      case Outcome::REFERENCE_UNREADABLE:
        if (thrown.empty()) {
          return run_error(base, Exit::FAILED, "run.reference_unreadable", traded.detail_, "nothing was sent; start the run again");
        }
        break;
      case Outcome::NOT_STARTED:
        if (thrown.empty()) {
          return run_error(base, Exit::FAILED, "file.unwritable", "the run's start line could not be written; nothing was sent");
        }
        break;
      default:
        break;
    }
    // The ending, mapped.
    json::object body = base;
    Exit exit = Exit::OK;
    std::string code;
    std::string message = traded.detail_;
    if (!thrown.empty()) {
      exit = Exit::UNKNOWN;
      code = thrown_code;
      message = thrown;
    } else if (traded.outcome_ != Outcome::CLEAN) {
      exit = Exit::UNKNOWN;
      code = std::string("unknown.") + outcome_name(traded.outcome_);
    }
    body["outcome"] = thrown.empty() ? outcome_name(traded.outcome_) : "stopped_by_error";
    // The journal as the library restores it, read again from the file.
    const auto view = journal_view(session, atl::runner::read_journal_file(journal_path), scope, 0);
    if (exit == Exit::OK && guard.lock_lost_) {
      exit = Exit::UNKNOWN;
      code = "unknown.lock_lost";
      message = "the run's lock file was replaced during the run";
    }
    if (exit == Exit::OK && (!view.owned_ || !view.intact_)) {
      exit = Exit::UNKNOWN;
      code = "unknown.journal_damaged";
      message = view.detail_.empty() ? "the run's journal is not intact after the run" : view.detail_;
    }
    if (exit == Exit::OK && !view.owed_.empty()) {
      exit = Exit::UNKNOWN;
      code = "unknown.owed_at_end";
      message = "the run's journal still owes an order after the run";
    }
    if (exit == Exit::OK && !lock.still_at_path()) {
      exit = Exit::UNKNOWN;
      code = "unknown.lock_lost";
      message = "the run's lock file was replaced";
    }
    // The last scan: the closing pass's cancellations happen after the last step.
    scan_orders(market, ledger, [&session](const std::string& id) { return session.cancelled(id); }, guard);
    body["stopped"] = guard.stopped_.empty() ? json::value(nullptr) : json::value(guard.stopped_);
    body["from"] = at_text(from);
    body["to"] = at_text(*last_end).substr(0, 10);
    body["ended_at"] = at_text(market.now());
    body["executions"] = ledger.executions_;
    body["commission_paid"] = ledger.commission_.str();
    body["tax_paid"] = ledger.tax_.str();
    body["opening_equity"] = loaded.cash_.str();
    auto equity = ledger.cash_;
    json::object holdings;
    for (std::size_t k = 0; k < loaded.symbols_.size(); ++k) {
      holdings[loaded.symbols_[k]] = "0";
    }
    const auto held = market.holdings();
    if (!held) {
      throw SumOverflow();
    }
    for (const auto& position : held.value().positions_) {
      holdings[position.instrument_.symbol_] = position.quantity_.str();
      const auto quote = market.quote(position.instrument_);
      if (!quote) {
        throw SumOverflow();
      }
      equity = add_exact(equity, multiply_exact(position.quantity_, quote.value().last_price_));
    }
    body["closing_equity"] = equity.str();
    body["holdings_at_end"] = holdings;
    json::array owed;
    for (const auto& o : view.owed_) {
      owed.push_back(json::object{{"symbol", o.symbol_}, {"side", o.side_}, {"quantity", o.quantity_}, {"journal_id", o.journal_id_},
                                  {"order_id", o.order_id_}, {"state", o.state_}});
    }
    body["owed_at_end"] = owed;
    if (exit != Exit::OK) {
      body["error"] = json::object{
        {"code", code},
        {"message", message},
        {"next", "the run's market ended with its process and no account was affected; its result is unknown: start a new run"},
      };
    }
    // The reply first, then the end event, then the lock released (when this scope ends).
    const int exit_code = write_run_reply(body, exit);
    (void)events.write(json::object{{"event", "end"}, {"at", at_text(market.now())}, {"exit", exit_code}, {"result", body["result"]}});
    return exit_code;
  } catch (const std::exception& error) {
    if (trading) {
      return run_error(base, Exit::UNKNOWN, "unknown.internal", error.what(),
                       "the run's market ended with its process and no account was affected; its result is unknown: start a new run");
    }
    return run_error(base, Exit::FAILED, "internal.error", error.what());
  }
#endif
}

}  // namespace driver
}  // namespace cmdatl
