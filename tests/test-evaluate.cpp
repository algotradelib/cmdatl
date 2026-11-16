// Many replays in one driver (include/cmdatl/replay/evaluate.hpp): the index order, the lines, the choice of the best,
// the runner on threads (order, the waiting-line bound, stops that join every thread), and the stdin bar source. The
// `[threads]` cases also run under ThreadSanitizer (`docs/run-spec.md`, "Many replays"), so the program here keeps its
// state per instance; the few atomics below are what the tests observe.
#include <cmdatl/replay/evaluate.hpp>

#include "./run-strategy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

namespace replay = cmdatl::replay;
namespace atl = algotradelib;

/** @brief What the tests observe of the replays running: set by replay k = 0 while it holds, read by the others. */
struct Observed {
  std::atomic<bool> holding_{false};
  std::atomic<long> highest_while_held_{-1};
};

Observed& observed() {
  static Observed o;
  return o;
}

/**
 * @brief A program whose parameters say what it does: it buys `k * m` shares at market on its second bar; replay
 *   `k = 0` sleeps `hold` ms on its first bar (`holding_` set meanwhile) and then fails as `fail` says (1
 *   `std::bad_alloc`, 2 `throw 7`, 3 `std::runtime_error`); `k` below 0 is a refused bound.
 */
struct Ticker {
  static constexpr const char* atls_mode = "named";
  static constexpr bool atls_session = false;
  static std::vector<std::string> atls_instruments() {
    return {"x"};
  }

  explicit Ticker(std::vector<atl::InstrumentId> ids) : ids_(std::move(ids)) {}

  std::size_t warmup_bars() const {
    return 1;
  }

  void set(std::string_view name, double value) {
    if (name == "k") {
      k_ = value;
    } else if (name == "m") {
      m_ = value;
    } else if (name == "hold") {
      hold_ = value;
    } else if (name == "fail") {
      fail_ = value;
    } else {
      throw std::invalid_argument("the program has no parameter '" + std::string(name) + "'");
    }
    if (k_ < 0) {
      throw std::invalid_argument("k is below 0");
    }
  }

  void set_order_pricing(atl::OrderType, const atl::Decimal64&) {}

  const runtest::Refusals& pricing_refusals() const {
    return refusals_;
  }

  template <class Ctx>
  void on_bar(Ctx& ctx, const atl::strategy::BarEvent& event) {
    ++seen_;
    if (seen_ == 1) {
      behave();
    }
    if (seen_ == 2 && !ids_.empty() && event.instrument_ == ids_.front()) {
      atl::strategy::TargetPosition target;
      target.instrument_ = event.instrument_;
      target.target_quantity_ = atl::Decimal64(static_cast<std::int64_t>(k_ * m_), 0);
      target.tag_ = "ticker";
      ctx.submit(target);
    }
  }

private:
  void behave() const {
    if (k_ != 0) {
      if (observed().holding_) {
        auto highest = observed().highest_while_held_.load();
        while (k_ > highest && !observed().highest_while_held_.compare_exchange_weak(highest, static_cast<long>(k_))) {
        }
      }
      return;
    }
    if (hold_ > 0) {
      observed().holding_ = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(hold_)));
      observed().holding_ = false;
    }
    if (fail_ == 1) {
      throw std::bad_alloc();
    }
    if (fail_ == 2) {
      throw 7;
    }
    if (fail_ == 3) {
      throw std::runtime_error("the test's own failure");
    }
  }

  std::vector<atl::InstrumentId> ids_;
  runtest::Refusals refusals_;
  std::size_t seen_{0};
  double k_{0};
  double m_{1};
  double hold_{0};
  double fail_{0};
};

/** @brief A directory of its own for a test's files, removed with everything in it. */
class Scratch {
public:
  Scratch() {
    static std::atomic<int> count{0};
    path_ = std::filesystem::temp_directory_path() /
      ("cmdatl-evaluate-" + std::to_string(::getpid()) + "-" + std::to_string(count++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::string write(const std::string& name, const std::string& text) const {
    std::ofstream(path_ / name, std::ios::binary | std::ios::trunc) << text;
    return (path_ / name).string();
  }

private:
  std::filesystem::path path_;
};

/**
 * @brief Long CSV rows of one symbol, one bar a day from 2020-01-01, each bar's four prices one value.
 * @param symbol The symbol.
 * @param prices Each day's price.
 * @param extra A last column's cell on every row (`,CELL`), or empty for none.
 * @return The rows, without a header.
 */
std::string rows_of(const std::string& symbol, const std::vector<int>& prices, const std::string& extra = "") {
  std::string text;
  for (std::size_t day = 0; day < prices.size(); ++day) {
    const auto p = std::to_string(prices[day]);
    text += std::to_string(1577836800 + 86400 * static_cast<long long>(day)) + "," + symbol + "," + p + "," + p + "," +
      p + "," + p + ",1000" + (extra.empty() ? "" : "," + extra) + "\n";
  }
  return text;
}

/** @brief The header `rows_of` writes under. */
const std::string HEADER = "ts,symbol,open,high,low,close,volume";

/** @brief What a driver's main gave. */
struct Ran {
  int exit_{-1};
  std::string out_;
  std::string err_;
  double seconds_{0};
};

/**
 * @brief Runs `evaluate_main` (or `main`, without `evaluate.method`) on arguments, stdout and stderr captured.
 * @param args The arguments.
 * @param input The descriptor stdin bars are read from.
 * @param hooks The runner's hooks.
 * @return What it gave.
 */
template <typename Strategy = Ticker>
Ran drive(std::vector<std::string> args, int input = -1, const replay::RunnerHooks& hooks = {}) {
  std::vector<char*> argv{const_cast<char*>("driver")};
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  const auto s = replay::Settings::from_args(static_cast<int>(argv.size()), argv.data());
  auto fills = [](atl::backtest::NextBarFillModel model) {
    return model;
  };
  std::ostringstream out;
  std::ostringstream err;
  auto* const saved_out = std::cout.rdbuf(out.rdbuf());
  auto* const saved_err = std::cerr.rdbuf(err.rdbuf());
  Ran ran;
  const auto start = std::chrono::steady_clock::now();
  ran.exit_ = s.get("evaluate.method") ?
    replay::evaluate_main<Strategy>(s, fills, input, &hooks) :
    replay::main<Strategy>(static_cast<int>(argv.size()), argv.data(), fills);
  ran.seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout.rdbuf(saved_out);
  std::cerr.rdbuf(saved_err);
  ran.out_ = out.str();
  ran.err_ = err.str();
  return ran;
}

/**
 * @brief The lines of a text.
 * @param text The text.
 * @return Its lines.
 */
std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    out.push_back(line);
  }
  return out;
}

/** @brief Clears what the tests observe, before a test and after it. */
struct FreshObserved {
  FreshObserved() {
    observed().holding_ = false;
    observed().highest_while_held_ = -1;
  }
  FreshObserved(const FreshObserved&) = delete;
  FreshObserved& operator=(const FreshObserved&) = delete;
  ~FreshObserved() {
    observed().holding_ = false;
  }
};

/**
 * @brief The settings of an evaluation on a CSV of one symbol, `AAA`.
 * @param file The CSV.
 * @param more Further arguments.
 * @return The arguments.
 */
std::vector<std::string> on_csv(const std::string& file, std::vector<std::string> more) {
  std::vector<std::string> args{
    "bars.source=csv", "bars.file=" + file, "bars.symbol_column=symbol", "instrument.x=AAA", "evaluate.method=grid"
  };
  args.insert(args.end(), more.begin(), more.end());
  return args;
}

} // namespace

TEST_CASE("evaluate: the index order is names in byte order with the last name varying fastest") {
  const replay::Grid three{{"a", {"1", "2"}}, {"b", {"10", "20", "30"}}, {"c", {"7", "8"}}};
  CHECK(
    replay::grid_set(three, 0) == std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"b", "10"}, {"c", "7"}}
  );
  CHECK(
    replay::grid_set(three, 1) == std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"b", "10"}, {"c", "8"}}
  );
  CHECK(
    replay::grid_set(three, 2) == std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"b", "20"}, {"c", "7"}}
  );
  CHECK(
    replay::grid_set(three, 11) == std::vector<std::pair<std::string, std::string>>{{"a", "2"}, {"b", "30"}, {"c", "8"}}
  );
  CHECK(replay::set_count(three) == 12);
  CHECK(replay::grid_set({{"k", {"5", "6"}}}, 1) == std::vector<std::pair<std::string, std::string>>{{"k", "6"}});
  CHECK(replay::grid_set({}, 0).empty());
  CHECK(replay::set_count({}) == 1);
  // The driver's own reading: byte order whatever the arguments' order, each value a JSON number, no value twice.
  const auto grid = replay::grid_of(
    replay::Settings::from_args(
      3,
      std::vector<char*>{
        const_cast<char*>("d"), const_cast<char*>("grid.slow=60,90"), const_cast<char*>("grid.Fast=-0.5,1e2")
      }
        .data()
    )
  );
  REQUIRE(grid.size() == 2);
  CHECK(grid[0].first == "Fast");
  CHECK(grid[1].second == std::vector<std::string>{"60", "90"});
  for (const auto* bad : {"grid.k=1,1.0", "grid.k=0,-0", "grid.k=1,,2", "grid.k=01", "grid.k=1e999", "grid.k=nan"}) {
    INFO(bad);
    std::vector<char*> argv{const_cast<char*>("d"), const_cast<char*>(bad)};
    CHECK_THROWS_AS(replay::grid_of(replay::Settings::from_args(2, argv.data())), std::invalid_argument);
  }
  CHECK_THROWS_AS(replay::check_count(100001), std::invalid_argument);
  CHECK_NOTHROW(replay::check_count(100000));
}

TEST_CASE("evaluate: one set's line carries the single backtest's report on one line") {
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103, 102, 104}));
  const auto single =
    drive({"bars.source=csv", "bars.file=" + file, "bars.symbol_column=symbol", "instrument.x=AAA", "param.k=2"});
  REQUIRE(single.exit_ == 0);
  std::string flat;
  for (std::size_t at = 0; at < single.out_.size(); ++at) {
    if (single.out_[at] == '\n') {
      while (at + 1 < single.out_.size() && single.out_[at + 1] == ' ') {
        ++at;
      }
    } else {
      flat.push_back(single.out_[at]);
    }
  }
  const auto evaluated = drive(on_csv(file, {"grid.k=2"}));
  INFO(evaluated.err_);
  REQUIRE(evaluated.exit_ == 0);
  const auto lines = lines_of(evaluated.out_);
  REQUIRE(lines.size() == 2);
  CHECK(lines[0] == "{\"index\":0,\"params\":{\"k\":2},\"window\":null,\"path\":null,\"report\":" + flat + "}");
  CHECK(
    lines[1] ==
    "{\"summary\":{\"method\":\"grid\",\"jobs\":1,\"param_sets\":1,\"invalid\":0,\"tried\":1,\"replays\":1,"
    "\"paths\":null,\"failed\":0,\"objective\":null,\"direction\":null,\"best\":null}}"
  );
  // Without a grid: one replay, params {}.
  const auto plain = drive(on_csv(file, {"param.k=2"}));
  CHECK(
    lines_of(plain.out_).front() == "{\"index\":0,\"params\":{},\"window\":null,\"path\":null,\"report\":" + flat + "}"
  );
}

TEST_CASE("evaluate: a refused bound is a params failure and a replay that throws a replay failure") {
  const FreshObserved fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  const auto ran = drive(on_csv(file, {"grid.k=-1,0,1", "param.fail=3", "evaluate.objective=total_return"}));
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 4);
  CHECK(
    lines[0] ==
    "{\"index\":0,\"params\":{\"k\":-1},\"window\":null,\"path\":null,\"report\":null,\"failure\":{\"kind\":\"params\",\"message\":\"k is below 0\"}}"
  );
  CHECK(
    lines[1] ==
    "{\"index\":1,\"params\":{\"k\":0},\"window\":null,\"path\":null,\"report\":null,\"failure\":{\"kind\":\"replay\",\"message\":\"the test's own failure\"}}"
  );
  CHECK(lines[2].find("\"report\":{\"bars\": 3,") != std::string::npos);
  CHECK(
    lines[3].find("\"param_sets\":3,\"invalid\":1,\"tried\":2,\"replays\":3,\"paths\":null,\"failed\":1,") !=
    std::string::npos
  );
  CHECK(lines[3].find("\"best\":{\"index\":2,\"params\":{\"k\":1},\"value\":") != std::string::npos);
  // A session program that never met its session's edge is a replay failure, not a stop.
  const auto session = drive<runtest::SessionProgram>(on_csv(file, {"bars.open_point=true", "grid.entry=60,70"}));
  REQUIRE(session.exit_ == 0);
  CHECK(
    lines_of(session.out_)
      .at(1)
      .find("\"failure\":{\"kind\":\"replay\",\"message\":\"the program's on_open never ran") != std::string::npos
  );
}

TEST_CASE("evaluate: the best skips halted and undefined figures, ties to the lower index, either direction") {
  std::optional<replay::Best> best;
  const auto replay_of = [](std::size_t index) {
    return replay::Replay{index, {{"k", std::to_string(index)}}, std::nullopt, std::nullopt};
  };
  const auto figures = [](double value) {
    return std::vector<std::pair<std::string, double>>{{"total_return", value}};
  };
  replay::choose(best, "total_return", false, replay_of(0), figures(0.5), true);
  replay::choose(best, "total_return", false, replay_of(1), figures(std::nan("")), false);
  replay::choose(best, "total_return", false, replay_of(2), {}, false);
  CHECK(!best);
  replay::choose(best, "total_return", false, replay_of(4), figures(0.2), false);
  replay::choose(best, "total_return", false, replay_of(3), figures(0.2), false);
  replay::choose(best, "total_return", false, replay_of(5), figures(0.2), false);
  REQUIRE(best);
  CHECK(best->index_ == 3);
  replay::choose(best, "total_return", false, replay_of(6), figures(0.1), false);
  CHECK(best->index_ == 3);
  std::optional<replay::Best> least;
  replay::choose(least, "max_drawdown", true, replay_of(0), {{"max_drawdown", 0.3}}, false);
  replay::choose(least, "max_drawdown", true, replay_of(1), {{"max_drawdown", 0.1}}, false);
  CHECK(least->index_ == 1);
  CHECK(least->value_ == 0.1);
}

TEST_CASE("evaluate: one job and four print the same bytes and keep values in index order", "[threads]") {
  const FreshObserved fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103, 102, 104, 107}));
  const auto args = on_csv(file, {"grid.k=1,2,3,4", "grid.m=1,2,3", "evaluate.objective=total_return"});
  auto four = args;
  four.push_back("evaluate.jobs=4");
  const auto one = drive(args);
  const auto many = drive(four);
  REQUIRE(one.exit_ == 0);
  REQUIRE(many.exit_ == 0);
  CHECK(lines_of(one.out_).size() == 13);
  CHECK(one.out_.substr(0, one.out_.rfind("{\"summary\"")) == many.out_.substr(0, many.out_.rfind("{\"summary\"")));
  CHECK(many.out_.find("\"jobs\":4,") != std::string::npos);
  // What keep returns reaches the sink in index order, whatever thread ran it.
  std::vector<char*> argv{const_cast<char*>("d")};
  for (auto& arg : four) {
    argv.push_back(arg.data());
  }
  const auto s = replay::Settings::from_args(static_cast<int>(argv.size()), argv.data());
  for (const std::size_t jobs : {std::size_t{1}, std::size_t{4}}) {
    replay::GridSource source(std::make_shared<const replay::Prepared>(replay::prepare<Ticker>(s)), replay::grid_of(s));
    auto fills = [](atl::backtest::NextBarFillModel model) {
      return model;
    };
    auto keep = [](const atl::backtest::ReplayResult& result, const atl::backtest::NextBarFillModel&) {
      return result.executions_.empty() ? std::string() : result.executions_.front().fill_.quantity_.str();
    };
    auto work = [&](replay::Task task) {
      return replay::finish_one<Ticker>(s, std::move(task), fills, keep);
    };
    std::vector<std::pair<std::size_t, std::string>> seen;
    auto sink = [&seen](replay::Finished<std::string>&& done) {
      seen.emplace_back(done.replay_.index_, std::move(done.kept_));
    };
    replay::run_replays(source, jobs, work, sink);
    REQUIRE(seen.size() == 12);
    for (std::size_t i = 0; i < seen.size(); ++i) {
      CHECK(seen[i].first == i);
      CHECK(seen[i].second == std::to_string((i / 3 + 1) * (i % 3 + 1)));
    }
  }
}

TEST_CASE("evaluate: a slow first replay holds at most four lines per job", "[threads]") {
  const FreshObserved fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  std::string ks = "grid.k=0";
  for (int k = 1; k < 20; ++k) {
    ks += "," + std::to_string(k);
  }
  const auto ran = drive(on_csv(file, {ks, "param.hold=300", "evaluate.jobs=2"}));
  REQUIRE(ran.exit_ == 0);
  CHECK(lines_of(ran.out_).size() == 21);
  // While replay 0 held, the other thread started replays after it, and none past 4 * jobs - 1.
  CHECK(observed().highest_while_held_ >= 1);
  CHECK(observed().highest_while_held_ <= 7);
}

TEST_CASE("evaluate: a fatal throw stops the batch, joins every thread and prints the stop line", "[threads]") {
  const FreshObserved fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  std::string ks = "grid.k=0";
  for (int k = 1; k < 40; ++k) {
    ks += "," + std::to_string(k);
  }
  for (
    const auto& [fail, message] : std::vector<std::pair<std::string, std::string>>{
      {"1", "std::bad_alloc"}, {"2", "a replay threw something that is not a std::exception"}
    }
  ) {
    INFO(fail);
    const auto ran = drive(on_csv(file, {ks, "param.fail=" + fail, "evaluate.jobs=4"}));
    CHECK(ran.exit_ == 1);
    const auto lines = lines_of(ran.out_);
    REQUIRE(!lines.empty());
    CHECK(lines.back().rfind("{\"stopped\":{\"code\":\"replay.failed\",\"message\":", 0) == 0);
    CHECK(ran.out_.find("\"summary\"") == std::string::npos);
    CHECK(ran.err_.rfind("error: ", 0) == 0);
    if (fail == "2") {
      CHECK(ran.err_ == "error: " + message + "\n");
    }
  }
  // A worker already waiting at admission (index 8 behind a held index 0, two jobs) is woken and dropped.
  const auto waiting = drive(on_csv(file, {ks, "param.hold=200", "param.fail=1", "evaluate.jobs=2"}));
  CHECK(waiting.exit_ == 1);
  CHECK(waiting.seconds_ < 1.0);
  CHECK(waiting.out_.find("\"stopped\"") != std::string::npos);
  // The single backtest answers a throw that is not a std::exception with its error line.
  const auto single =
    drive({"bars.source=csv", "bars.file=" + file, "bars.symbol_column=symbol", "instrument.x=AAA", "param.fail=2"});
  CHECK(single.exit_ == 1);
  CHECK(single.err_ == "error: the replay threw something that is not a std::exception\n");
}

TEST_CASE("evaluate: a thread that cannot start stops and joins those started", "[threads]") {
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  replay::RunnerHooks hooks;
  hooks.starting_ = [](std::size_t k) {
    if (k == 2) {
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "thread");
    }
  };
  const auto ran = drive(on_csv(file, {"grid.k=1,2,3,4,5,6,7,8", "evaluate.jobs=4"}), -1, hooks);
  CHECK(ran.exit_ == 1);
  CHECK(ran.out_.find("{\"stopped\":{\"code\":\"replay.failed\",\"message\":\"thread: ") != std::string::npos);
}
