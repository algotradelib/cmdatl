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
#include <tuple>
#include <vector>

#if !defined(_WIN32)
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
  char text[16];
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
 * @brief The struct atlc names a program, from `atlc check FILE --info`.
 * @param atlc atlc's path.
 * @param program The program.
 * @return The struct, or empty.
 */
std::string struct_of(const std::string& atlc, const std::string& program) {
  const auto result = cmdatl::run_child({atlc, "check", program, "--info"}, [](const std::string&) {});
  const auto info = cmdatl::spec::read_info(result.stdout_, program);
  return info && info->ok_ ? info->program_.struct_ : std::string();
}

/**
 * @brief The backtest driver cmdatl built for a struct, in the cache.
 * @param structure The struct.
 * @return The executable, or empty.
 */
std::filesystem::path backtest_driver(const std::string& structure) {
  const auto root = cmdatl::build::cache_root();
  if (!root || !std::filesystem::exists(*root)) {
    return {};
  }
  std::filesystem::path found;
  std::filesystem::file_time_type newest{};
  for (const auto& entry : std::filesystem::directory_iterator(*root)) {
    const auto exe = entry.path() / "build" / "driver";
    if (
      entry.path().filename().string().rfind(structure + "-", 0) == 0 && std::filesystem::exists(exe) &&
      slurp(entry.path() / "src" / "program.hpp").find("cmdatl strategy backtest") != std::string::npos &&
      (found.empty() || std::filesystem::last_write_time(exe) > newest)
    ) {
      found = exe;
      newest = std::filesystem::last_write_time(exe);
    }
  }
  return found;
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

/** @brief A fill as (day, side, quantity, price). */
using WalkFill = std::tuple<long, std::string, long, double>;

/**
 * @brief The fills the mean-reversion starter must give, replayed here: the sample z-score of the closes over
 *   `lookback` bars, a target of `quantity` below `-entry` and of 0 above `exit`, each order filled at the next bar's
 *   open.
 * @param bars The bars.
 * @param lookback The window.
 * @param entry The entry threshold.
 * @param exit The exit threshold.
 * @param quantity The target.
 * @return The fills.
 */
std::vector<WalkFill> expected_fills(
  const std::vector<WalkBar>& bars, std::size_t lookback, double entry, double exit, long quantity
) {
  std::vector<WalkFill> fills;
  long position = 0;
  long pending = 0;
  for (std::size_t i = 0; i < bars.size(); ++i) {
    const auto& b = bars[i];
    if (pending != 0) {
      fills.emplace_back(b.day_, pending > 0 ? "buy" : "sell", std::labs(pending), static_cast<double>(b.open_));
      position += pending;
      pending = 0;
    }
    if (i + 1 < lookback) {
      continue;
    }
    double sum = 0;
    for (std::size_t k = i + 1 - lookback; k <= i; ++k) {
      sum += static_cast<double>(bars[k].close_);
    }
    const double mean = sum / static_cast<double>(lookback);
    double squares = 0;
    for (std::size_t k = i + 1 - lookback; k <= i; ++k) {
      const double d = static_cast<double>(bars[k].close_) - mean;
      squares += d * d;
    }
    const double var = squares / static_cast<double>(lookback - 1);
    if (var == 0.0) {
      continue;
    }
    const double z = (static_cast<double>(b.close_) - mean) / std::sqrt(var);
    const long target = z < -entry ? quantity : (z > exit ? 0 : position);
    pending = target - position;
  }
  return fills;
}

/**
 * @brief Days from 2020-01-01 to a time of 2020 (a leap year) written `2020-MM-DDT...`.
 * @param at The time.
 * @return The days, or -1 for another year.
 */
long day_of_2020(const std::string& at) {
  static constexpr int before[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
  if (at.size() < 10 || at.compare(0, 5, "2020-") != 0) {
    return -1;
  }
  return before[std::stoi(at.substr(5, 2)) - 1] + std::stoi(at.substr(8, 2)) - 1;
}

/**
 * @brief A fills file as (day, side, quantity, price).
 * @param text The file.
 * @return The fills.
 */
std::vector<WalkFill> read_fills(const std::string& text) {
  std::vector<WalkFill> fills;
  auto rows = lines_of(text);
  if (rows.empty() || rows.front() != "at,symbol,side,quantity,price,commission,tax") {
    return fills;
  }
  for (std::size_t k = 1; k < rows.size(); ++k) {
    const auto cells = cmdatl::split(rows[k], ',');
    if (cells.size() != 7) {
      return {};
    }
    fills.emplace_back(day_of_2020(cells[0]), cells[2], std::stol(cells[3]), std::stod(cells[4]));
  }
  return fills;
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
  // Recorded once from atlc 3f9ce62's `atlc run` (tests/fixtures/examples/how-made.txt).
  const auto fixtures = std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "examples";
  int compared = 0;
  for (const auto* name : {"mean-reversion", "pairs", "trailing-stop", "universe", "user-indicator"}) {
    INFO(name);
    const auto spec = (examples / (std::string(name) + ".toml")).string();
    const auto expected = slurp(fixtures / (std::string(name) + ".json"));
    REQUIRE(!expected.empty());
    CHECK(report_of({spec}) == boost::json::parse(expected));
    // The driver's own bytes and arguments: a stand-in records both, then runs the driver cmdatl built.
    const auto structure = struct_of(equip->atlc_, (examples / (std::string(name) + ".atls")).string());
    REQUIRE(!structure.empty());
    const auto driver = backtest_driver(structure);
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

  SECTION("a limit-order spec whose offset makes pricing refuse, from the file or the command line") {
    spit(at("limit.toml"), with("type = \"market\"", "type = \"limit\"\noffset = 0.299999999999999999"));
    const auto report = report_of({at("limit.toml")});
    CHECK(report.as_object().at("pricing_refused").as_int64() > 0);
    spit(at("plain.toml"), with("type = \"market\"", ""));
    CHECK(report_of({at("plain.toml"), "orders.type=limit", "orders.offset=0.299999999999999999"}) == report);
  }
  SECTION("a parameter on the command line, as the file's") {
    const auto ran = backtest({"params.lookback=30", at("base.toml")});
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("settings_given") == boost::json::value(boost::json::array{"params.lookback=30"}));
    spit(at("thirty.toml"), spec + "\n[params]\nlookback = 30\n");
    CHECK(reply.at("report") == report_of({at("thirty.toml")}));
    CHECK(reply.at("report") != report_of({at("base.toml")}));
  }
  SECTION("a setting on the command line wins over the file's") {
    spit(at("changed.toml"), with("cash = 100000", "cash = 250000"));
    const auto report = report_of({at("base.toml"), "replay.cash=250000"});
    CHECK(report == report_of({at("changed.toml")}));
    CHECK(report.as_object().at("opening_equity").as_string() == "250000");
  }
  SECTION("a program with settings only") {
    const InDir here(dir.path() / "work");
    const auto ran = backtest(
      {at("base.atls"), "data.source=csv", "data.file=../bars.csv", "data.symbol_column=symbol", "data.time=date",
        "data.time_format=iso", "instruments.stock=AAA", "replay.cash=100000", "replay.commission=0.0005"}
    );
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("spec").is_null());
    CHECK(reply.at("program").as_string() == at("base.atls"));
    CHECK(reply.at("report") == report_of({at("base.toml")}));
  }
  SECTION("two backtests of one program at once") {
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
  SECTION("--fills, relative to the working directory") {
    const InDir here(dir.path() / "work");
    const auto ran = backtest({at("base.toml"), "--fills", "fills.csv"});
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("fills").as_string() == (dir.path() / "work" / "fills.csv").string());
    CHECK(reply.at("report") == report_of({at("base.toml")}));
    const auto fills = slurp(dir.path() / "work" / "fills.csv");
    CHECK(fills.rfind("at,symbol,side,quantity,price,commission,tax\n", 0) == 0);
    CHECK(
      static_cast<std::int64_t>(lines_of(fills).size()) ==
      reply.at("report").as_object().at("executions").as_int64() + 1
    );
  }
  SECTION("a warning reaches the reader as progress") {
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
  SECTION("build.algotradelib is refused") {
    spit(at("broken.toml"), spec + "\n[build]\nalgotradelib = \"/nonexistent/prefix\"\n");
    const auto ran = backtest({at("broken.toml")});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "spec.invalid");
    CHECK(ran.out_.find("cmdatl env") != std::string::npos);
  }
  SECTION("a build failure") {
    // A compiler that is not there: CMake cannot configure a shape that is new for it.
    const ScopedVariable compiler("CXX", "/nonexistent/c++");
    const auto ran = backtest({at("base.toml")});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "build.failed");
  }
  SECTION("a replay failure: a parameter outside its bounds") {
    const auto ran = backtest({at("base.toml"), "params.lookback=1"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "replay.failed");
  }
  SECTION("a program error") {
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
  SECTION("a spec error") {
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
  const std::string data = "strategy = 'dip.atls'\n[data]\nstore = \"test/1d/AAA\"\nstore_root = 'store'\n";
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
  // The wide tables read with open-prev-close, and limit orders on the KRX grid with their fills.
  REQUIRE(report_of({at("wide.toml")}) == boost::json::parse(fixture("wide.json")));
  REQUIRE(
    report_of({at("limit.toml"), "--fills", at("limit-cmdatl.csv")}) == boost::json::parse(fixture("limit.json"))
  );
  CHECK(slurp(at("limit-cmdatl.csv")) == fixture("limit.fills.csv"));
  const auto structure = struct_of(equip->atlc_, at("dip.atls"));
  REQUIRE(!structure.empty());
  const auto driver = backtest_driver(structure);
  REQUIRE(!driver.empty());
  check_replay_includes(driver);
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

TEST_CASE("strategy backtest's fills are those of a replay computed in the test", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto at = [&dir](const std::string& file) {
    return (dir.path() / file).string();
  };
  const auto bars = walk(120);
  spit(at("bars.csv"), walk_csv(bars));
  // The mean-reversion starter: a z-score of the closes, entry and exit thresholds, a quantity.
  spit(at("mr.atls"), cmdatl::TEMPLATES[0].text_);
  const std::string data = "[data]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol = \"AAA\"\n";
  spit(at("mr.toml"), "strategy = 'mr.atls'\n" + data + "[replay]\ncash = 100_000\n[params]\nentry = 1\n");
  for (const std::size_t lookback : {5, 8}) {
    INFO(lookback);
    const auto fills = at("fills-" + std::to_string(lookback) + ".csv");
    const auto report =
      report_of({at("mr.toml"), "params.lookback=" + std::to_string(lookback), "--fills", fills}).as_object();
    const auto want = expected_fills(bars, lookback, 1.0, 0.0, 100);
    CHECK(read_fills(slurp(fills)) == want);
    CHECK(want.size() >= 6);
    long position = 0;
    long cash = 100000;
    for (const auto& [day, side, quantity, price] : want) {
      position += side == "buy" ? quantity : -quantity;
      cash -= (side == "buy" ? quantity : -quantity) * static_cast<long>(price);
    }
    CHECK(report.at("executions").as_int64() == static_cast<std::int64_t>(want.size()));
    CHECK(report.at("bars").as_int64() == static_cast<std::int64_t>(bars.size()));
    CHECK(report.at("opening_equity").as_string() == "100000");
    CHECK(
      std::stod(std::string(report.at("closing_equity").as_string())) ==
      static_cast<double>(cash + position * bars.back().close_)
    );
    CHECK(report.at("first_bar").as_string() == "2020-01-01T00:00:00Z");
    CHECK(report.at("rejected").as_int64() == 0);
  }
  // A bound is checked when the strategy is built, and the replay stops.
  const auto low = backtest({at("mr.toml"), "params.lookback=1"});
  CHECK(low.exit_ == 1);
  CHECK(code_of(low.out_) == "replay.failed");
  CHECK(low.out_.find("lower bound") != std::string::npos);
  // A bound on another parameter is checked against the final values, whatever their order.
  spit(
    at("bounded.atls"),
    "params {\n  real<lower=0> entry = 2;\n  real<lower=0, upper=entry> exit = 1;\n}\nseries {\n  real z = zscore(close, "
    "5);\n}\non_bar {\n  if (z < -entry) {\n    target(10);\n  } else if (z > exit) {\n    target(0);\n  }\n}\n"
  );
  spit(at("bounded.toml"), "strategy = 'bounded.atls'\n" + data);
  CHECK(backtest({at("bounded.toml"), "params.entry=0.5", "params.exit=0.2"}).exit_ == 0);
  const auto above = backtest({at("bounded.toml"), "params.entry=0.5", "params.exit=0.9"});
  CHECK(above.exit_ == 1);
  CHECK(code_of(above.out_) == "replay.failed");
  CHECK(above.out_.find("exit = 0.9 is above its upper bound") != std::string::npos);
}

TEST_CASE(
  "strategy backtest's orders: market said or not, limits bounded per fill and refused pricing", "[acceptance]"
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
  const auto bars = walk(120);
  spit(at("bars.csv"), walk_csv(bars));
  spit(at("mr.atls"), cmdatl::TEMPLATES[0].text_);
  const auto spec = [&at](const std::string& name, const std::string& orders) {
    spit(
      at(name),
      "strategy = 'mr.atls'\n[data]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol = \"AAA\"\n[params]\nentry = 1\n"
      "lookback = 5\n" +
        orders
    );
  };
  spec("none.toml", "");
  spec("market.toml", "[orders]\ntype = \"market\"\n");
  spec("limit.toml", "[orders]\ntype = \"limit\"\noffset = 0.002\n");
  const auto none = report_of({at("none.toml")});
  // Market orders, said or not, are the same replay.
  CHECK(report_of({at("market.toml")}) == none);
  const auto limit = report_of({at("limit.toml"), "--fills", at("limit-fills.csv")});
  CHECK(limit != none);
  CHECK(limit.as_object().at("pricing_refused").as_int64() == 0);
  CHECK(none.as_object().at("pricing_refused").as_int64() == 0);
  CHECK(limit.as_object().at("pricing_refusal_reasons").as_object().empty());
  // Every limit is the decision close times 1.002 for a buy, 0.998 for a sell (whole prices on a free grid), and
  // fills at most at it for a buy, at least for a sell, on the next bar.
  const auto fills = read_fills(slurp(at("limit-fills.csv")));
  CHECK(fills.size() >= 2);
  for (const auto& [day, side, quantity, price] : fills) {
    INFO(day << " " << side << " " << price);
    REQUIRE(day >= 1);
    const auto decided = static_cast<double>(bars[static_cast<std::size_t>(day - 1)].close_);
    if (side == "buy") {
      CHECK(price <= decided * 1.002);
    } else {
      CHECK(price >= decided * 0.998);
    }
  }
  // Numbers written with an exponent reach the replay as exact decimals.
  CHECK(
    report_of({at("none.toml"), "replay.cash=1e5", "replay.commission=1.5e-4"}).as_object().at("opening_equity") ==
    "100000"
  );
  // A pricing the generated strategy refuses stops the driver (the run spec refuses it first; this is a C++ caller's
  // guard).
  const auto driver = backtest_driver(struct_of(equip->atlc_, at("mr.atls")));
  REQUIRE(!driver.empty());
  const auto direct = [&driver, &at](const std::string& type, const std::string& offset) {
    return cmdatl::run_child(
      {driver.string(), "data.source=csv", "data.file=" + at("bars.csv"), "data.symbol=AAA", "orders.type=" + type,
        "orders.offset=" + offset},
      [](const std::string&) {}
    );
  };
  const auto market_offset = direct("market", "0.001");
  CHECK(market_offset.code_ == 1);
  CHECK(cmdatl::last_line(market_offset).find("an offset prices a limit") != std::string::npos);
  const auto wide_offset = direct("limit", "0.3");
  CHECK(wide_offset.code_ == 1);
  CHECK(cmdatl::last_line(wide_offset).find("below 0.3") != std::string::npos);
  // Prices that gap up every day: each day's buy limit at its close is never reached, so each expires with its
  // session, and the last is still queued at the end.
  std::string gaps = "ts,open,high,low,close,volume\n";
  for (int day = 0; day < 4; ++day) {
    const auto price = std::to_string(100 * (day + 1));
    gaps +=
      std::to_string(1577836800 + day * 86400) + "," + price + "," + price + "," + price + "," + price + ",1000\n";
  }
  spit(at("gaps.csv"), gaps);
  spit(at("buy.atls"), "on_bar {\n  target(1);\n}\n");
  spit(at("gaps.toml"), "strategy = 'buy.atls'\n[data]\nsource = \"csv\"\nfile = \"gaps.csv\"\nsymbol = \"AAA\"\n");
  const auto expired = report_of({at("gaps.toml"), "orders.type=limit", "orders.offset=0"}).as_object();
  CHECK(expired.at("executions").as_int64() == 0);
  CHECK(expired.at("caveats").as_object().at("expired").as_int64() == 3);
  CHECK(expired.at("caveats").as_object().at("unfilled").as_int64() == 1);
  // An offset whose decimal has 19 digits: every price times it overflows, so every target is refused and counted,
  // and no order is sent at all.
  const auto refused = report_of({at("gaps.toml"), "orders.type=limit", "orders.offset=0.299999999999999999"});
  CHECK(refused.as_object().at("pricing_refused").as_int64() == 4);
  CHECK(refused.as_object().at("pricing_refusal_reasons") == boost::json::parse(R"({"price_not_representable": 4})"));
  CHECK(refused.as_object().at("caveats").as_object().at("unfilled").as_int64() == 0);
  CHECK(refused.as_object().at("executions").as_int64() == 0);
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
  REQUIRE(report_of({at("store.toml"), duckdb}) == boost::json::parse(fixture("store.json")));
  const auto driver = backtest_driver(struct_of(equip->atlc_, at("dip.atls")));
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
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "skills")) {
    if (entry.path().extension() == ".md") {
      files.push_back(entry.path());
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
