#include "./broker-folders.hpp"
#include "./paper-support.hpp"
#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace support;

namespace {

/** @brief The real atlc and algotradelib the acceptance cases run against. */
struct Equipment {
  /** @brief atlc's path (`CMDATL_TEST_ATLC`). */
  std::string atlc_;
  /** @brief algotradelib's prefix (`CMDATL_TEST_PREFIX`). */
  std::string prefix_;
};

/**
 * @brief The equipment the environment names.
 * @return It, or nothing when either variable is unset.
 */
std::optional<Equipment> equipment() {
  const auto atlc = cmdatl::variable("CMDATL_TEST_ATLC");
  const auto prefix = cmdatl::variable("CMDATL_TEST_PREFIX");
  if (!atlc || !prefix) {
    return std::nullopt;
  }
  return Equipment{*atlc, *prefix};
}

/** @brief Points cmdatl at the real atlc, and atlc's build at the prefix, for a test's length. */
class Equipped {
public:
  /**
   * @brief Sets the variables.
   * @param equipment The equipment.
   */
  explicit Equipped(const Equipment& equipment) {
    if (const auto* saved = std::getenv("CMAKE_PREFIX_PATH")) {
      saved_ = saved;
    }
    ::setenv("CMDATL_ATLC", equipment.atlc_.c_str(), 1);
    ::setenv("CMAKE_PREFIX_PATH", equipment.prefix_.c_str(), 1);
  }

  Equipped(const Equipped&) = delete;
  Equipped& operator=(const Equipped&) = delete;

  /** @brief Restores them. */
  ~Equipped() {
    ::unsetenv("CMDATL_ATLC");
    if (saved_) {
      ::setenv("CMAKE_PREFIX_PATH", saved_->c_str(), 1);
    } else {
      ::unsetenv("CMAKE_PREFIX_PATH");
    }
  }

private:
  std::optional<std::string> saved_;
};

/** @brief Says why the acceptance cases cannot run here. */
const char* const UNEQUIPPED =
  "CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the acceptance cases did not run";

/**
 * @brief The date `days` after 1970-01-01, as `YYYY-MM-DD` (the civil calendar).
 * @param days The days.
 * @return The date.
 */
std::string date_of(long days) {
  days += 719468;
  const long era = days / 146097;
  const long doe = days - era * 146097;
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long mp = (5 * doy + 2) / 153;
  const long d = doy - (153 * mp + 2) / 5 + 1;
  const long m = mp < 10 ? mp + 3 : mp - 9;
  const long y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  char text[64];
  std::snprintf(text, sizeof(text), "%04ld-%02ld-%02ld", y, m, d);
  return text;
}

/**
 * @brief Writes 400 daily bars of AAA that swing, trend and dip, so that every starter trades.
 * @param path The file.
 */
void write_bars(const std::filesystem::path& path) {
  std::ostringstream out;
  out << "symbol,date,open,high,low,close,volume\n";
  double previous = 100;
  for (int i = 0; i < 400; ++i) {
    double close = 100 + 15 * std::sin(2 * M_PI * i / 60.0) + 5 * std::sin(2 * M_PI * i / 17.0) + 0.05 * i;
    if (i % 37 == 36) {
      close *= 0.85;
    }
    const double open = previous;
    const double high = std::max(open, close) * 1.01;
    const double low = std::min(open, close) * 0.99;
    char row[160];
    std::snprintf(
      row, sizeof(row), "AAA,%s,%.2f,%.2f,%.2f,%.2f,1000\n", date_of(19358 + i).c_str(), open, high, low, close
    );
    out << row;
    previous = close;
  }
  spit(path, out.str());
}

/**
 * @brief Runs `cmdatl strategy backtest` in process, in JSON.
 * @param args What follows `strategy backtest`.
 * @return What it wrote.
 */
Ran backtest(const std::vector<std::string>& args) {
  std::vector<std::string> line{"--json", "strategy", "backtest"};
  line.insert(line.end(), args.begin(), args.end());
  return run(line);
}

/**
 * @brief A backtest's report, which must succeed.
 * @param args What follows `strategy backtest`.
 * @return The report.
 */
boost::json::value report_of(const std::vector<std::string>& args) {
  const auto ran = backtest(args);
  INFO(cmdatl::join(args) << "\n" << ran.out_ << "\n" << ran.err_);
  REQUIRE(ran.exit_ == 0);
  return reply_of(ran.out_).at("report");
}

/**
 * @brief The driver a backtest built or reused: the shape its progress names, so never another build's.
 * @param err What the backtest wrote on stderr.
 * @return The executable, or empty.
 */
std::filesystem::path driver_of(const std::string& err) {
  const std::string said = "cmdatl: building the backtest's driver for ";
  const auto at = err.find(said);
  if (at == std::string::npos) {
    return {};
  }
  const auto in = err.find(" in ", at + said.size());
  return std::filesystem::path(err.substr(in + 4, err.find('\n', in) - in - 4)) / "build" / "driver";
}

/**
 * @brief A program with a series nothing reads, which atlc warns about.
 * @return The program.
 */
std::string program_with_a_warning() {
  std::string text = cmdatl::TEMPLATES[0].text_;
  const std::string series = "series {\n";
  text.insert(text.find(series) + series.size(), "  real unused = sma(close, 5);\n");
  return text;
}

/** @brief What the backtest driver printed and was given, recorded by a stand-in around the driver cmdatl built. */
struct Recorded {
  /** @brief The backtest's exit. */
  int exit_{-1};
  /** @brief The driver's standard output. */
  std::string out_;
  /** @brief The driver's arguments, one per line. */
  std::vector<std::string> args_;
};

/**
 * @brief Runs `strategy backtest` with a stand-in for its driver that records the arguments and the output of the
 *   driver cmdatl built, then runs it.
 * @param dir Where the stand-in and its records go.
 * @param name The records' name.
 * @param driver The driver cmdatl built.
 * @param args What follows `strategy backtest`.
 * @return What the driver printed and was given.
 */
Recorded through_stand_in(
  const std::filesystem::path& dir, const std::string& name, const std::filesystem::path& driver,
  const std::vector<std::string>& args
) {
  const auto stand_in = dir / (name + ".sh");
  spit(
    stand_in,
    "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\" >> '" + stand_in.string() + ".args'; done\n'" +
      driver.string() + "' \"$@\" > '" + stand_in.string() + ".out'\ncode=$?\ncat '" + stand_in.string() +
      ".out'\nexit $code\n"
  );
  std::filesystem::permissions(stand_in, std::filesystem::perms::owner_all);
  const ScopedVariable hook("CMDATL_TEST_BACKTEST_DRIVER", stand_in.string());
  Recorded recorded;
  recorded.exit_ = backtest(args).exit_;
  recorded.out_ = slurp(stand_in.string() + ".out");
  recorded.args_ = lines_of(slurp(stand_in.string() + ".args"));
  return recorded;
}

/**
 * @brief A fixture's text with `@DIR@` replaced.
 * @param text The text.
 * @param dir The directory it stands for.
 * @return The text.
 */
std::string with_dir(std::string text, const std::string& dir) {
  const std::string placeholder = "@DIR@";
  for (auto at = text.find(placeholder); at != std::string::npos; at = text.find(placeholder, at + dir.size())) {
    text.replace(at, placeholder.size(), dir);
  }
  return text;
}

/**
 * @brief The replay fixtures (tests/fixtures/replay/), recorded from `atlc run`.
 * @return Their directory.
 */
std::filesystem::path replay_fixtures() {
  return std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "replay";
}

/**
 * @brief The `error:` line errors.txt records for a spec.
 * @param name The spec's name.
 * @param dir The directory `@DIR@` stands for.
 * @return The line, or empty.
 */
std::string recorded_error(const std::string& name, const std::string& dir) {
  for (const auto& line : lines_of(slurp(replay_fixtures() / "errors.txt"))) {
    if (line.rfind(name + "\t", 0) == 0) {
      return with_dir(line.substr(name.size() + 1), dir);
    }
  }
  return {};
}

/**
 * @brief Checks a backtest's driver was compiled from the replay module and from nothing of atlc's but the runtime
 *   the generated strategy includes, as the compiler's own dependency files list it.
 * @param driver The driver in the build cache.
 */
void check_replay_includes(const std::filesystem::path& driver) {
  std::size_t read = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(driver.parent_path())) {
    if (entry.is_regular_file() && entry.path().extension() == ".d" && entry.path().stem().extension() == ".o") {
      const auto deps = slurp(entry.path());
      ++read;
      CHECK(deps.find("cmdatl/replay/replay.hpp") != std::string::npos);
      // replay.hpp includes "./settings.hpp", so the path reads cmdatl/replay/./settings.hpp.
      CHECK(deps.find("cmdatl/replay/./settings.hpp") != std::string::npos);
      CHECK(deps.find("atls/run.hpp") == std::string::npos);
      CHECK(deps.find("atls/args.hpp") == std::string::npos);
      CHECK(deps.find("atls/ops.hpp") != std::string::npos);
    }
  }
  CHECK(read >= 1);
}

/** @brief One bar of the walk. */
struct WalkBar {
  /** @brief Days after 2020-01-01. */
  long day_;
  /** @brief The open. */
  long open_;
  /** @brief The close. */
  long close_;
};

/**
 * @brief A deterministic walk of whole-number prices (sums stay exact), 2020-01-01 onward, one bar a day.
 * @param days How many.
 * @return The bars.
 */
std::vector<WalkBar> walk(long days) {
  std::uint64_t state = 7;
  const auto next = [&state](long spread) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<long>((state >> 33) % static_cast<std::uint64_t>(2 * spread + 1)) - spread;
  };
  std::vector<WalkBar> bars;
  long close = 1000;
  for (long day = 0; day < days; ++day) {
    const long open = close + next(3);
    close = std::max(close + next(25), 500L);
    bars.push_back({day, open, close});
  }
  return bars;
}

/**
 * @brief The walk as an OHLCV CSV with Unix times.
 * @param bars The bars.
 * @return The text.
 */
std::string walk_csv(const std::vector<WalkBar>& bars) {
  std::string text = "ts,open,high,low,close,volume\n";
  for (const auto& b : bars) {
    text += std::to_string(1577836800 + b.day_ * 86400) + "," + std::to_string(b.open_) + "," +
      std::to_string(std::max(b.open_, b.close_) + 2) + "," + std::to_string(std::min(b.open_, b.close_) - 2) + "," +
      std::to_string(b.close_) + ",1000\n";
  }
  return text;
}

} // namespace

TEST_CASE("each starter written by strategy new backtests and trades from another directory", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  std::filesystem::create_directories(dir.path() / "my strats");
  std::filesystem::create_directories(dir.path() / "work");
  write_bars(dir.path() / "my strats" / "bars.csv");
  const InDir here(dir.path() / "work");
  int traded = 0;
  for (const auto& entry : cmdatl::TEMPLATES) {
    const std::string name = std::string("starter-") + entry.name_;
    INFO(name);
    const auto made = run({"--json", "strategy", "new", name, "--template", entry.name_, "--dir", "../my strats"});
    REQUIRE(made.exit_ == 0);
    const auto next = shell_words(std::string(reply_of(made.out_).at("next").as_array()[0].as_string()));
    REQUIRE(next);
    const auto ran = backtest({next->back()});
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    const auto report = reply_of(ran.out_).at("report").as_object();
    CHECK(report.at("halted").is_null());
    CHECK(report.at("executions").as_int64() > 0);
    traded += report.at("executions").as_int64() > 0 ? 1 : 0;
  }
  CHECK(traded == 4);
}

TEST_CASE(
  "strategy backtest's reports and arguments for the examples are the recorded ones, byte for byte", "[acceptance]"
) {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto examples = std::filesystem::path(CMDATL_SOURCE_DIR) / "examples";
  // Recorded once from atlc 27b912a's `atlc run` (tests/fixtures/examples/how-made.txt).
  const auto fixtures = std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "examples";
  int compared = 0;
  for (const auto* name : {"mean-reversion", "pairs", "trailing-stop", "universe", "user-indicator"}) {
    INFO(name);
    const auto spec = (examples / (std::string(name) + ".toml")).string();
    const auto expected = slurp(fixtures / (std::string(name) + ".json"));
    REQUIRE(!expected.empty());
    const auto built = backtest({spec});
    REQUIRE(built.exit_ == 0);
    CHECK(reply_of(built.out_).at("report") == boost::json::parse(expected));
    // The driver's own bytes and arguments: a stand-in records both, then runs the driver cmdatl built.
    const auto driver = driver_of(built.err_);
    REQUIRE(!driver.empty());
    const auto stand_in = dir.path() / (std::string(name) + ".sh");
    spit(
      stand_in,
      "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\" >> '" + stand_in.string() + ".args'; done\n'" +
        driver.string() + "' \"$@\" > '" + stand_in.string() + ".out'\ncode=$?\ncat '" + stand_in.string() +
        ".out'\nexit $code\n"
    );
    std::filesystem::permissions(stand_in, std::filesystem::perms::owner_all);
    const ScopedVariable hook("CMDATL_TEST_BACKTEST_DRIVER", stand_in.string());
    REQUIRE(backtest({spec}).exit_ == 0);
    CHECK(slurp(stand_in.string() + ".out") == expected);
    std::vector<std::string> args;
    for (auto line : lines_of(slurp(fixtures / (std::string(name) + ".args")))) {
      const std::string placeholder = "@EXAMPLES@";
      if (const auto at = line.find(placeholder); at != std::string::npos) {
        line.replace(at, placeholder.size(), examples.string());
      }
      args.push_back(line);
    }
    CHECK(lines_of(slurp(stand_in.string() + ".args")) == args);
    ++compared;
  }
  CHECK(compared == 5);
}

TEST_CASE(
  "strategy evaluate replays each example as its backtest, and a grid alike on one job and four", "[acceptance]"
) {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const auto examples = std::filesystem::path(CMDATL_SOURCE_DIR) / "examples";
  const auto fixtures = std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "examples";
  for (const auto* name : {"mean-reversion", "pairs", "trailing-stop", "universe", "user-indicator"}) {
    INFO(name);
    const auto ran = run({"--json", "strategy", "evaluate", (examples / (std::string(name) + ".toml")).string()});
    INFO(ran.out_ << ran.err_);
    REQUIRE(ran.exit_ == 0);
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 2);
    CHECK(reply_of(lines[0]).at("report") == boost::json::parse(slurp(fixtures / (std::string(name) + ".json"))));
    CHECK(reply_of(lines[1]).at("replays").as_int64() == 1);
  }
  const auto spec = (examples / "mean-reversion.toml").string();
  const std::vector<std::string> grid{
    "--json", "strategy", "evaluate", spec, "grid.lookback=10,20,30", "grid.entry=1,2",
    "evaluate.objective=total_return"
  };
  auto four = grid;
  four.push_back("evaluate.jobs=4");
  const auto one = run(grid);
  const auto many = run(four);
  REQUIRE(one.exit_ == 0);
  REQUIRE(many.exit_ == 0);
  auto one_lines = lines_of(one.out_);
  auto many_lines = lines_of(many.out_);
  REQUIRE(one_lines.size() == 7);
  REQUIRE(many_lines.size() == 7);
  CHECK(reply_of(many_lines.back()).at("jobs").as_int64() == 4);
  CHECK(reply_of(one_lines.back()).at("best") == reply_of(many_lines.back()).at("best"));
  one_lines.pop_back();
  many_lines.pop_back();
  CHECK(one_lines == many_lines);
  // Names in byte order, the last fastest: set 4 is entry 2, lookback 20, the backtest of those parameters.
  CHECK(reply_of(one_lines[4]).at("report") == report_of({spec, "params.lookback=20", "params.entry=2"}));
}

TEST_CASE("strategy evaluate on two stdin paths reports what two backtests of the same rows report", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto examples = std::filesystem::path(CMDATL_SOURCE_DIR) / "examples";
  std::vector<std::string> rows;
  for (const auto& line : lines_of(slurp(examples / "data" / "daily.csv"))) {
    if (line.find(",AAA,") != std::string::npos) {
      rows.push_back(line);
    }
  }
  REQUIRE(rows.size() == 250);
  const std::string header = "date,symbol,open,high,low,close,volume";
  std::string piped = header + ",path\n";
  std::vector<std::string> files;
  for (const auto& [from, id] : std::vector<std::pair<std::size_t, std::string>>{{0, "early"}, {100, "late"}}) {
    std::string text = header + "\n";
    for (std::size_t i = from; i < from + 150; ++i) {
      text += rows[i] + "\n";
      piped += rows[i] + "," + id + "\n";
    }
    files.push_back((dir.path() / (id + ".csv")).string());
    spit(files.back(), text);
  }
  const std::vector<std::string> common{
    (examples / "mean-reversion.atls").string(), "bars.symbol_column=symbol", "bars.time=date", "bars.time_format=iso",
    "instruments.stock=AAA", "replay.cash=100000", "replay.commission=0.0005"
  };
  // The pipe holds the rows whole, closed, as cmdatl's own stdin, which the driver inherits.
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  REQUIRE(::write(fds[1], piped.data(), piped.size()) == static_cast<ssize_t>(piped.size()));
  ::close(fds[1]);
  const int saved = ::dup(STDIN_FILENO);
  ::dup2(fds[0], STDIN_FILENO);
  ::close(fds[0]);
  std::vector<std::string> args{"--json", "strategy", "evaluate"};
  args.insert(args.end(), common.begin(), common.end());
  args.insert(args.end(), {"bars.source=stdin", "bars.path_column=path"});
  const auto ran = run(args);
  ::dup2(saved, STDIN_FILENO);
  ::close(saved);
  INFO(ran.out_ << ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 3);
  CHECK(reply_of(lines[2]).at("paths").as_int64() == 2);
  for (std::size_t p = 0; p < 2; ++p) {
    auto backtest_args = common;
    backtest_args.insert(backtest_args.end(), {"bars.source=csv", "bars.file=" + files[p]});
    const auto line = reply_of(lines[p]);
    CHECK(line.at("path").as_object().at("number").as_int64() == static_cast<std::int64_t>(p));
    CHECK(line.at("report") == report_of(backtest_args));
  }
}

TEST_CASE("strategy evaluate walks forward by calendar months alike on one job and four", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const auto spec = (std::filesystem::path(CMDATL_SOURCE_DIR) / "examples" / "mean-reversion.toml").string();
  const std::vector<std::string> words{
    "--json", "strategy", "evaluate", spec, "evaluate.method=walk-forward", "walk_forward.train=3mo",
    "walk_forward.test=1mo", "grid.lookback=10,20", "grid.entry=1,2", "evaluate.objective=total_return"
  };
  auto four = words;
  four.push_back("evaluate.jobs=4");
  const auto one = run(words);
  const auto many = run(four);
  INFO(one.out_ << one.err_);
  REQUIRE(one.exit_ == 0);
  REQUIRE(many.exit_ == 0);
  auto one_lines = lines_of(one.out_);
  auto many_lines = lines_of(many.out_);
  const auto reply = reply_of(one_lines.back());
  // From the first bar, 2024-01-02: tests start on the 2nd of April to November; December's would end in 2025.
  const auto& folds = reply.at("folds").as_array();
  REQUIRE(folds.size() == 8);
  CHECK(reply.at("train") == boost::json::value("3mo"));
  CHECK(reply.at("tried") == 4);
  CHECK(reply.at("train_replays") == 32);
  CHECK(reply.at("uncovered_from") == boost::json::value("2024-12-02"));
  double product = 1;
  std::size_t tests = 0;
  for (std::size_t k = 0; k < folds.size(); ++k) {
    const auto& fold = folds[k].as_object();
    INFO(k << " " << boost::json::serialize(fold));
    const std::string month = k < 6 ? "0" + std::to_string(k + 4) : std::to_string(k + 4);
    CHECK(std::string(fold.at("test_from").as_string()).substr(0, 7) == "2024-" + month);
    if (!fold.at("test_index").is_null()) {
      const auto line = reply_of(one_lines[fold.at("test_index").to_number<std::size_t>()]);
      CHECK(line.at("window").at("role") == boost::json::value("test"));
      CHECK(line.at("params") == fold.at("params"));
      product *= 1 + fold.at("test_total_return").to_number<double>();
      ++tests;
    }
  }
  // Every fold has a choice on these bars, so the tests chain.
  REQUIRE(tests == 8);
  REQUIRE(one_lines.size() == 32 + tests + 1);
  REQUIRE(reply.at("chained_null_reason").is_null());
  CHECK(std::abs(reply.at("chained").at("total_return").to_number<double>() - (product - 1)) < 1e-8);
  CHECK(reply_of(many_lines.back()).at("folds") == reply.at("folds"));
  CHECK(reply_of(many_lines.back()).at("chained") == reply.at("chained"));
  one_lines.pop_back();
  many_lines.pop_back();
  CHECK(one_lines == many_lines);
}

TEST_CASE(
  "strategy backtest's settings: limits, parameters, overrides, a program alone, --fills, warnings and failures",
  "[acceptance]"
) {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  std::filesystem::create_directories(dir.path() / "work");
  write_bars(dir.path() / "bars.csv");
  REQUIRE(run({"strategy", "new", "base", "--dir", dir.path().string()}).exit_ == 0);
  const auto spec = slurp(at("base.toml"));
  const auto with = [&spec](const std::string& from, const std::string& to) {
    auto text = spec;
    text.replace(text.find(from), from.size(), to);
    return text;
  };

  // A limit-order spec whose offset makes pricing refuse, from the file or the command line.
  {
    spit(at("limit.toml"), with("type = \"market\"", "type = \"limit\"\noffset = 0.299999999999999999"));
    const auto report = report_of({at("limit.toml")});
    CHECK(report.as_object().at("pricing_refused").as_int64() > 0);
    // Every price times the offset overflows: each target refused for that reason, and nothing sent.
    CHECK(report.as_object().at("executions").as_int64() == 0);
    CHECK(
      report.as_object().at("pricing_refusal_reasons") ==
      boost::json::object{{"price_not_representable", report.as_object().at("pricing_refused")}}
    );
    spit(at("plain.toml"), with("type = \"market\"", ""));
    CHECK(report_of({at("plain.toml"), "orders.type=limit", "orders.offset=0.299999999999999999"}) == report);
  }
  // A parameter on the command line, as the file's.
  {
    const auto ran = backtest({"params.lookback=30", at("base.toml")});
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("settings_given") == boost::json::value(boost::json::array{"params.lookback=30"}));
    spit(at("thirty.toml"), spec + "\n[params]\nlookback = 30\n");
    CHECK(reply.at("report") == report_of({at("thirty.toml")}));
    CHECK(reply.at("report") != report_of({at("base.toml")}));
  }
  // A setting on the command line wins over the file's.
  {
    spit(at("changed.toml"), with("cash = 100000", "cash = 250000"));
    const auto report = report_of({at("base.toml"), "replay.cash=250000"});
    CHECK(report == report_of({at("changed.toml")}));
    CHECK(report.as_object().at("opening_equity").as_string() == "250000");
  }
  // A program with settings only.
  {
    const InDir here(dir.path() / "work");
    const auto ran = backtest(
      {at("base.atls"), "bars.source=csv", "bars.file=../bars.csv", "bars.symbol_column=symbol", "bars.time=date",
        "bars.time_format=iso", "instruments.stock=AAA", "replay.cash=100000", "replay.commission=0.0005"}
    );
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("spec").is_null());
    CHECK(reply.at("program").as_string() == at("base.atls"));
    CHECK(reply.at("report") == report_of({at("base.toml")}));
  }
  // Two backtests of one program at once.
  {
    const auto twenty = report_of({at("base.toml"), "params.lookback=20"});
    const auto forty = report_of({at("base.toml"), "params.lookback=40"});
    Spawned first({"--json", "strategy", "backtest", at("base.toml"), "params.lookback=20"});
    Spawned second({"--json", "strategy", "backtest", at("base.toml"), "params.lookback=40"});
    const int a = first.wait(600);
    const int b = second.wait(600);
    INFO(first.out() << first.err() << second.out() << second.err());
    REQUIRE(WIFEXITED(a));
    REQUIRE(WIFEXITED(b));
    CHECK(WEXITSTATUS(a) == 0);
    CHECK(WEXITSTATUS(b) == 0);
    CHECK(reply_of(first.out()).at("report") == twenty);
    CHECK(reply_of(second.out()).at("report") == forty);
    // Each ran from its own copy, removed after: nothing is left beside the cache.
    const auto copies = std::filesystem::path(*cmdatl::build::cache_root()).parent_path() / "backtests";
    CHECK((!std::filesystem::exists(copies) || std::filesystem::is_empty(copies)));
  }
  // --fills and --equity, relative to the working directory.
  {
    const InDir here(dir.path() / "work");
    const auto ran = backtest({at("base.toml"), "--fills", "fills.csv", "--equity", "equity.csv"});
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("fills").as_string() == (dir.path() / "work" / "fills.csv").string());
    CHECK(reply.at("equity").as_string() == (dir.path() / "work" / "equity.csv").string());
    CHECK(reply.at("report") == report_of({at("base.toml")}));
    const auto& report = reply.at("report").as_object();
    const auto fills = slurp(dir.path() / "work" / "fills.csv");
    CHECK(fills.rfind("at,symbol,side,quantity,price,commission,tax\n", 0) == 0);
    CHECK(static_cast<std::int64_t>(lines_of(fills).size()) == report.at("executions").as_int64() + 1);
    // One instrument on daily bars: a point per bar valued, from the first_bar to the last_bar the report names.
    const auto equity = lines_of(slurp(dir.path() / "work" / "equity.csv"));
    REQUIRE(equity.size() > 2);
    CHECK(equity.front() == "at,equity,period_return");
    CHECK(
      static_cast<std::int64_t>(equity.size()) ==
      report.at("bars").as_int64() - report.at("caveats").as_object().at("unvalued_bars").as_int64() + 1
    );
    CHECK(equity[1].substr(0, equity[1].find(',')) == report.at("first_bar").as_string());
    CHECK(equity.back().substr(0, equity.back().find(',')) == report.at("last_bar").as_string());
    CHECK(equity[1].back() == ',');
  }
  // A warning reaches the reader as progress.
  {
    spit(at("warn.atls"), program_with_a_warning());
    auto warned = spec;
    warned.replace(
      warned.find("strategy = \"base.atls\""), std::string("strategy = \"base.atls\"").size(),
      "strategy = \"warn.atls\""
    );
    spit(at("warn.toml"), warned);
    const auto ran = backtest({at("warn.toml")});
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    CHECK(ran.err_.find("warning[unused-series]") != std::string::npos);
    // A series nothing reads changes no trade.
    CHECK(reply_of(ran.out_).at("report") == report_of({at("base.toml")}));
  }
  // build.algotradelib is refused.
  {
    spit(at("broken.toml"), spec + "\n[build]\nalgotradelib = \"/nonexistent/prefix\"\n");
    const auto ran = backtest({at("broken.toml")});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "spec.invalid");
    CHECK(ran.out_.find("cmdatl env") != std::string::npos);
  }
  // A build failure.
  {
    // A compiler that is not there: CMake cannot configure a shape that is new for it.
    const ScopedVariable compiler("CXX", "/nonexistent/c++");
    const auto ran = backtest({at("base.toml")});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "build.failed");
  }
  // A replay failure: a parameter outside its bounds.
  {
    const auto ran = backtest({at("base.toml"), "params.lookback=1"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "replay.failed");
  }
  // A program error.
  {
    spit(at("bad.atls"), "params {\n  int x = ;\n}\n");
    auto bad = spec;
    bad.replace(
      bad.find("strategy = \"base.atls\""), std::string("strategy = \"base.atls\"").size(), "strategy = \"bad.atls\""
    );
    spit(at("bad.toml"), bad);
    const auto ran = backtest({at("bad.toml")});
    CHECK(ran.exit_ == 5);
    CHECK(code_of(ran.out_) == "program.invalid");
    CHECK(reply_of(ran.out_).at("errors").as_array().at(0).as_object().at("line").as_int64() == 2);
  }
  // A spec error.
  {
    spit(at("typo.toml"), spec + "\n[replay]\ncash = 1\n");
    const auto ran = backtest({at("typo.toml")});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "spec.invalid");
    CHECK(!reply_of(ran.out_).at("errors").as_array().empty());
  }
}

// Compiled only when CMDATL_TEST_DUCKDB named DuckDB's C library at configure time, so a build without it lists no store
// case rather than one that always skips.
#if defined(CMDATL_TEST_DUCKDB)
TEST_CASE("a store backtest links DuckDB from build.duckdb or DUCKDB_ROOT alone", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const std::string duckdb = CMDATL_TEST_DUCKDB;
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto store = dir.path() / "store";
  REQUIRE(cmdatl::run_child({CMDATL_MAKE_STORE, store.string()}, [](const std::string&) {}).code_ == 0);
  REQUIRE(run({"strategy", "new", "dip", "--dir", dir.path().string()}).exit_ == 0);
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  const std::string data = "strategy = 'dip.atls'\n[bars]\nstore = \"test/1d/AAA\"\nstore_root = 'store'\n";
  spit(at("store.toml"), data + "[build]\nduckdb = '" + duckdb + "'\n");
  spit(at("plain.toml"), data);
  boost::json::value expected;
  {
    const ScopedVariable root("DUCKDB_ROOT", std::nullopt);
    expected = report_of({at("store.toml")});
    CHECK(expected.as_object().at("executions").as_int64() > 0);
    CHECK(expected.as_object().at("bars").as_int64() == 120);
    // Without build.duckdb or DUCKDB_ROOT, and none on the system's paths: the build fails and says where it looked.
    const auto none = backtest({at("plain.toml")});
    if (none.exit_ != 0) {
      CHECK(code_of(none.out_) == "build.failed");
      CHECK(none.out_.find("DUCKDB_ROOT") != std::string::npos);
    }
    const auto wrong = backtest({at("plain.toml"), "build.duckdb=" + at("no-duckdb")});
    CHECK(code_of(wrong.out_) == "build.failed");
    CHECK(wrong.out_.find("no-duckdb") != std::string::npos);
  }
  {
    const ScopedVariable root("DUCKDB_ROOT", duckdb);
    const auto ran = backtest({at("plain.toml")});
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("report") == expected);
  }
}
#endif

#if defined(CMDATL_TEST_DUCKDB)
TEST_CASE(
  "a backtest on a broker folder's history fetches into the store, replays that version, and shows no token",
  "[acceptance]"
) {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScopedVariable root("DUCKDB_ROOT", std::string(CMDATL_TEST_DUCKDB));
  // The folder in another project directory, named relative to the spec.
  const BrokerFolders project;
  const std::filesystem::path fixture = std::string(CMDATL_SOURCE_DIR) + "/tests/fixtures/adapters/vendor/adapter";
  const auto folder = project.folder("fixture", slurp(fixture / "broker.toml"), slurp(fixture / "adapter.hpp"));
  const ScratchDir dir;
  REQUIRE(run({"strategy", "new", "dip", "--dir", dir.path().string()}).exit_ == 0);
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  const auto relative = std::filesystem::relative(folder, dir.path()).string();
  spit(
    at("vendor.toml"),
    "strategy = 'dip.atls'\n[bars]\nsource = 'fixture'\nadapter = '" + relative +
      "'\ncount = 150\nstore_root = 'store'\n[bars.options]\nexchange = 'US'\nhistory = 200\n[instruments]\nstock = 'AAA'\n"
      "[replay]\nmarket = 'us'\n"
  );
  // The login as auth login stores it: bound to the folder's canonical path.
  const auto login_for = [](const std::filesystem::path& bound, const std::string& token) {
    return "{\"version\":1,\"adapter\":\"" + bound.string() + "\",\"login\":{\"api_token\":\"" + token + "\"}}";
  };
  spit(at("login.json"), login_for(folder, "T-0123456789"));
  const ScopedVariable login("CMDATL_TEST_FETCH_LOGIN", at("login.json"));
  auto ran = backtest({at("vendor.toml")});
  INFO(ran.out_ << "\n" << ran.err_);
  REQUIRE(ran.exit_ == 0);
  auto reply = reply_of(ran.out_);
  CHECK(reply.at("report").at("bars").as_int64() == 150);
  const auto& fetched = reply.at("fetched").as_object();
  CHECK(fetched.at("source").as_string() == "fixture");
  const auto& series = fetched.at("series").as_array().at(0).as_object();
  CHECK(series.at("symbol").as_string() == "AAA");
  CHECK(series.at("rows").as_int64() == 150);
  CHECK(series.at("new_rows").as_int64() == 150);
  CHECK(std::filesystem::exists(at("store/bars/source=fixture/interval=1d/symbol=AAA/bars.parquet")));
  CHECK(ran.out_.find("T-0123456789") == std::string::npos);
  CHECK(ran.err_.find("T-0123456789") == std::string::npos);
  const auto first = reply.at("report");

  // The same history again: nothing new in the store, the same report.
  ran = backtest({at("vendor.toml")});
  REQUIRE(ran.exit_ == 0);
  reply = reply_of(ran.out_);
  CHECK(reply.at("fetched").at("series").as_array().at(0).at("new_rows").as_int64() == 0);
  CHECK(reply.at("report") == first);

  // An option from the command line, typed by broker.toml, reaches the adapter (a history of 100 bars), and the driver
  // is the same build: options are the run's data, not its shape.
  const auto shape_of = [](const std::string& err) {
    const auto at = err.find("cmdatl: building the backtest's driver for ");
    REQUIRE(at != std::string::npos);
    const auto end = err.find('\n', at);
    return err.substr(err.find(" in ", at) + 4, end - err.find(" in ", at) - 4);
  };
  const auto shape = shape_of(ran.err_);
  ran = backtest({"bars.options.history=100", at("vendor.toml")});
  INFO(ran.out_ << "\n" << ran.err_);
  REQUIRE(ran.exit_ == 0);
  CHECK(reply_of(ran.out_).at("report").at("bars").as_int64() == 100);
  CHECK(shape_of(ran.err_) == shape);
  // Options broker.toml does not declare, or of another type, or outside its choices, or a required one missing:
  // refused before anything is built.
  for (
    const auto& words : std::vector<std::vector<std::string>>{
      {"bars.options.history=1.5"}, {"bars.options.colour=red"}, {"bars.options.exchange=XX"}
    }
  ) {
    auto line = words;
    line.insert(line.begin(), at("vendor.toml"));
    ran = backtest(line);
    CHECK(code_of(ran.out_) == "spec.invalid");
    CHECK(ran.err_.find("building") == std::string::npos);
  }
  auto bare = slurp(at("vendor.toml"));
  spit(at("bare.toml"), bare.replace(bare.find("exchange = 'US'\n"), 16, ""));
  ran = backtest({at("bare.toml")});
  CHECK(code_of(ran.out_) == "spec.invalid");
  CHECK(ran.out_.find("needs `bars.options.exchange`") != std::string::npos);
  CHECK(ran.err_.find("building") == std::string::npos);

  // The version replayed again from the store alone, as the reply says.
  const std::string as_of(reply.at("fetched").at("fetched_at").as_string());
  // The reply's own instant, as it is: the store held nothing else for this series, so the history is the same.
  spit(
    at("again.toml"),
    "strategy = 'dip.atls'\n[bars]\nstore = 'fixture/1d/AAA'\nstore_root = 'store'\nas_of = '" + as_of +
      "'\n[instruments]\nstock = 'AAA'\n[replay]\nmarket = 'us'\n"
  );
  ran = backtest({at("again.toml")});
  INFO(ran.out_);
  REQUIRE(ran.exit_ == 0);
  CHECK(reply_of(ran.out_).at("report") == first);

  // Fewer bars than the store holds: the replay is exactly this fetch's bars. Descriptor 3 is held by something else
  // in this process (as on Linux under ctest): the login reaches the driver anyway, and cmdatl leaves it alone.
  const int held = ::open("/dev/null", O_RDONLY);
  REQUIRE(held >= 0);
  const int saved = ::fcntl(3, F_DUPFD_CLOEXEC, 10);
  REQUIRE(::dup2(held, 3) == 3);
  if (held != 3) {
    ::close(held);
  }
  struct stat before{};
  REQUIRE(::fstat(3, &before) == 0);
  ran = backtest({at("vendor.toml"), "bars.count=20"});
  struct stat after{};
  CHECK(::fstat(3, &after) == 0);
  CHECK((after.st_dev == before.st_dev && after.st_ino == before.st_ino));
  if (saved >= 0) {
    ::dup2(saved, 3);
    ::close(saved);
  } else {
    ::close(3);
  }
  INFO(ran.out_);
  REQUIRE(ran.exit_ == 0);
  CHECK(reply_of(ran.out_).at("report").at("bars").as_int64() == 20);

  // No bars at all: refused, never replaced by the bars the store already holds.
  spit(at("login.json"), login_for(folder, "empty-1"));
  ran = backtest({at("vendor.toml")});
  CHECK(code_of(ran.out_) == "fetch.failed");
  CHECK(ran.out_.find("no bars") != std::string::npos);

  // A token the vendor answers 401 with, in its text: the reply names the status, never the token.
  spit(at("login.json"), login_for(folder, "fail-SECRET-987"));
  ran = backtest({at("vendor.toml")});
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "fetch.failed");
  CHECK(ran.out_.find("HTTP 401") != std::string::npos);
  CHECK(ran.out_.find("SECRET-987") == std::string::npos);
  CHECK(ran.err_.find("SECRET-987") == std::string::npos);

  // A login bound to another folder (the same files in a project cloned elsewhere), and one stored before logins were
  // bound: refused before anything is built or handed on.
  const auto copy = project.folder("copy", slurp(fixture / "broker.toml"), slurp(fixture / "adapter.hpp"));
  spit(at("login.json"), login_for(copy, "OTHER-SECRET-1"));
  ran = backtest({at("vendor.toml")});
  CHECK(code_of(ran.out_) == "auth.adapter_mismatch");
  CHECK(ran.err_.find("building") == std::string::npos);
  CHECK((ran.out_ + ran.err_).find("OTHER-SECRET-1") == std::string::npos);
  spit(at("login.json"), "{\"api_token\":\"OLD-SECRET-1\"}");
  ran = backtest({at("vendor.toml")});
  CHECK(code_of(ran.out_) == "auth.adapter_mismatch");
  CHECK((ran.out_ + ran.err_).find("OLD-SECRET-1") == std::string::npos);

  // No folder at the path, no path, the path on the command line, a fill rule beside the login, no login, and a spec
  // without what the fetch needs.
  const auto other = [&](const std::string& lines) {
    spit(
      at("other.toml"),
      "strategy = 'dip.atls'\n[bars]\nsource = 'fixture'\ncount = 5\nstore_root = 'store'\n" + lines +
        "[instruments]\nstock = 'AAA'\n[replay]\nmarket = 'us'\n"
    );
    return code_of(backtest({at("other.toml")}).out_);
  };
  CHECK(other("adapter = 'nofolder'\n") == "adapter.not_found");
  CHECK(other("") == "spec.invalid");
  CHECK(code_of(backtest({at("vendor.toml"), "bars.adapter=" + copy.string()}).out_) == "spec.invalid");
  CHECK(code_of(backtest({at("vendor.toml"), "bars.count=0"}).out_) == "spec.invalid");
  std::filesystem::remove(at("login.json"));
  CHECK(code_of(backtest({at("vendor.toml")}).out_) == "auth.not_found");
}
#endif

TEST_CASE("the replay fixtures recorded from atlc run are strategy backtest's byte for byte", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  for (const auto& entry : std::filesystem::directory_iterator(replay_fixtures())) {
    std::filesystem::copy_file(entry.path(), dir.path() / entry.path().filename());
  }
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  const auto fixture = [&dir](const std::string& file) {
    return with_dir(slurp(replay_fixtures() / file), dir.path().string());
  };
  // The wide tables at the decision close (`replay.fill`), and limits on the KRX grid with their fills.
  const auto wide = backtest({at("wide.toml")});
  REQUIRE(wide.exit_ == 0);
  REQUIRE(reply_of(wide.out_).at("report") == boost::json::parse(fixture("wide.json")));
  // The same price by a header rule of one's own: the same report but for the rule's name, the header's function.
  auto by_header = slurp(at("wide.toml"));
  const std::string preset = "fill = \"decision-close\"\n";
  REQUIRE(by_header.find(preset) != std::string::npos);
  by_header.replace(
    by_header.find(preset), preset.size(), "fill_header = \"decision-close.hpp\"\nfill_function = \"decision_close\"\n"
  );
  spit(dir.path() / "wide-header.toml", by_header);
  auto header_report = boost::json::parse(fixture("wide.json"));
  header_report.as_object()["caveats"].as_object()["market_price_rule"] = "decision_close";
  const auto ruled_run = backtest({at("wide-header.toml")});
  REQUIRE(ruled_run.exit_ == 0);
  REQUIRE(reply_of(ruled_run.out_).at("report") == header_report);
  REQUIRE(
    report_of({at("limit.toml"), "--fills", at("limit-cmdatl.csv")}) == boost::json::parse(fixture("limit.json"))
  );
  CHECK(slurp(at("limit-cmdatl.csv")) == fixture("limit.fills.csv"));
  const auto driver = driver_of(wide.err_);
  REQUIRE(!driver.empty());
  check_replay_includes(driver);
  // A header rule makes a shape of its own; `replay.fill` does not.
  const auto ruled = driver_of(ruled_run.err_);
  REQUIRE(!ruled.empty());
  CHECK(ruled != driver);
  // A pricing the generated strategy refuses stops the driver before it replays (the run spec refuses it first; this
  // is a C++ caller's guard).
  const auto direct = [&driver, &fixture](const std::string& type, const std::string& offset) {
    auto args = lines_of(fixture("limit.args"));
    args.insert(args.begin(), driver.string());
    args.insert(args.end(), {"orders.type=" + type, "orders.offset=" + offset});
    return cmdatl::run_child(args, [](const std::string&) {});
  };
  const auto market_offset = direct("market", "0.001");
  CHECK(market_offset.code_ == 1);
  CHECK(cmdatl::last_line(market_offset).find("an offset prices a limit") != std::string::npos);
  const auto wide_offset = direct("limit", "0.3");
  CHECK(wide_offset.code_ == 1);
  CHECK(cmdatl::last_line(wide_offset).find("below 0.3") != std::string::npos);
  for (const auto* name : {"wide", "limit"}) {
    INFO(name);
    const auto recorded = through_stand_in(dir.path(), name, driver, {at(std::string(name) + ".toml")});
    CHECK(recorded.exit_ == 0);
    CHECK(recorded.out_ == fixture(std::string(name) + ".json"));
    CHECK(recorded.args_ == lines_of(fixture(std::string(name) + ".args")));
  }
  // The replay's refusals: a CSV date that does not exist, wide dates that do not increase, a parameter bound.
  for (const auto* name : {"bad-date", "wide-order", "bound"}) {
    INFO(name);
    const auto expected = recorded_error(name, dir.path().string());
    REQUIRE(!expected.empty());
    const auto ran = backtest({at(std::string(name) + ".toml")});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "replay.failed");
    CHECK(reply_of(ran.out_).at("error").as_object().at("message").as_string() == "the replay failed: " + expected);
  }
}

#if defined(CMDATL_TEST_DUCKDB)
TEST_CASE("the store fixture recorded from atlc run is strategy backtest's byte for byte", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const std::string duckdb = "build.duckdb=" + std::string(CMDATL_TEST_DUCKDB);
  const Equipped equipped(*equip);
  const ScratchDir dir;
  for (const auto* file : {"dip.atls", "store.toml", "missing.toml"}) {
    std::filesystem::copy_file(replay_fixtures() / file, dir.path() / file);
  }
  REQUIRE(cmdatl::run_child({CMDATL_MAKE_STORE, (dir.path() / "store").string()}, [](const std::string&) {
  }).code_ == 0);
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  const auto fixture = [&dir](const std::string& file) {
    return with_dir(slurp(replay_fixtures() / file), dir.path().string());
  };
  const auto built = backtest({at("store.toml"), duckdb});
  REQUIRE(built.exit_ == 0);
  REQUIRE(reply_of(built.out_).at("report") == boost::json::parse(fixture("store.json")));
  const auto driver = driver_of(built.err_);
  REQUIRE(!driver.empty());
  check_replay_includes(driver);
  const auto recorded = through_stand_in(dir.path(), "store", driver, {at("store.toml"), duckdb});
  CHECK(recorded.exit_ == 0);
  CHECK(recorded.out_ == fixture("store.json"));
  CHECK(recorded.args_ == lines_of(fixture("store.args")));
  const auto expected = recorded_error("missing", dir.path().string());
  REQUIRE(!expected.empty());
  const auto ran = backtest({at("missing.toml"), duckdb});
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "replay.failed");
  CHECK(reply_of(ran.out_).at("error").as_object().at("message").as_string() == "the replay failed: " + expected);
}
#endif

TEST_CASE("every strategy reference the docs and skills name answers from the real atlc", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(
      "CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the acceptance cases did not run"
    );
  }
  const ScopedVariable atlc("CMDATL_ATLC", equip->atlc_);
  const std::filesystem::path root(CMDATL_SOURCE_DIR);
  std::vector<std::filesystem::path> files{root / "README.md"};
  for (const auto* dir : {"docs", "skills"}) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / dir)) {
      if (entry.path().extension() == ".md") {
        files.push_back(entry.path());
      }
    }
  }
  // Each `cmdatl strategy reference` with or without a topic, as written; a topic is atlc's file stem, known only to
  // the atlc that runs, so only the real one can say each exists.
  // In a code span, or a fenced line that is the command alone.
  const std::regex named(
    "`cmdatl strategy reference( ([a-z][a-z0-9-]*))?`|^cmdatl strategy reference( ([a-z][a-z0-9-]*))?$"
  );
  std::set<std::string> topics;
  // The chapters a line offers beside it, as in "(or `run-spec`, `syntax`)".
  const std::regex offered("`([a-z][a-z0-9-]*)`");
  for (const auto& file : files) {
    for (const auto& line : lines_of(slurp(file))) {
      for (std::sregex_iterator it(line.begin(), line.end(), named), end; it != end; ++it) {
        topics.insert((*it)[2].matched ? (*it)[2].str() : (*it)[4].str());
      }
      const auto open = line.find("(or `");
      if (line.find("cmdatl strategy reference") == std::string::npos || open == std::string::npos) {
        continue;
      }
      const auto inside = line.substr(open, line.find(')', open) - open);
      for (std::sregex_iterator it(inside.begin(), inside.end(), offered), end; it != end; ++it) {
        topics.insert((*it)[1].str());
      }
    }
  }
  REQUIRE(topics.count("run-spec") == 1);
  REQUIRE(topics.count("") == 1);
  REQUIRE(topics.count("builtins") == 1);
  for (const auto& topic : topics) {
    INFO(topic);
    std::vector<std::string> line{"--json", "strategy", "reference"};
    if (!topic.empty()) {
      line.push_back(topic);
    }
    const auto ran = run(line);
    CHECK(ran.exit_ == 0);
    CHECK(!reply_of(ran.out_).at("text").as_string().empty());
  }
  const auto wrong = run({"--json", "strategy", "reference", "indicators"});
  CHECK(wrong.exit_ == 2);
  CHECK(code_of(wrong.out_) == "usage.bad_value");
}
#endif

TEST_CASE("an on_start block runs once, before the first bar", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  spit(at("bars.csv"), walk_csv(walk(30)));
  // The size is set only if no bar came first; any later or repeated call leaves it zero.
  spit(
    at("start.atls"),
    "state {\n  int calls = 0;\n  int size = 0;\n}\non_start {\n  size = calls == 0 ? 10 : 0;\n}\n"
    "on_bar {\n  calls = calls + 1;\n  target(size);\n}\n"
  );
  spit(at("start.toml"), "strategy = 'start.atls'\n[bars]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol = \"AAA\"\n");
  const auto report = report_of({at("start.toml")});
  CHECK(report.as_object().at("halted").is_null());
  CHECK(report.as_object().at("executions").as_int64() == 1);
}

TEST_CASE("a program's data {} reads its data file in time order with the bars", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  const auto bars = walk_csv(walk(30));
  spit(at("bars.csv"), bars);
  // The bars' own times: the value 1 is known with the 10th bar, 0 with the 20th.
  std::vector<std::string> times;
  std::istringstream lines(bars);
  std::string line;
  std::getline(lines, line);
  while (std::getline(lines, line)) {
    times.push_back(line.substr(0, line.find(',')));
  }
  REQUIRE(times.size() == 30);
  // And one after the last bar, inside the replay: fed and counted, though no bar decides on it.
  spit(
    at("level.csv"),
    "ts,level\n" + times[9] + ",1\n" + times[19] + ",0\n" + std::to_string(std::stoll(times[29]) + 3600) + ",1\n"
  );
  spit(at("follow.atls"), "data { real level; }\non_bar {\n  target(level);\n}\n");
  const std::string base =
    "strategy = 'follow.atls'\n[bars]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol = \"AAA\"\n";
  spit(at("follow.toml"), base + "[data.level]\nfile = \"level.csv\"\n");
  const auto report = report_of({at("follow.toml")});
  CHECK(report.as_object().at("records").as_int64() == 3);
  CHECK(report.as_object().at("executions").as_int64() == 2);
}

/**
 * @brief A scratch directory holding the measures fixture, 30 bars and a program buying one share.
 * @param dir The directory.
 * @param name The run spec's name.
 * @param measures The run spec's `measures` line, or empty for a spec without them.
 * @return The run spec.
 */
std::string measured_spec(const ScratchDir& dir, const std::string& name, const std::string& measures) {
  std::filesystem::copy(
    std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "measures" / "measures.hpp",
    dir.path() / "measures.hpp", std::filesystem::copy_options::overwrite_existing
  );
  spit(dir.path() / "bars.csv", walk_csv(walk(30)));
  spit(dir.path() / "buy.atls", "on_bar {\n  target(1);\n}\n");
  const auto spec = (dir.path() / (name + ".toml")).string();
  spit(
    spec,
    "strategy = 'buy.atls'\nheader = 'measures.hpp'\n" + measures +
      "\n[bars]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol = \"AAA\"\n"
  );
  return spec;
}

TEST_CASE("a measure that is not one fails the build in one line", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  for (const std::string name : {"wrong", "undeclared", "explicit_only"}) {
    const auto failed = backtest({measured_spec(dir, name, "measures = ['total_again', '" + name + "']")});
    INFO(failed.out_);
    CHECK(failed.exit_ == 1);
    CHECK(code_of(failed.out_) == "build.failed");
    const std::string message(reply_of(failed.out_).at("error").as_object().at("message").as_string());
    // The compiler's first error is the assertion's line (a short stderr is given whole, after `In file included`).
    const auto first = message.find("error: ");
    REQUIRE(first != std::string::npos);
    CHECK(
      message.substr(first, message.find('\n', first) - first)
        .find("measures: " + name + " must be callable as double(const algotradelib::backtest::MeasureInput&)") !=
      std::string::npos
    );
  }
}

// Hidden until atlc's published package has `measures {}` (acceptance.sh fails on a skipped case); run it with
// CMDATL_TEST_ATLC_MEASURES naming such an atlc: `test-cli "[atls-measures]"`. Then it becomes an acceptance case.
TEST_CASE("a program's measures {} come first in the report, before the run spec's", "[.][atls-measures]") {
  const auto equip = equipment();
  const auto atlc = cmdatl::variable("CMDATL_TEST_ATLC_MEASURES");
  if (!equip || !atlc) {
    SKIP(
      "CMDATL_TEST_ATLC, CMDATL_TEST_PREFIX and CMDATL_TEST_ATLC_MEASURES name no atlc, prefix and atlc with "
      "measures {}: the program's measures were not built"
    );
  }
  const Equipped equipped(Equipment{*atlc, equip->prefix_});
  const ScratchDir dir;
  const auto plain = report_of({measured_spec(dir, "plain", "")});
  // The fixture's spec, with the program given measures after measured_spec writes the plain one.
  const auto with_measures = [&dir](const std::string& name, const std::string& line) {
    const auto spec = measured_spec(dir, name, line);
    spit(
      dir.path() / "buy.atls",
      "on_bar {\n  target(1);\n}\nmeasures {\n  real total = cum_last(equity) / cum_first(equity) - 1;\n"
      "  real one = 1;\n}\n"
    );
    return spec;
  };
  auto report = report_of({with_measures("both", "measures = ['total_again']")});
  const auto measures = report.as_object().at("measures").as_object();
  std::vector<std::string> keys;
  for (const auto& entry : measures) {
    keys.emplace_back(entry.key());
  }
  CHECK(keys == std::vector<std::string>{"total", "one", "total_again"});
  CHECK(measures.at("total").to_number<double>() == measures.at("total_again").to_number<double>());
  CHECK(measures.at("one").to_number<double>() == 1);
  report.as_object().erase("measures");
  CHECK(report == plain);
  // A spec measure named like the program's is refused before anything is built.
  const auto clash = backtest({with_measures("clash", "measures = ['one']")});
  CHECK(clash.exit_ == 2);
  CHECK(code_of(clash.out_) == "spec.invalid");
  CHECK(
    reply_of(clash.out_).at("error").as_object().at("message").as_string() ==
    "`measures`: \"one\" is the program's own measure"
  );
  // A program's measure that throws fails the replay as the spec's does, naming it.
  const auto fills = dir.path() / "fills.csv";
  const auto thrower = measured_spec(dir, "thrower", "");
  spit(
    dir.path() / "measures.hpp",
    slurp(dir.path() / "measures.hpp") +
      "\nnamespace atls::user {\ninline double boom(double) {\n  throw std::runtime_error(\"deliberately\");\n}\n}\n"
  );
  spit(
    dir.path() / "buy.atls",
    "functions {\n  extern real boom(real x);\n}\non_bar {\n  target(1);\n}\n"
    "measures {\n  real b = cum_mean(boom(ret));\n}\n"
  );
  const auto threw = backtest({thrower, "--fills", fills.string()});
  CHECK(threw.exit_ == 1);
  CHECK(code_of(threw.out_) == "replay.failed");
  CHECK(
    std::string(reply_of(threw.out_).at("error").as_object().at("message").as_string())
      .find("the measure b threw: deliberately") != std::string::npos
  );
  CHECK_FALSE(std::filesystem::exists(fills));
  // On the loops' wide closes, the example with a measure of its total return: bootstrap gives it an interval, and
  // walk-forward chooses by it as by total_return.
  const auto source = std::filesystem::path(CMDATL_SOURCE_DIR);
  std::filesystem::copy(source / "tests" / "fixtures" / "loops" / "closes.csv", dir.path() / "closes.csv");
  spit(
    dir.path() / "mr.atls",
    slurp(source / "examples" / "mean-reversion.atls") +
      "measures {\n  real total = cum_last(equity) / cum_first(equity) - 1;\n}\n"
  );
  auto loops = slurp(source / "tests" / "fixtures" / "loops" / "closes.toml");
  loops.replace(loops.find("../../../examples/mean-reversion.atls"), 37, "mr.atls");
  spit(dir.path() / "mr.toml", loops);
  const auto evaluate = [&dir](std::vector<std::string> more) {
    std::vector<std::string> args{"--json", "strategy", "evaluate", (dir.path() / "mr.toml").string()};
    args.insert(args.end(), more.begin(), more.end());
    const auto ran = run(args);
    INFO(ran.out_ << ran.err_);
    REQUIRE(ran.exit_ == 0);
    return reply_of(lines_of(ran.out_).back());
  };
  const auto boot = evaluate(
    {"evaluate.method=bootstrap", "bootstrap.seed=7", "bootstrap.block=5", "bootstrap.paths=5",
      "bootstrap.intervals=[\"measures.total\", \"total_return\"]"}
  );
  const auto& intervals = boot.at("intervals").as_object();
  REQUIRE(intervals.contains("measures.total"));
  for (const auto* part : {"lower", "median", "upper"}) {
    CHECK(
      intervals.at("measures.total").at(part).to_number<double>() ==
      intervals.at("total_return").at(part).to_number<double>()
    );
  }
  const std::vector<std::string> walk{
    "evaluate.method=walk-forward", "walk_forward.train=30", "walk_forward.test=15", "grid.lookback=5,10"
  };
  auto by_measure = walk;
  by_measure.push_back("evaluate.objective=measures.total");
  auto by_figure = walk;
  by_figure.push_back("evaluate.objective=total_return");
  const auto measured = evaluate(by_measure);
  const auto figured = evaluate(by_figure);
  CHECK(measured.at("objective") == boost::json::value("measures.total"));
  REQUIRE(!measured.at("folds").as_array().empty());
  CHECK(measured.at("folds") == figured.at("folds"));
}

TEST_CASE("strategy evaluate windows and bootstrap with atlc: spans; history counted apart; path 0", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const auto source = std::filesystem::path(CMDATL_SOURCE_DIR);
  const auto spec = (source / "tests" / "fixtures" / "loops" / "closes.toml").string();
  const auto evaluate = [&](const std::string& target, std::vector<std::string> more) {
    std::vector<std::string> args{"--json", "strategy", "evaluate", target};
    args.insert(args.end(), more.begin(), more.end());
    const auto ran = run(args);
    INFO(ran.out_ << ran.err_);
    REQUIRE(ran.exit_ == 0);
    return lines_of(ran.out_);
  };
  const auto backtest = [&](const std::string& target, std::vector<std::string> more) {
    std::vector<std::string> args{"--json", "strategy", "backtest", target};
    args.insert(args.end(), more.begin(), more.end());
    const auto ran = run(args);
    INFO(ran.out_ << ran.err_);
    REQUIRE(ran.exit_ == 0);
    return reply_of(lines_of(ran.out_).back()).at("report");
  };
  const auto date = [](const boost::json::value& at) {
    return std::string(at.as_string()).substr(0, 10);
  };
  // A window without warm-up reports what a backtest of its dates does.
  const auto windows = evaluate(spec, {"evaluate.method=windows", "windows.length=20", "windows.warmup=0"});
  REQUIRE(windows.size() == 5);
  const auto window = reply_of(windows[1]);
  const auto& w = window.at("window").as_object();
  CHECK(window.at("report") == backtest(spec, {"replay.from=" + date(w.at("from")), "replay.to=" + date(w.at("to"))}));
  // Months from the first date, 2024-01-02: three whole windows; the fourth would end after the history.
  CHECK(!reply_of(windows.back()).contains("summary"));
  const auto months = evaluate(
    spec, {"evaluate.method=windows", "windows.length=1mo", "windows.warmup=0", "windows.summary=[\"max\", \"q00\"]"}
  );
  REQUIRE(months.size() == 4);
  CHECK(date(reply_of(months[1]).at("window").at("from")) == "2024-02-02");
  const auto months_reply = reply_of(months.back());
  CHECK(months_reply.at("windows").as_int64() == 3);
  // The statistics named, in order, of the three windows' figures.
  std::vector<std::int64_t> executions;
  for (std::size_t k = 0; k < 3; ++k) {
    executions.push_back(reply_of(months[k]).at("report").at("executions").as_int64());
  }
  CHECK(
    months.back().find(
      "\"executions\":{\"defined\":3,\"null\":0,\"max\":" +
      std::to_string(*std::max_element(executions.begin(), executions.end())) +
      ",\"q00\":" + std::to_string(*std::min_element(executions.begin(), executions.end())) + "}"
    ) != std::string::npos
  );
  CHECK(months_reply.at("uncovered_from") == boost::json::value("2024-04-02"));
  // A universe with open points: two history days, then two traded days that open and close two sessions.
  const auto universe = evaluate(
    (source / "examples" / "universe.toml").string(),
    {"params.lookback=1", "evaluate.method=windows", "windows.length=2", "windows.warmup=2", "replay.from=20240102",
      "replay.to=20240105"}
  );
  REQUIRE(universe.size() == 2);
  const auto first = reply_of(universe[0]);
  const auto& caveats = first.at("report").at("caveats").as_object();
  CHECK(caveats.at("sessions_opened").as_int64() == 2);
  CHECK(caveats.at("sessions_closed").as_int64() == 2);
  // Bootstrap: path 0 is the backtest; seed 7, circular blocks of 5, three paths, as recorded.
  const auto paths = evaluate(
    spec,
    {"evaluate.method=bootstrap", "bootstrap.seed=7", "bootstrap.block=5", "bootstrap.paths=3",
      "bootstrap.intervals=[\"total_return\", \"max_drawdown\"]"}
  );
  REQUIRE(paths.size() == 5);
  CHECK(reply_of(paths[0]).at("report") == backtest(spec, {}));
  std::string recorded;
  for (std::size_t k = 0; k < 4; ++k) {
    recorded += paths[k] + "\n";
  }
  CHECK(recorded == slurp(source / "tests" / "fixtures" / "loops" / "bootstrap-seed7.jsonl"));
  const auto reply = reply_of(paths.back());
  CHECK(reply.at("seed").as_int64() == 7);
  CHECK(
    reply.at("intervals").at("total_return").at("defined").as_int64() +
      reply.at("intervals").at("total_return").at("null").as_int64() ==
    3
  );
}
