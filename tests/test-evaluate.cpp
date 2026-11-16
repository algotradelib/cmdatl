// Many replays in one driver (include/cmdatl/replay/evaluate.hpp): the index order, the lines, the choice of the best,
// and the runner on threads (order, the waiting-line bound, stops that join every thread). The `[threads]` cases also
// run under ThreadSanitizer (a build with `-fsanitize=thread`, `test-replay "[threads]"`), so the program here keeps its
// state per instance; the few atomics below are what the tests observe.
#include <cmdatl/replay/evaluate.hpp>

#include "./run-strategy.hpp"

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
