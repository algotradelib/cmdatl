// Many replays in one driver (include/cmdatl/replay/evaluate.hpp, loops.hpp): the index order, the lines, the choice of
// the best, the runner on threads (order, the waiting-line bound, stops that join every thread), and the methods
// windows, walk-forward and bootstrap. The `[threads]` cases also
// run under ThreadSanitizer (a build with `-fsanitize=thread`, `test-replay "[threads]"`), so the program here keeps its
// state per instance; the few atomics below are what the tests observe.
#include <cmdatl/replay/evaluate.hpp>

#include "./run-strategy.hpp"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
#include <tuple>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
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

/** @brief A table of measures, as the driver's `program_measures()` gives it. */
using Measures = std::vector<std::pair<const char*, double (*)(const atl::backtest::MeasureInput&)>>;

/** @brief A measure: the replay's executions. */
double executions(const atl::backtest::MeasureInput& in) {
  return static_cast<double>(in.report_.executions_);
}

/** @brief A measure that throws on a replay without executions. */
double picky(const atl::backtest::MeasureInput& in) {
  if (in.report_.executions_ == 0) {
    throw std::runtime_error("no executions");
  }
  return 1;
}

/** @brief A measure that runs out of memory on a replay without executions. */
double exhausting(const atl::backtest::MeasureInput& in) {
  if (in.report_.executions_ == 0) {
    throw std::bad_alloc();
  }
  return 1;
}

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
 * @param measures The report's measures.
 * @return What it gave.
 */
template <typename Strategy = Ticker>
Ran drive(
  std::vector<std::string> args, int input = -1, const replay::RunnerHooks& hooks = {}, const Measures& measures = {}
) {
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
    replay::evaluate_main<Strategy>(s, fills, measures, input, &hooks) :
    replay::main<Strategy>(static_cast<int>(argv.size()), argv.data(), fills, [&measures]() {
      return measures;
    });
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
      return replay::finish_one<Ticker>(s, std::move(task), fills, Measures{}, keep);
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

TEST_CASE("evaluate: a stop with a worker waiting at admission returns within a second", "[.][local][threads]") {
  const FreshObserved fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  std::string ks = "grid.k=0";
  for (int k = 1; k < 40; ++k) {
    ks += "," + std::to_string(k);
  }
  const auto waiting = drive(on_csv(file, {ks, "param.hold=200", "param.fail=1", "evaluate.jobs=2"}));
  CHECK(waiting.exit_ == 1);
  CHECK(waiting.seconds_ < 1.0);
}

TEST_CASE("evaluate: a sink that throws stops the batch and is never called again", "[threads]") {
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  auto args = on_csv(file, {"grid.k=1,2,3,4,5,6,7,8,9,10,11,12"});
  std::vector<char*> argv{const_cast<char*>("d")};
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  const auto s = replay::Settings::from_args(static_cast<int>(argv.size()), argv.data());
  replay::GridSource source(std::make_shared<const replay::Prepared>(replay::prepare<Ticker>(s)), replay::grid_of(s));
  auto fills = [](atl::backtest::NextBarFillModel model) {
    return model;
  };
  auto keep = [](const atl::backtest::ReplayResult&, const atl::backtest::NextBarFillModel&) {
    return std::string("kept");
  };
  auto work = [&](replay::Task task) {
    return replay::finish_one<Ticker>(s, std::move(task), fills, Measures{}, keep);
  };
  std::vector<std::string> seen;
  auto sink = [&seen](replay::Finished<std::string>&& done) {
    seen.push_back(std::move(done.kept_));
    throw std::runtime_error("the sink broke");
  };
  CHECK_THROWS_AS(replay::run_replays(source, 2, work, sink), std::runtime_error);
  CHECK(seen == std::vector<std::string>{"kept"});
}

TEST_CASE("evaluate: each line's report carries the measures, which an objective may name") {
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  const auto ran = drive(
    on_csv(file, {"grid.k=0,1,2", "evaluate.objective=measures.executions", "evaluate.jobs=2"}), -1, {},
    {{"executions", executions}}
  );
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 4);
  CHECK(
    lines[0].find("\"last_bar\": \"2020-01-03T00:00:00Z\",\"measures\": {\"executions\": 0}}") != std::string::npos
  );
  CHECK(lines[1].find("\"measures\": {\"executions\": 1}") != std::string::npos);
  CHECK(
    lines[3].find("\"objective\":\"measures.executions\",\"direction\":\"max\",\"best\":{\"index\":1,") !=
    std::string::npos
  );
  // The report's own count, as bootstrap.intervals names it, chooses the same.
  const auto counted = drive(on_csv(file, {"grid.k=0,1,2", "evaluate.objective=executions"}));
  REQUIRE(counted.exit_ == 0);
  CHECK(
    lines_of(counted.out_)[3].find("\"objective\":\"executions\",\"direction\":\"max\",\"best\":{\"index\":1,") !=
    std::string::npos
  );
}

TEST_CASE("evaluate: a measure's exception fails its replay and its bad_alloc stops the batch", "[threads]") {
  Scratch dir;
  const auto file = dir.write("bars.csv", HEADER + "\n" + rows_of("AAA", {100, 101, 103}));
  const auto args = on_csv(file, {"grid.k=0,1,2,3", "evaluate.jobs=2"});
  const auto picked = drive(args, -1, {}, {{"picky", picky}});
  REQUIRE(picked.exit_ == 0);
  const auto lines = lines_of(picked.out_);
  REQUIRE(lines.size() == 5);
  CHECK(
    lines[0].find(
      "\"report\":null,\"failure\":{\"kind\":\"replay\",\"message\":\"the measure picky threw: no executions\"}"
    ) != std::string::npos
  );
  CHECK(lines[1].find("\"measures\": {\"picky\": 1}") != std::string::npos);
  CHECK(lines[4].find("\"failed\":1,") != std::string::npos);
  const auto exhausted = drive(args, -1, {}, {{"exhausting", exhausting}});
  CHECK(exhausted.exit_ == 1);
  CHECK(lines_of(exhausted.out_).back() == "{\"stopped\":{\"code\":\"replay.failed\",\"message\":\"std::bad_alloc\"}}");
}

#if !defined(_WIN32)

namespace {

/**
 * @brief A pipe holding a text, written whole and closed before the reader's first look (the text fits the pipe).
 * @param text The text.
 * @return The read end.
 */
int closed_pipe(const std::string& text) {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  REQUIRE(::write(fds[1], text.data(), text.size()) == static_cast<ssize_t>(text.size()));
  ::close(fds[1]);
  return fds[0];
}

/**
 * @brief Runs an evaluation reading a text on a descriptor: a pipe, or a file when the text does not fit one.
 * @param text The text.
 * @param more Further arguments.
 * @return What it gave.
 */
Ran on_stdin(const std::string& text, std::vector<std::string> more) {
  Scratch dir;
  const int fd = text.size() < 60000 ? closed_pipe(text) : ::open(dir.write("in.csv", text).c_str(), O_RDONLY);
  std::vector<std::string> args{"bars.source=stdin", "bars.symbol_column=symbol", "instrument.x=AAA"};
  args.insert(args.end(), more.begin(), more.end());
  args.push_back("evaluate.method=grid");
  auto ran = drive(args, fd);
  ::close(fd);
  return ran;
}

/** @brief The header of rows with a path column. */
const std::string PATHS = HEADER + ",path\n";

} // namespace

TEST_CASE("evaluate: stdin paths end where the path cell changes and a reused cell is a new path") {
  // Written and closed before the first poll, the last line without its newline.
  auto text = PATHS + rows_of("AAA", {100, 101, 103}, "a") + rows_of("AAA", {50, 49}, "b") + rows_of("AAA", {7}, "a");
  text.pop_back();
  const auto ran = on_stdin(text, {"bars.path_column=path", "grid.k=1,2", "evaluate.objective=total_return"});
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 7);
  const char* paths[] = {
    "0,\"seed\":null,\"id\":\"a\"", "1,\"seed\":null,\"id\":\"b\"", "2,\"seed\":null,\"id\":\"a\""
  };
  for (std::size_t i = 0; i < 6; ++i) {
    INFO(lines[i]);
    CHECK(
      lines[i].rfind("{\"index\":" + std::to_string(i) + ",\"params\":{\"k\":" + std::to_string(i % 2 + 1), 0) == 0
    );
    CHECK(lines[i].find(std::string("\"path\":{\"number\":") + paths[i / 2] + "}") != std::string::npos);
  }
  CHECK(lines[5].find("\"report\":{\"bars\": 1,") != std::string::npos);
  CHECK(lines[6].find("\"replays\":6,\"paths\":3,") != std::string::npos);
  CHECK(lines[6].find("\"best\":null") != std::string::npos);
  // A refused bound counts once per set, whatever the paths.
  const auto refused = on_stdin(text, {"bars.path_column=path", "grid.k=-1,1"});
  CHECK(
    lines_of(refused.out_).back().find("\"invalid\":1,\"tried\":1,\"replays\":6,\"paths\":3,") != std::string::npos
  );
  // A path's report is the report of the same rows read as a CSV.
  Scratch dir;
  const auto csv = drive(on_csv(dir.write("a.csv", HEADER + "\n" + rows_of("AAA", {50, 49})), {"grid.k=1,2"}));
  REQUIRE(csv.exit_ == 0);
  const auto report = [](const std::string& line) {
    return line.substr(line.find("\"report\":"));
  };
  CHECK(report(lines[3]) == report(lines_of(csv.out_)[1]));
  // Without bars.path_column the stream is one path, its id null.
  auto rows = rows_of("AAA", {100, 101, 102}, "a");
  rows[rows.size() - 2] = 'b';
  const auto one = on_stdin(PATHS + rows, {});
  CHECK(
    lines_of(one.out_).at(0).find("\"path\":{\"number\":0,\"seed\":null,\"id\":null},\"report\":{\"bars\": 3,") !=
    std::string::npos
  );
  CHECK(lines_of(one.out_).at(1).find("\"paths\":1,") != std::string::npos);
}

TEST_CASE("evaluate: a stdin path over 1 MiB arrives whole through many reads") {
  std::vector<int> prices(30000);
  for (std::size_t i = 0; i < prices.size(); ++i) {
    prices[i] = 100 + static_cast<int>(i % 50);
  }
  const auto text = HEADER + "\n" + rows_of("AAA", prices);
  REQUIRE(text.size() > replay::STDIN_LINE_MAX);
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  std::thread writer([&text, fd = fds[1]]() {
    for (std::size_t at = 0; at < text.size();) {
      const auto n = ::write(fd, text.data() + at, std::min<std::size_t>(text.size() - at, 1000003));
      if (n <= 0) {
        break;
      }
      at += static_cast<std::size_t>(n);
    }
    ::close(fd);
  });
  const auto ran = drive({"bars.source=stdin", "bars.symbol=AAA", "instrument.x=AAA", "evaluate.method=grid"}, fds[0]);
  writer.join();
  ::close(fds[0]);
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  CHECK(ran.out_.find("\"report\":{\"bars\": 30000,") != std::string::npos);
}

TEST_CASE("evaluate: a broken stdin stream prints the stop line naming its line after the lines printed") {
  const std::string long_cell(replay::STDIN_LINE_MAX + 1, '1');
  const std::vector<std::tuple<std::string, std::vector<std::string>, std::string>> cases{
    {PATHS + rows_of("AAA", {1}, "a") + rows_of("BBB", {1}, "b"), {"bars.path_column=path"},
      "stdin:3: the path from here carries other symbols than the first path"},
    {PATHS + rows_of("AAA", {1}, " "), {"bars.path_column=path"}, "stdin:2: a path cell is 1 to 64 bytes"},
    {PATHS + rows_of("AAA", {1}, std::string(65, 'p')), {"bars.path_column=path"}, "a path cell is 1 to 64 bytes"},
    {PATHS + rows_of("AAA", {1}, "a") + long_cell, {"bars.path_column=path"}, "stdin:3: a line longer than 1 MiB"},
    {PATHS + "1577836800,AAA,1,1,1\n", {}, "stdin:2: expected 8 cells, found 5"},
    {PATHS + rows_of("AAA", {1}, "a"), {"bars.path_column=id"}, "stdin: no column 'id' (bars.path_column)"},
    {PATHS + rows_of("AAA", {1}, "a"), {"bars.path_column=path", "replay.from=2021-01-01"},
      "stdin:2: the path from here has no bar"},
    {PATHS, {}, "stdin: no path: standard input holds no row"},
    {"", {}, "stdin:1: no header: standard input is empty"},
  };
  for (const auto& [text, more, message] : cases) {
    INFO(message);
    const auto ran = on_stdin(text, more);
    CHECK(ran.exit_ == 1);
    const auto lines = lines_of(ran.out_);
    REQUIRE(!lines.empty());
    CHECK(lines.back().rfind("{\"stopped\":{\"code\":\"replay.failed\",\"message\":\"", 0) == 0);
    CHECK(lines.back().find(message) != std::string::npos);
    CHECK(ran.out_.find("\"summary\"") == std::string::npos);
  }
  // A device macOS cannot poll is read all the same; a descriptor that is not open is a broken stream.
  const int null = ::open("/dev/null", O_RDONLY);
  const std::vector<std::string> args{
    "bars.source=stdin", "bars.symbol=AAA", "instrument.x=AAA", "evaluate.method=grid"
  };
  CHECK(drive(args, null).out_.find("stdin:1: no header: standard input is empty") != std::string::npos);
  ::close(null);
  CHECK(drive(args, null).out_.find("stdin:1: standard input is not open") != std::string::npos);
  CHECK(drive(args, -1).out_.find("stdin:1: standard input is not open") != std::string::npos);
  // The first path's line stands before the stop line.
  const auto first = on_stdin(std::get<0>(cases[0]), {"bars.path_column=path"});
  CHECK(lines_of(first.out_).at(0).rfind("{\"index\":0,", 0) == 0);
  // One backtest does not read standard input.
  const auto single = drive({"bars.source=stdin", "bars.symbol=AAA", "instrument.x=AAA"});
  CHECK(single.exit_ == 1);
  CHECK(single.err_.find("bars.source = stdin is read by an evaluation") != std::string::npos);
}

TEST_CASE("evaluate: one job and four print the same bytes on stdin paths", "[threads]") {
  const auto text =
    PATHS + rows_of("AAA", {100, 101, 103}, "a") + rows_of("AAA", {50, 49, 48}, "b") + rows_of("AAA", {7, 8}, "c");
  const auto one = on_stdin(text, {"bars.path_column=path", "grid.k=1,2,3"});
  const auto four = on_stdin(text, {"bars.path_column=path", "grid.k=1,2,3", "evaluate.jobs=4"});
  REQUIRE(one.exit_ == 0);
  REQUIRE(four.exit_ == 0);
  CHECK(lines_of(one.out_).size() == 10);
  CHECK(one.out_.substr(0, one.out_.rfind("{\"summary\"")) == four.out_.substr(0, four.out_.rfind("{\"summary\"")));
}

namespace {

/** @brief What a silent producer's evaluation gave, and whether the test had to close the pipe to end it. */
struct Silent {
  Ran ran_;
  bool closed_by_test_{false};
};

/**
 * @brief Path A and one row of path B on a pipe held open and silent, two jobs, and A's replay failing with
 *   `std::bad_alloc` after 100 ms: the reader waits on B while the batch stops. The pipe is closed after 5 s if the
 *   evaluation has not returned by then, so a reader that ignores the stop fails the test instead of hanging it.
 * @return What it gave.
 */
Silent silent_producer() {
  const FreshObserved fresh;
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const auto text = PATHS + rows_of("AAA", {100, 101, 103}, "a") + rows_of("AAA", {50}, "b");
  REQUIRE(::write(fds[1], text.data(), text.size()) == static_cast<ssize_t>(text.size()));
  std::atomic<bool> finished{false};
  Silent out;
  std::thread guard([&]() {
    for (int step = 0; step < 500 && !finished; ++step) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished) {
      out.closed_by_test_ = true;
      ::close(fds[1]);
    }
  });
  out.ran_ = drive(
    {"bars.source=stdin", "bars.symbol_column=symbol", "bars.path_column=path", "instrument.x=AAA",
      "evaluate.method=grid", "evaluate.jobs=2", "param.hold=100", "param.fail=1"},
    fds[0]
  );
  finished = true;
  guard.join();
  if (!out.closed_by_test_) {
    ::close(fds[1]);
  }
  ::close(fds[0]);
  return out;
}

/**
 * @brief One short path, then a path of 400,000 rows in a regular file (always ready), two jobs, and the first path's
 *   replay failing with `std::bad_alloc`.
 * @param stopped_at Set to how far the reader got into the file.
 * @param size Set to the file's size.
 * @return What it gave.
 */
Ran ready_file(off_t& stopped_at, off_t& size) {
  Scratch dir;
  std::vector<int> prices(400000, 100);
  const auto path = dir.write("in.csv", PATHS + rows_of("AAA", {100, 101}, "a") + rows_of("AAA", prices, "b"));
  const int fd = ::open(path.c_str(), O_RDONLY);
  const auto ran = drive(
    {"bars.source=stdin", "bars.symbol_column=symbol", "bars.path_column=path", "instrument.x=AAA",
      "evaluate.method=grid", "evaluate.jobs=2", "param.fail=1"},
    fd
  );
  stopped_at = ::lseek(fd, 0, SEEK_CUR);
  size = ::lseek(fd, 0, SEEK_END);
  ::close(fd);
  return ran;
}

} // namespace

TEST_CASE(
  "evaluate: a fatal throw stops a stdin reader waiting on a silent pipe or reading a ready file", "[threads]"
) {
  const auto silent = silent_producer();
  CHECK(!silent.closed_by_test_);
  CHECK(silent.ran_.exit_ == 1);
  CHECK(
    lines_of(silent.ran_.out_).back() == "{\"stopped\":{\"code\":\"replay.failed\",\"message\":\"std::bad_alloc\"}}"
  );
  off_t stopped_at = 0;
  off_t size = 0;
  const auto ready = ready_file(stopped_at, size);
  CHECK(ready.exit_ == 1);
  CHECK(ready.out_.find("\"stopped\"") != std::string::npos);
  CHECK(stopped_at < size);
}

TEST_CASE("evaluate: a stdin reader stops within a second of a fatal throw", "[.][local]") {
  CHECK(silent_producer().ran_.seconds_ < 1.0);
  off_t stopped_at = 0;
  off_t size = 0;
  CHECK(ready_file(stopped_at, size).seconds_ < 1.0);
}

namespace {

/**
 * @brief The peak resident memory of an evaluation of one-row stdin paths, run in a child with its output discarded.
 * @param paths The paths.
 * @param jobs The jobs.
 * @return The child's peak, in the system's unit.
 */
long peak_of(std::size_t paths, int jobs) {
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    int fds[2];
    const int null = ::open("/dev/null", O_WRONLY);
    if (::pipe(fds) != 0 || null < 0 || ::dup2(null, 1) < 0 || ::dup2(null, 2) < 0) {
      ::_exit(2);
    }
    std::thread writer([paths, fd = fds[1]]() {
      std::string chunk = PATHS;
      for (std::size_t p = 0; p < paths; ++p) {
        chunk += rows_of("AAA", {100}, "p" + std::to_string(p));
        if (chunk.size() > 60000 || p + 1 == paths) {
          if (::write(fd, chunk.data(), chunk.size()) != static_cast<ssize_t>(chunk.size())) {
            break;
          }
          chunk.clear();
        }
      }
      ::close(fd);
    });
    std::vector<std::string> args{
      "bars.source=stdin", "bars.symbol_column=symbol", "bars.path_column=path", "instrument.x=AAA",
      "evaluate.method=grid", "evaluate.jobs=" + std::to_string(jobs)
    };
    std::vector<char*> argv{const_cast<char*>("driver")};
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    const auto s = replay::Settings::from_args(static_cast<int>(argv.size()), argv.data());
    auto fills = [](atl::backtest::NextBarFillModel model) {
      return model;
    };
    const int code = replay::evaluate_main<Ticker>(s, fills, Measures{}, fds[0], nullptr);
    writer.join();
    std::cout.flush();
    ::_exit(code);
  }
  int status = 0;
  rusage usage{};
  REQUIRE(::wait4(pid, &status, 0, &usage) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  return usage.ru_maxrss;
}

} // namespace

TEST_CASE("evaluate: 100,000 one-row stdin paths peak within 10 % of 1,000", "[.][local]") {
  for (const int jobs : {1, 4}) {
    INFO(jobs);
    const auto few = peak_of(1000, jobs);
    const auto many = peak_of(100000, jobs);
    CAPTURE(few, many);
    CHECK(static_cast<double>(many) <= 1.1 * static_cast<double>(few));
  }
}

#endif
namespace {

namespace json = boost::json;

/**
 * @brief A program without state: on every bar it holds `k` shares of the bar's instrument; it states `w` bars of
 *   warm-up; `k` below 0 is a refused bound.
 */
struct Holder {
  static constexpr const char* atls_mode = "named";
  static constexpr bool atls_session = false;
  static std::vector<std::string> atls_instruments() {
    return {"x", "y"};
  }

  explicit Holder(std::vector<atl::InstrumentId> ids) : ids_(std::move(ids)) {}

  std::size_t warmup_bars() const {
    return static_cast<std::size_t>(w_);
  }

  void set(std::string_view name, double value) {
    if (name == "k") {
      k_ = value;
    } else if (name == "w") {
      w_ = value;
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
    atl::strategy::TargetPosition target;
    target.instrument_ = event.instrument_;
    target.target_quantity_ = atl::Decimal64(static_cast<std::int64_t>(k_), 0);
    target.tag_ = "hold";
    ctx.submit(target);
  }

  std::vector<atl::InstrumentId> ids_;
  runtest::Refusals refusals_;
  double k_{1};
  double w_{1};
};

/**
 * @brief Long CSV rows of two symbols at midnight UTC, one bar a day from a date, every day or weekdays only; each
 *   symbol's four prices one value, AAA rising and falling by its own rule, BBB by another.
 * @param from The first date, yyyymmdd.
 * @param count The bars of each symbol.
 * @param weekdays Whether Saturdays and Sundays have none.
 * @return The file's text, with its header.
 */
std::string days_of(int from, std::size_t count, bool weekdays = false) {
  std::string text = HEADER + "\n";
  long long day = replay::day_seconds(from) / 86400;
  for (std::size_t made = 0; made < count; ++day) {
    // 1970-01-01 was a Thursday: (day + 4) % 7 is 0 on Sundays, 6 on Saturdays.
    if (weekdays && ((day + 4) % 7 == 0 || (day + 4) % 7 == 6)) {
      continue;
    }
    for (
      const auto& [symbol, price] :
      {std::pair<std::string, std::size_t>{"AAA", 100 + made % 7 + made / 3}, {"BBB", 200 + (made * 5) % 11}}
    ) {
      const auto p = std::to_string(price);
      text += std::to_string(day * 86400) + "," + symbol + "," + p + "," + p + "," + p + "," + p + ",1000\n";
    }
    ++made;
  }
  return text;
}

/**
 * @brief The settings of an evaluation of `Holder` on a CSV of AAA and BBB.
 * @param file The CSV.
 * @param method The method, or empty for a single backtest.
 * @param more Further arguments.
 * @return The arguments.
 */
std::vector<std::string> on_pair(const std::string& file, const std::string& method, std::vector<std::string> more) {
  std::vector<std::string> args{
    "bars.source=csv", "bars.file=" + file, "bars.symbol_column=symbol", "instrument.x=AAA", "instrument.y=BBB",
    "evaluate.method=" + method
  };
  if (method.empty()) {
    args.pop_back();
  }
  args.insert(args.end(), more.begin(), more.end());
  return args;
}

/**
 * @brief A line of a driver's output as JSON.
 * @param ran What the driver gave.
 * @param k The line, from 0; the last when past the end.
 * @return The object.
 */
json::object line_of(const Ran& ran, std::size_t k) {
  const auto lines = lines_of(ran.out_);
  REQUIRE(!lines.empty());
  return json::parse(lines[std::min(k, lines.size() - 1)]).as_object();
}

/**
 * @brief The summary of a driver's output.
 * @param ran What it gave.
 * @return The summary object.
 */
json::object summary_of(const Ran& ran) {
  INFO(ran.out_ << ran.err_);
  REQUIRE(ran.exit_ == 0);
  return line_of(ran, std::size_t(-1)).at("summary").as_object();
}

/**
 * @brief Each line's window, as `from` and `to` dates and its periods.
 * @param ran What the driver gave.
 * @return One text per replay line, `FROM..TO/PERIODS`, with `<WARM` when it has a warm-up.
 */
std::vector<std::string> windows_of(const Ran& ran) {
  std::vector<std::string> out;
  for (const auto& line : lines_of(ran.out_)) {
    const auto value = json::parse(line).as_object();
    if (const auto* w = value.if_contains("window"); w != nullptr && w->is_object()) {
      const auto& o = w->as_object();
      const auto date = [&o](const char* key) {
        return o.at(key).is_null() ? std::string() : std::string(o.at(key).as_string()).substr(0, 10);
      };
      out.push_back(
        (o.at("warm_from").is_null() ? "" : date("warm_from") + "<") + date("from") + ".." + date("to") + "/" +
        std::to_string(o.at("periods").as_int64())
      );
    }
  }
  return out;
}

/**
 * @brief A single backtest's report text on one line, as a replay line carries it.
 * @param ran The backtest.
 * @return The text.
 */
std::string flat(const Ran& ran) {
  return replay::one_line(ran.out_);
}

/**
 * @brief The report a line carries, as text.
 * @param line The line.
 * @return The report's text, as the line has it.
 */
std::string report_text(const std::string& line) {
  return line.substr(line.find("\"report\":") + 9, line.size() - line.find("\"report\":") - 10);
}

} // namespace

TEST_CASE("loops: a span adds months first clamped to the month's last day and then days; a length's grammar") {
  CHECK(replay::plus(20240131, 1, 0) == 20240229);
  CHECK(replay::plus(20240131, 2, 0) == 20240331);
  CHECK(replay::plus(20240131, 13, 0) == 20250228);
  CHECK(replay::plus(20230131, 1, 0) == 20230228);
  CHECK(replay::plus(20240229, 12, 0) == 20250228);
  CHECK(replay::plus(20240131, 1, 7) == 20240307);
  CHECK(replay::plus(20241231, 0, 1) == 20250101);
  CHECK_THROWS_AS(replay::plus(20240131, 12 * 99999, 0), replay::Refused);
  CHECK(replay::length_of("k", "252", 2).periods_ == std::optional<std::size_t>(252));
  CHECK(replay::length_of("k", "0", 0).periods_ == std::optional<std::size_t>(0));
  const auto year = replay::length_of("k", "3y", 2);
  CHECK((!year.periods_ && year.months_ == 36 && year.days_ == 0));
  CHECK(replay::length_of("k", "2w", 2).days_ == 14);
  CHECK(replay::length_of("k", "6mo", 2).months_ == 6);
  CHECK(replay::length_of("k", "99999d", 2).days_ == 99999);
  for (const auto* bad : {"0y", "1.5y", "3Y", "100000d", "-1", "1", "01", "y", "", "1e3", "1000000001"}) {
    INFO(bad);
    CHECK_THROWS_AS(replay::length_of("k", bad, 2), std::invalid_argument);
  }
}

TEST_CASE("loops: windows in periods are the library's folds rolling and expanding with the tail uncovered") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto rolling =
    drive<Holder>(on_pair(file, "windows", {"windows.length=10", "windows.step=5", "windows.warmup=2"}));
  const auto summary = summary_of(rolling);
  CHECK(
    windows_of(rolling) ==
    std::vector<std::string>{
      "2024-01-01<2024-01-03..2024-01-16/10", "2024-01-08<2024-01-10..2024-01-23/10",
      "2024-01-15<2024-01-17..2024-01-30/10", "2024-01-22<2024-01-24..2024-02-06/10"
    }
  );
  CHECK(summary.at("windows").as_int64() == 4);
  CHECK(summary.at("uncovered_from") == json::value("2024-02-07"));
  CHECK(summary.at("length").as_int64() == 10);
  CHECK(summary.at("warmup").as_int64() == 2);
  // Without windows.summary the lines are the output: no summary of them.
  CHECK(!summary.contains("summary"));
  CHECK(summary.at("halted").as_int64() == 0);
  const auto expanding = drive<Holder>(
    on_pair(file, "windows", {"windows.scheme=expanding", "windows.length=10", "windows.step=5", "windows.warmup=0"})
  );
  CHECK(
    windows_of(expanding) ==
    std::vector<std::string>{
      "2024-01-01..2024-01-12/10", "2024-01-01..2024-01-19/15", "2024-01-01..2024-01-26/20",
      "2024-01-01..2024-02-02/25", "2024-01-01..2024-02-09/30"
    }
  );
  CHECK(summary_of(expanding).at("uncovered_from").is_null());
  // The default warm-up on daily bars is the program's warm-up, with its parameters.
  CHECK(summary_of(drive<Holder>(on_pair(file, "windows", {"windows.length=10", "param.w=3"}))).at("warmup") == 3);
  // A warm-up shorter than the program's is refused before any line; so is a window that does not fit.
  const auto short_warm =
    drive<Holder>(on_pair(file, "windows", {"windows.length=10", "windows.warmup=2", "param.w=3"}));
  CHECK(short_warm.exit_ == 1);
  CHECK(lines_of(short_warm.out_).size() == 1);
  CHECK(
    short_warm.out_ ==
    "{\"stopped\":{\"code\":\"evaluate.refused\",\"message\":\"window 0's warm-up holds 2 bars, and the program needs "
    "3\",\"next\":\"give a longer windows.warmup\"}}\n"
  );
  const auto none = drive<Holder>(on_pair(file, "windows", {"windows.length=29", "windows.warmup=2"}));
  CHECK(
    none.out_.find("\"code\":\"evaluate.refused\",\"message\":\"no window fits the history's 30 periods\"") !=
    std::string::npos
  );
}

/** @brief A measure: the points of the replay's curve. */
double points(const atl::backtest::MeasureInput& in) {
  return static_cast<double>(in.levels_.size());
}

TEST_CASE("loops: windows.summary gives the statistics named of each figure and measure, in order; others refused") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto run = [&](const std::string& names) {
    return drive<Holder>(
      on_pair(
        file, "windows",
        {"windows.scheme=expanding", "windows.length=10", "windows.step=5", "windows.warmup=0",
          "windows.summary=" + names}
      ),
      -1, {}, {{"points", points}}
    );
  };
  const auto ran = run("max,q25,mean,q05,median");
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 6);
  // The expanding windows trade 10, 15, 20, 25 and 30 periods, a curve point each.
  for (std::size_t k = 0; k < 5; ++k) {
    CHECK(line_of(ran, k).at("report").at("measures").at("points").to_number<double>() == 10.0 + 5.0 * k);
  }
  // Of 10, 15, 20, 25, 30: h = 4p, so q25 = 15 and q05 = 10 + 0.2 * 5.
  CHECK(
    lines.back().find("\"halted\":0,\"summary\":{\"total_return\":{\"defined\":5,\"null\":0,\"max\":") !=
    std::string::npos
  );
  CHECK(
    lines.back().find(
      ",\"measures\":{\"points\":{\"defined\":5,\"null\":0,\"max\":30,\"q25\":15,\"mean\":20,\"q05\":11,\"median\":20}}}"
    ) != std::string::npos
  );
  // An unknown or malformed name, or one named twice, is refused before any replay.
  for (
    const auto& [names, message] :
    {std::pair<std::string, std::string>{"mean,q5", "windows.summary names no statistic: 'q5'"},
      {"q101", "windows.summary names no statistic: 'q101'"},
      {"average", "windows.summary names no statistic: 'average'"},
      {"mean,median,mean", "windows.summary names 'mean' twice"}}
  ) {
    INFO(names);
    const auto refused = run(names);
    CHECK(refused.exit_ == 1);
    CHECK(
      refused.out_ ==
      "{\"stopped\":{\"code\":\"evaluate.refused\",\"message\":\"" + message +
        "\",\"next\":\"name each of mean, min, max, median, q00 to q100 at most once\"}}\n"
    );
  }
}

TEST_CASE("loops: span windows end on the anchored boundary; whole windows only; no warm-up before the anchor") {
  Scratch dir;
  const auto every = dir.write("every.csv", days_of(20240131, 130));
  const auto monthly = [&](std::vector<std::string> more) {
    more.insert(more.end(), {"windows.length=1mo", "windows.step=1mo"});
    return drive<Holder>(on_pair(every, "windows", more));
  };
  // From 2024-01-31 by months: Jan-31, Feb-29, Mar-31, Apr-30, May-31; each window ends where the next starts.
  CHECK(
    windows_of(monthly({"windows.warmup=0", "replay.from=20240131"})) ==
    std::vector<std::string>{
      "2024-01-31..2024-02-28/29", "2024-02-29..2024-03-30/31", "2024-03-31..2024-04-29/30", "2024-04-30..2024-05-30/31"
    }
  );
  // Window 1 ends on Mar-31, after replay.to (Mar-28) plus a day.
  CHECK(
    windows_of(monthly({"windows.warmup=0", "replay.to=20240328"})) ==
    std::vector<std::string>{"2024-01-31..2024-02-28/29"}
  );
  // A span warm-up from Mar-31: warm over [Mar-31, Apr-30), trade [Apr-30, May-31).
  const auto warm = monthly({"windows.warmup=1mo", "replay.from=20240331"});
  CHECK(windows_of(warm).front() == "2024-03-31<2024-04-30..2024-05-30/31");
  CHECK(summary_of(warm).at("warmup") == json::value("1mo"));
  // A warm-up in periods with span lengths: the anchor is the date of period 3.
  CHECK(windows_of(monthly({"windows.warmup=3"})).front() == "2024-01-31<2024-02-03..2024-03-02/29");
  // Weekdays to Friday Mar-29: the window ending Apr-1 is kept only when replay.to says the history reaches Mar-31.
  const auto weekdays = dir.write("weekdays.csv", days_of(20240101, 65, true));
  const auto by_month = [&](std::vector<std::string> more) {
    more.insert(more.end(), {"windows.length=1mo", "windows.warmup=0"});
    return summary_of(drive<Holder>(on_pair(weekdays, "windows", more))).at("windows").as_int64();
  };
  CHECK(by_month({}) == 2);
  CHECK(by_month({"replay.to=20240331"}) == 3);
  // A day's window holds one period at most: refused, naming it.
  const auto daily = drive<Holder>(on_pair(weekdays, "windows", {"windows.length=1d", "windows.warmup=0"}));
  CHECK(daily.out_.find("\"message\":\"window 0 holds fewer than 2 periods\"") != std::string::npos);
}

TEST_CASE("loops: a window replays as a backtest of its span and a warm stateless window as one of the window") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto windows = drive<Holder>(on_pair(file, "windows", {"windows.length=5", "windows.warmup=0", "param.k=3"}));
  const auto lines = lines_of(windows.out_);
  REQUIRE(lines.size() == 7);
  const auto backtest = drive<Holder>(on_pair(file, "", {"param.k=3", "replay.from=20240108", "replay.to=20240112"}));
  REQUIRE(backtest.exit_ == 0);
  CHECK(report_text(lines[1]) == flat(backtest));
  // With two days of history before it, a program without state reports what a backtest of the window alone does.
  const auto warm = drive<Holder>(on_pair(file, "windows", {"windows.length=5", "windows.warmup=2", "param.k=3"}));
  const auto warm_lines = lines_of(warm.out_);
  CHECK(
    warm_lines[0].find("\"warm_from\":\"2024-01-01T00:00:00Z\",\"from\":\"2024-01-03T00:00:00Z\"") != std::string::npos
  );
  CHECK(
    report_text(warm_lines[0]) ==
    flat(drive<Holder>(on_pair(file, "", {"param.k=3", "replay.from=20240103", "replay.to=20240109"})))
  );
}

TEST_CASE("loops: walk-forward chooses on each training window then tests the choice and chains the tests") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto ran = drive<Holder>(on_pair(
    file, "walk-forward",
    {"walk_forward.train=10", "walk_forward.test=5", "grid.k=1,-1,3", "grid.w=1,2", "evaluate.objective=total_return"}
  ));
  const auto summary = summary_of(ran);
  const auto lines = lines_of(ran.out_);
  // Four folds of six sets (k, then w fastest), then four tests.
  REQUIRE(lines.size() == 4 * 6 + 4 + 1);
  CHECK(lines[2].find("\"failure\":{\"kind\":\"params\"") != std::string::npos);
  CHECK(
    lines[6].find("\"window\":{\"fold\":1,\"role\":\"train\",\"warm_from\":null,\"from\":\"2024-01-08") !=
    std::string::npos
  );
  CHECK(lines[24].rfind("{\"index\":24,\"params\":{\"k\":", 0) == 0);
  CHECK(lines[24].find("\"window\":{\"fold\":0,\"role\":\"test\",\"warm_from\":\"2024-01-01") != std::string::npos);
  CHECK(summary.at("param_sets") == 6);
  CHECK(summary.at("invalid") == 2);
  CHECK(summary.at("tried") == 4);
  CHECK(summary.at("train_replays") == 16);
  CHECK(summary.at("replays") == 28);
  CHECK(summary.at("best").is_null());
  const auto& folds = summary.at("folds").as_array();
  REQUIRE(folds.size() == 4);
  double product = 1;
  for (std::size_t k = 0; k < 4; ++k) {
    const auto& f = folds[k].as_object();
    INFO(k << " " << json::serialize(f));
    CHECK(f.at("eligible") == 4);
    CHECK(f.at("test_index") == static_cast<std::int64_t>(24 + k));
    CHECK(f.at("test_periods") == 5);
    product *= 1 + f.at("test_total_return").to_number<double>();
    // w changes nothing traded: each tie goes to the lower set, w = 1 (sets 0 and 4).
    CHECK((f.at("chosen") == 0 || f.at("chosen") == 4));
  }
  CHECK(summary.at("chained_null_reason").is_null());
  const auto chained = summary.at("chained").at("total_return").to_number<double>();
  CHECK(std::abs(chained - (product - 1)) < 1e-8);
  // One fold: the chain is that fold's test report.
  const auto one = drive<Holder>(on_pair(
    file, "walk-forward",
    {"walk_forward.train=20", "walk_forward.test=10", "grid.k=1,2", "evaluate.objective=total_return"}
  ));
  const auto one_lines = lines_of(one.out_);
  REQUIRE(one_lines.size() == 4);
  CHECK(json::parse(report_text(one_lines[2])) == summary_of(one).at("chained"));
  // No set eligible in a fold: no test there, and no chain.
  const auto none = drive<Holder>(on_pair(
    file, "walk-forward",
    {"walk_forward.train=10", "walk_forward.test=5", "grid.k=-1,-2", "evaluate.objective=total_return"}
  ));
  CHECK(summary_of(none).at("chained_null_reason") == json::value("unchosen"));
  CHECK(summary_of(none).at("folds").at(0).at("chosen").is_null());
  // A training window shorter than the warm-up and two bars is refused for the set that needs it.
  const auto refused = drive<Holder>(on_pair(
    file, "walk-forward",
    {"walk_forward.train=10", "walk_forward.test=5", "grid.w=1,9", "evaluate.objective=total_return"}
  ));
  CHECK(
    refused.out_.find(
      "\"message\":\"set 1 {\\\"w\\\":9}, fold 0: the training window holds 10 bars, and the program needs 11\""
    ) != std::string::npos
  );
}

namespace {

/**
 * @brief A program that changes after `after` bars: it holds `k` shares of each bar's instrument, then `q`, or throws
 *   when `fail` is 1; so a training window shorter than `after` bars replays as `Holder` and a test fed more fails.
 */
struct Late {
  static constexpr const char* atls_mode = "named";
  static constexpr bool atls_session = false;
  static std::vector<std::string> atls_instruments() {
    return {"x", "y"};
  }

  explicit Late(std::vector<atl::InstrumentId> ids) : ids_(std::move(ids)) {}

  std::size_t warmup_bars() const {
    return 1;
  }

  void set(std::string_view name, double value) {
    if (name == "k") {
      k_ = value;
    } else if (name == "q") {
      q_ = value;
    } else if (name == "after") {
      after_ = value;
    } else if (name == "fail") {
      fail_ = value;
    } else {
      throw std::invalid_argument("the program has no parameter '" + std::string(name) + "'");
    }
  }

  void set_order_pricing(atl::OrderType, const atl::Decimal64&) {}

  const runtest::Refusals& pricing_refusals() const {
    return refusals_;
  }

  template <class Ctx>
  void on_bar(Ctx& ctx, const atl::strategy::BarEvent& event) {
    ++seen_;
    if (static_cast<double>(seen_) > after_ && fail_ == 1) {
      throw std::runtime_error("late failure");
    }
    atl::strategy::TargetPosition target;
    target.instrument_ = event.instrument_;
    target.target_quantity_ =
      atl::Decimal64(static_cast<std::int64_t>(static_cast<double>(seen_) > after_ ? q_ : k_), 0);
    target.tag_ = "late";
    ctx.submit(target);
  }

  std::vector<atl::InstrumentId> ids_;
  runtest::Refusals refusals_;
  std::size_t seen_{0};
  double k_{1};
  double q_{1};
  double after_{1e9};
  double fail_{0};
};

/** @brief A measure that throws on a curve of more than 12 points. */
double short_only(const atl::backtest::MeasureInput& in) {
  if (in.levels_.size() > 12) {
    throw std::runtime_error("a long curve");
  }
  return 1;
}

/**
 * @brief Each fold of a walk-forward's summary as dates and counts.
 * @param summary The summary.
 * @return One text per fold, `TRAIN_FROM..TRAIN_TO|TEST_FROM..TEST_TO/TRAIN_PERIODS+TEST_PERIODS`.
 */
std::vector<std::string> folds_of(const json::object& summary) {
  std::vector<std::string> out;
  for (const auto& fold : summary.at("folds").as_array()) {
    const auto& f = fold.as_object();
    const auto date = [&f](const char* key) {
      return std::string(f.at(key).as_string()).substr(0, 10);
    };
    out.push_back(
      date("train_from") + ".." + date("train_to") + "|" + date("test_from") + ".." + date("test_to") + "/" +
      std::to_string(f.at("train_periods").as_int64()) + "+" + std::to_string(f.at("test_periods").as_int64())
    );
  }
  return out;
}

} // namespace

TEST_CASE("loops: walk-forward folds are the library's in periods and anchored at replay.from in spans") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto expanding = drive<Holder>(on_pair(
    file, "walk-forward",
    {"walk_forward.scheme=expanding", "walk_forward.train=10", "walk_forward.test=5", "grid.k=1,2",
      "evaluate.objective=total_return"}
  ));
  // Every training window starts at the first period; the tests tile the rest.
  CHECK(
    folds_of(summary_of(expanding)) ==
    std::vector<std::string>{
      "2024-01-01..2024-01-12|2024-01-15..2024-01-19/10+5", "2024-01-01..2024-01-19|2024-01-22..2024-01-26/15+5",
      "2024-01-01..2024-01-26|2024-01-29..2024-02-02/20+5", "2024-01-01..2024-02-02|2024-02-05..2024-02-09/25+5"
    }
  );
  CHECK(summary_of(expanding).at("scheme") == json::value("expanding"));
  CHECK(summary_of(expanding).at("uncovered_from").is_null());
  // A test line's history is its training window.
  CHECK(windows_of(expanding)[8] == "2024-01-01<2024-01-15..2024-01-19/5");
  CHECK(windows_of(expanding)[11] == "2024-01-01<2024-02-05..2024-02-09/5");
  // Weekdays from Tuesday 2024-01-02 to Friday 2024-06-28; folds of two months' training and a month's test.
  const auto weekdays = dir.write("weekdays.csv", days_of(20240102, 129, true));
  const auto by_months = [&](std::vector<std::string> more) {
    more.insert(
      more.end(), {"walk_forward.train=2mo", "walk_forward.test=1mo", "grid.k=1,2", "evaluate.objective=total_return"}
    );
    return summary_of(drive<Holder>(on_pair(weekdays, "walk-forward", more)));
  };
  // Anchored at replay.from, 2024-01-01: the tests are March to June, the last ending on 2024-07-01, replay.to's next day.
  const auto anchored = by_months({"replay.from=20240101", "replay.to=20240630"});
  CHECK(
    folds_of(anchored) ==
    std::vector<std::string>{
      "2024-01-02..2024-02-29|2024-03-01..2024-03-29/43+21", "2024-02-01..2024-03-29|2024-04-01..2024-04-30/42+22",
      "2024-03-01..2024-04-30|2024-05-01..2024-05-31/43+23", "2024-04-01..2024-05-31|2024-06-03..2024-06-28/45+20"
    }
  );
  CHECK(anchored.at("train") == json::value("2mo"));
  CHECK(anchored.at("test") == json::value("1mo"));
  CHECK(anchored.at("uncovered_from").is_null());
  // Without replay.to the history ends on Friday 2024-06-28: the June test does not fit, and June is uncovered.
  const auto unbounded = by_months({"replay.from=20240101"});
  CHECK(unbounded.at("folds").as_array().size() == 3);
  CHECK(unbounded.at("uncovered_from") == json::value("2024-06-03"));
  // Anchored at the first bar (2024-01-02) instead, the last test would end on 2024-07-02: dropped.
  CHECK(by_months({"replay.to=20240630"}).at("folds").as_array().size() == 3);
}

TEST_CASE(
  "loops: walk-forward chooses either way and by a measure; skips failed sets; a failed or halted test nulls the chain"
) {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto run = [&](std::vector<std::string> more, const Measures& measures = {}) {
    more.insert(more.end(), {"walk_forward.train=10", "walk_forward.test=5"});
    return drive<Holder>(on_pair(file, "walk-forward", more), -1, {}, measures);
  };
  const auto most = summary_of(run({"grid.k=1,3", "evaluate.objective=total_return"}));
  const auto least = summary_of(run({"grid.k=1,3", "evaluate.objective=total_return", "evaluate.direction=min"}));
  CHECK(least.at("direction") == json::value("min"));
  std::size_t differ = 0;
  for (std::size_t k = 0; k < 4; ++k) {
    const auto& high = most.at("folds").at(k).as_object();
    const auto& low = least.at("folds").at(k).as_object();
    INFO(k << " " << json::serialize(high) << " " << json::serialize(low));
    const auto top = high.at("objective_value").to_number<double>();
    const auto bottom = low.at("objective_value").to_number<double>();
    CHECK(bottom <= top);
    CHECK((top == bottom) == (high.at("chosen") == low.at("chosen")));
    differ += high.at("chosen") != low.at("chosen");
  }
  CHECK(differ > 0);
  // By a measure: k = 0 executes nothing, so every fold chooses k = 2; the chain's measure counts the tests'.
  const auto measured = run({"grid.k=0,2", "evaluate.objective=measures.executions"}, {{"executions", executions}});
  const auto summary = summary_of(measured);
  std::int64_t executed = 0;
  for (const auto& fold : summary.at("folds").as_array()) {
    CHECK(fold.at("chosen") == 1);
    const auto line = json::parse(lines_of(measured.out_)[fold.at("test_index").to_number<std::size_t>()]);
    executed += line.at("report").at("executions").as_int64();
  }
  CHECK(executed > 0);
  CHECK(summary.at("chained").at("executions") == executed);
  CHECK(summary.at("chained").at("measures").at("executions").to_number<double>() == static_cast<double>(executed));
  // A measure that throws on the chain alone: the chain is unavailable, with the message in warnings.
  const auto unavailable = summary_of(run({"grid.k=1,2", "evaluate.objective=total_return"}, {{"short", short_only}}));
  CHECK(unavailable.at("chained").is_null());
  CHECK(unavailable.at("chained_null_reason") == json::value("chain_unavailable"));
  CHECK(unavailable.at("warnings").at(0).at("code") == json::value("chain_unavailable"));
  CHECK(unavailable.at("warnings").at(0).at("message") == json::value("the measure short threw: a long curve"));
  CHECK(run({"grid.k=1,2", "evaluate.objective=total_return"}).out_.find("\"warnings\":[]") != std::string::npos);
  // Twenty bars train (two symbols), fifteen periods feed a test: failing after 24 bars fails only the tests.
  const auto late = [&](std::vector<std::string> more) {
    more.insert(more.end(), {"walk_forward.train=10", "walk_forward.test=5", "evaluate.objective=total_return"});
    return drive<Late>(on_pair(file, "walk-forward", more));
  };
  const auto failed = summary_of(late({"grid.k=1,2", "param.after=24", "param.fail=1"}));
  CHECK(failed.at("chained_null_reason") == json::value("failed"));
  CHECK(failed.at("failed") == 4);
  CHECK(!failed.at("folds").at(0).at("chosen").is_null());
  CHECK(failed.at("folds").at(0).at("test_total_return").is_null());
  // An order too large for the books after 24 bars halts every test: the chain is null.
  const auto halted = summary_of(late({"grid.k=1,2", "param.after=24", "param.q=9000000000000000"}));
  CHECK(halted.at("chained_null_reason") == json::value("halted"));
  CHECK(halted.at("folds").at(0).at("test_halted") == true);
  // A set that fails in training is skipped: the fold chooses among the others.
  const auto skipped = summary_of(late({"grid.after=1,1000", "param.fail=1"}));
  for (const auto& fold : skipped.at("folds").as_array()) {
    CHECK(fold.at("eligible") == 1);
    CHECK(fold.at("chosen") == 1);
  }
  CHECK(skipped.at("chained_null_reason").is_null());
}

TEST_CASE("loops: walk-forward refuses what fits no fold and lengths past the calendar") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 30, true));
  const auto run = [&](const std::string& train, const std::string& test) {
    return drive<Holder>(on_pair(
      file, "walk-forward",
      {"walk_forward.train=" + train, "walk_forward.test=" + test, "grid.k=1,2", "evaluate.objective=total_return"}
    ));
  };
  const auto refusal = [](const Ran& ran) {
    INFO(ran.out_ << ran.err_);
    CHECK(ran.exit_ == 1);
    CHECK(lines_of(ran.out_).size() == 1);
    return line_of(ran, 0).at("stopped").as_object();
  };
  const auto none = refusal(run("25", "10"));
  CHECK(none.at("code") == json::value("evaluate.refused"));
  CHECK(none.at("message") == json::value("no fold fits the history's 30 periods"));
  CHECK(none.at("next") == json::value("give a shorter walk_forward.train"));
  CHECK(refusal(run("1000000000", "1000000000")).at("code") == json::value("evaluate.refused"));
  CHECK(refusal(run("99999y", "1d")).at("message") == json::value("a window's boundary falls after the year 9999"));
  // The kinds are not mixed; the spec refuses these first, and the driver repeats it.
  CHECK(
    refusal(run("10", "1mo")).at("message") ==
    json::value("give walk_forward.train and walk_forward.test both as periods or both as spans")
  );
  CHECK(refusal(run("1", "5")).at("message") == json::value("walk_forward.train is 2 periods or more, not 1"));
  // Anchored on a Sunday before Monday's first bar, a day's training holds none: refused whatever the sets, even when
  // the bounds refuse every one (no set's warm-up then checks it).
  const auto weekdays = dir.write("weekdays.csv", days_of(20240108, 30, true));
  for (const auto* grid : {"grid.k=-1,-2", "grid.k=1,2"}) {
    INFO(grid);
    const auto empty = refusal(
      drive<Holder>(on_pair(
        weekdays, "walk-forward",
        {"replay.from=20240107", "walk_forward.train=1d", "walk_forward.test=1w", grid,
          "evaluate.objective=total_return"}
      ))
    );
    CHECK(empty.at("code") == json::value("evaluate.refused"));
    CHECK(empty.at("message") == json::value("fold 0: the training window holds 0 bars, fewer than 2"));
    CHECK(empty.at("next") == json::value("give a longer walk_forward.train"));
  }
  // Folds times sets plus the tests past the cap is refused before any replay.
  std::string many = "grid.k=1";
  for (int k = 2; k <= 1000; ++k) {
    many += "," + std::to_string(k);
  }
  const auto long_file = dir.write("long.csv", days_of(20240101, 210));
  const auto capped = refusal(
    drive<Holder>(on_pair(
      long_file, "walk-forward",
      {"walk_forward.train=2", "walk_forward.test=2", many, "evaluate.objective=total_return"}
    ))
  );
  CHECK(capped.at("message") == json::value("the evaluation makes 104104 replays, more than 100000"));
}

TEST_CASE("loops: bootstrap path 0 is the backtest; a path depends on its number alone; a block is given") {
  Scratch dir;
  std::string table = "date,AAA,BBB,CCC\n";
  for (int k = 0; k < 40; ++k) {
    table += std::to_string(20240101 + (k / 28) * 100 + k % 28 + (k / 28 == 0 ? 0 : 0)) + "," +
      std::to_string(5000 + 10 * ((k * k) % 17)) + "," + std::to_string(7000 + 10 * ((k * 7) % 13)) + ",9000\n";
  }
  const auto wide = dir.write("closes.csv", table);
  const auto boot = [&](std::vector<std::string> more, const std::string& y = "BBB") {
    std::vector<std::string> args{
      "bars.source=wide", "bars.file=" + wide, "replay.market=kr", "instrument.x=AAA", "instrument.y=" + y,
      "evaluate.method=bootstrap"
    };
    args.insert(args.end(), more.begin(), more.end());
    return drive<Holder>(args);
  };
  const auto ran = boot({"bootstrap.seed=7", "bootstrap.block=5", "bootstrap.paths=2", "param.k=10"});
  const auto summary = summary_of(ran);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 4);
  CHECK(
    lines[1].find("\"path\":{\"number\":1,\"seed\":7,\"id\":null,\"scheme\":\"circular\",\"block\":5}") !=
    std::string::npos
  );
  CHECK(summary.at("paths") == 3);
  CHECK(summary.at("intervals").as_object().empty());
  CHECK(!summary.contains("summary"));
  const auto backtest = drive<Holder>(
    {"bars.source=wide", "bars.file=" + wide, "replay.market=kr", "instrument.x=AAA", "instrument.y=BBB", "param.k=10"}
  );
  REQUIRE(backtest.exit_ == 0);
  CHECK(report_text(lines[0]) == flat(backtest));
  CHECK(summary.at("history").at("total_return") == line_of(ran, 0).at("report").at("total_return"));
  // Path 2 is the same with 2 paths and with 5; another seed draws another path.
  const auto five = boot({"bootstrap.seed=7", "bootstrap.block=5", "bootstrap.paths=5", "param.k=10"});
  CHECK(lines_of(five.out_)[2] == lines[2]);
  CHECK(
    lines_of(boot({"bootstrap.seed=8", "bootstrap.block=5", "bootstrap.paths=2", "param.k=10"}).out_)[2] != lines[2]
  );
  CHECK(summary.at("scheme") == json::value("circular"));
  // Without a seed one is drawn and echoed, below 2^53; without a block, the key is asked for, before any line.
  const auto drawn = summary_of(boot({"bootstrap.block=5", "bootstrap.paths=1"}));
  CHECK(drawn.at("seed").to_number<std::uint64_t>() < (std::uint64_t{1} << 53));
  const auto unblocked = boot({"bootstrap.paths=1"});
  CHECK(unblocked.exit_ == 1);
  CHECK(
    unblocked.out_ ==
    "{\"stopped\":{\"code\":\"evaluate.refused\",\"message\":\"bootstrap draws blocks of bootstrap.block periods: "
    "give one\",\"next\":\"give bootstrap.block = N\"}}\n"
  );
  // Below the program's warm-up the block is a warning, not a refusal.
  CHECK(
    summary_of(boot({"bootstrap.block=3", "bootstrap.paths=1", "param.w=4"})).at("warnings") ==
    json::parse("[{\"code\":\"block_below_lookback\",\"block\":3,\"lookback\":4}]")
  );
  CHECK(summary_of(boot({"bootstrap.block=4", "bootstrap.paths=1", "param.w=4"})).at("warnings").as_array().empty());
}

TEST_CASE("loops: bootstrap refusals come before any line with the key to give") {
  Scratch dir;
  const auto wide = [&](const std::string& name, const std::string& rows) {
    return dir.write(name, "date,AAA,BBB\n" + rows);
  };
  const auto boot = [&](const std::string& file, std::vector<std::string> more) {
    std::vector<std::string> args{
      "bars.source=wide", "bars.file=" + file, "instrument.x=AAA", "instrument.y=BBB", "evaluate.method=bootstrap"
    };
    args.insert(args.end(), more.begin(), more.end());
    const auto ran = drive<Holder>(args);
    CHECK(ran.exit_ == 1);
    CHECK(lines_of(ran.out_).size() == 1);
    return ran.out_;
  };
  std::string short_rows;
  for (int k = 1; k <= 8; ++k) {
    short_rows += "202401" + std::string(k < 10 ? "0" : "") + std::to_string(k) + "," +
      std::to_string(100 + k * k % 5) + "," + std::to_string(50 + k % 3) + "\n";
  }
  CHECK(
    boot(wide("short2.csv", short_rows), {"bootstrap.block=8"})
      .find("\"message\":\"bootstrap.block 8 is above the history's 7 returns\"") != std::string::npos
  );
  CHECK(
    boot(wide("ragged.csv", "20240102,100,50\n20240103,101,\n20240104,102,51\n"), {"bootstrap.block=1"})
      .find("\"message\":\"BBB has no close on 20240103\"") != std::string::npos
  );
}

TEST_CASE("loops: the summary's statistics are the ones named, in order; quantiles type 7 with nulls counted apart") {
  const std::vector<std::string> all{"mean", "min", "q05", "q25", "median", "q75", "q95", "max"};
  CHECK(
    replay::spread_json({}, 2, all) ==
    "{\"defined\":0,\"null\":2,\"mean\":null,\"min\":null,\"q05\":null,\"q25\":null,\"median\":null,\"q75\":null,\"q95\":null,\"max\":null}"
  );
  CHECK(
    replay::spread_json({3}, 0, all) ==
    "{\"defined\":1,\"null\":0,\"mean\":3,\"min\":3,\"q05\":3,\"q25\":3,\"median\":3,\"q75\":3,\"q95\":3,\"max\":3}"
  );
  // Two values: h = p, so q05 = 1 + 0.05 * 2.
  CHECK(
    replay::spread_json({3, 1}, 1, all) ==
    "{\"defined\":2,\"null\":1,\"mean\":2,\"min\":1,\"q05\":1.1,\"q25\":1.5,\"median\":2,\"q75\":2.5,\"q95\":2.9,\"max\":3}"
  );
  // Five values 1, 2, 4, 8, 16: h = 4p; q05 = 1 + 0.2 * 1, q25 = 2, q75 = 8, q95 = 8 + 0.8 * 8.
  CHECK(
    replay::spread_json({16, 1, 8, 2, 4}, 0, all) ==
    "{\"defined\":5,\"null\":0,\"mean\":6.2,\"min\":1,\"q05\":1.2,\"q25\":2,\"median\":4,\"q75\":8,\"q95\":14.4,\"max\":16}"
  );
  // In the order named; q00, q50 and q100 are min, median and max; q90: h = 3.6, 8 + 0.6 * 8.
  CHECK(
    replay::spread_json({16, 1, 8, 2, 4}, 0, {"q100", "q90", "median", "q50", "q00", "min"}) ==
    "{\"defined\":5,\"null\":0,\"q100\":16,\"q90\":12.8,\"median\":4,\"q50\":4,\"q00\":1,\"min\":1}"
  );
  CHECK(replay::spread_json({1}, 3, {}) == "{\"defined\":1,\"null\":3}");
}

TEST_CASE("loops: one job and four print the same bytes for every method", "[threads]") {
  Scratch dir;
  const auto file = dir.write("bars.csv", days_of(20240101, 40, true));
  std::string table = "date,AAA,BBB\n";
  for (int k = 0; k < 30; ++k) {
    table +=
      std::to_string(20240101 + k) + "," + std::to_string(100 + (k * k) % 7) + "," + std::to_string(50 + k % 4) + "\n";
  }
  const auto wide = dir.write("closes.csv", table);
  for (
    const auto& args : std::vector<std::vector<std::string>>{
      on_pair(file, "windows", {"windows.length=8", "windows.step=3"}),
      on_pair(
        file, "walk-forward",
        {"walk_forward.train=8", "walk_forward.test=4", "grid.k=1,2,3", "evaluate.objective=sharpe_annualized"}
      ),
      {"bars.source=wide", "bars.file=" + wide, "instrument.x=AAA", "instrument.y=BBB", "evaluate.method=bootstrap",
        "bootstrap.seed=3", "bootstrap.block=4", "bootstrap.paths=12", "bootstrap.scheme=stationary"}
    }
  ) {
    auto four = args;
    four.push_back("evaluate.jobs=4");
    const auto one = drive<Holder>(args);
    const auto many = drive<Holder>(four);
    INFO(args.back() << one.out_ << one.err_);
    REQUIRE(one.exit_ == 0);
    auto expected = one.out_;
    expected.replace(expected.find("\"jobs\":1,"), 9, "\"jobs\":4,");
    CHECK(many.out_ == expected);
  }
}

TEST_CASE("loops: bootstrap intervals for the metrics the user names; measures included") {
  Scratch dir;
  std::string table = "date,AAA,BBB\n";
  for (int k = 0; k < 28; ++k) {
    table += std::to_string(20240101 + k) + "," + std::to_string(5000 + 10 * ((k * k) % 17)) + "," +
      std::to_string(7000 + 10 * ((k * 7) % 13)) + "\n";
  }
  const auto wide = dir.write("closes.csv", table);
  const auto boot = [&](std::vector<std::string> more) {
    std::vector<std::string> args{
      "bars.source=wide", "bars.file=" + wide, "replay.market=kr", "instrument.x=AAA", "instrument.y=BBB",
      "evaluate.method=bootstrap", "bootstrap.seed=11", "bootstrap.block=3", "bootstrap.paths=20", "param.k=10"
    };
    args.insert(args.end(), more.begin(), more.end());
    return drive<Holder>(args, -1, {}, Measures{{"executions", executions}});
  };
  const auto wide95 = summary_of(boot({"bootstrap.intervals=total_return,measures.executions"})).at("intervals");
  const auto narrow = summary_of(boot({"bootstrap.intervals=total_return", "bootstrap.level=0.5"})).at("intervals");
  const auto& total = wide95.at("total_return").as_object();
  CHECK(total.at("level").to_number<double>() == 0.95);
  CHECK(total.at("defined").as_int64() + total.at("null").as_int64() == 20);
  CHECK(wide95.at("measures.executions").at("defined").as_int64() == 20);
  // A narrower level nests inside a wider one, around the same median.
  const auto& half = narrow.at("total_return").as_object();
  CHECK(half.at("median") == total.at("median"));
  CHECK(total.at("lower").to_number<double>() <= half.at("lower").to_number<double>());
  CHECK(half.at("upper").to_number<double>() <= total.at("upper").to_number<double>());
  CHECK(half.at("lower").to_number<double>() <= half.at("upper").to_number<double>());
  const auto unknown = boot({"bootstrap.intervals=total_return,calmar"});
  CHECK(unknown.exit_ == 1);
  CHECK(
    unknown.out_ ==
    "{\"stopped\":{\"code\":\"evaluate.refused\",\"message\":\"bootstrap.intervals names no figure of the report and "
    "no measure: 'calmar'\",\"next\":\"name report figures (total_return, sharpe_annualized, ...) or "
    "measures.NAME\"}}\n"
  );
}

TEST_CASE("loops: a driver called directly refuses bootstrap on bars with their own open high or low") {
  Scratch dir;
  std::string rows = HEADER + "\n";
  for (int day = 0; day < 6; ++day) {
    const auto ts = std::to_string(1704067200 + 86400 * day);
    rows += ts + ",AAA,100,103,99," + std::to_string(101 + day) + ",1000\n" + ts + ",BBB,50,52,49,51,1000\n";
  }
  const auto ran = drive<Holder>(
    on_pair(dir.write("ohlc.csv", rows), "bootstrap", {"bootstrap.block=2", "bootstrap.paths=1", "bootstrap.seed=7"})
  );
  CHECK(ran.exit_ == 1);
  REQUIRE(lines_of(ran.out_).size() == 1);
  CHECK(
    ran.out_.rfind(
      "{\"stopped\":{\"code\":\"evaluate.refused\",\"message\":\"a bootstrap path rebuilds daily closes", 0
    ) == 0
  );
}

TEST_CASE("loops: windows in periods meet the replay limit before any worker starts") {
  Scratch dir;
  // 100,002 days from 1700, inside every platform's clock: length 2, step 1, no warm-up make one window fewer.
  std::string rows = HEADER + "\n";
  const long long first = replay::day_seconds(17000101);
  for (long long day = 0; day < 100002; ++day) {
    rows += std::to_string(first + 86400 * day) + ",AAA,100,100,100,100,1000\n";
  }
  const auto file = dir.write("long.csv", rows);
  replay::RunnerHooks hooks;
  hooks.starting_ = [](std::size_t) {
    throw std::runtime_error("the workers start");
  };
  std::vector<std::string> args{
    "bars.source=csv", "bars.file=" + file, "bars.symbol_column=symbol", "instrument.x=AAA", "evaluate.method=windows",
    "windows.length=2", "windows.step=1", "windows.warmup=0", "evaluate.jobs=2"
  };
  const auto over = drive(args, -1, hooks);
  INFO(over.out_ << over.err_);
  CHECK(over.out_.find("\"message\":\"the evaluation makes 100001 replays, more than 100000\"") != std::string::npos);
  args.push_back("replay.to=" + std::to_string(replay::plus(17000101, 0, 100000)));
  const auto at = drive(args, -1, hooks);
  CHECK(at.out_ == "{\"stopped\":{\"code\":\"replay.failed\",\"message\":\"the workers start\"}}\n");
}
