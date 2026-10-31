#include "./support.hpp"

#include <cmdatl/cli/cli.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
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

/** @brief Moves into a directory for a test's length. */
class InDir {
public:
  /**
   * @brief Moves.
   * @param dir The directory.
   */
  explicit InDir(const std::filesystem::path& dir)
  : back_(std::filesystem::current_path()) {
    std::filesystem::current_path(dir);
  }

  InDir(const InDir&) = delete;
  InDir& operator=(const InDir&) = delete;

  /** @brief Moves back. */
  ~InDir() { std::filesystem::current_path(back_); }

private:
  std::filesystem::path back_;
};

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
    std::snprintf(row, sizeof(row), "AAA,%s,%.2f,%.2f,%.2f,%.2f,1000\n", date_of(19358 + i).c_str(), open, high, low, close);
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
 * @brief Runs `atlc run` directly and reads its report.
 * @param atlc atlc's path.
 * @param args What follows `atlc run`.
 * @return The report, or null when it printed none.
 */
boost::json::value atlc_report(const std::string& atlc, const std::vector<std::string>& args) {
  std::vector<std::string> argv{atlc, "run"};
  argv.insert(argv.end(), args.begin(), args.end());
  const auto result = cmdatl::run_child(argv, [](const std::string&) {});
  if (result.kind_ != cmdatl::ChildResult::Kind::FINISHED || result.code_ != 0) {
    return nullptr;
  }
  const auto report = cmdatl::one_object(result.stdout_);
  return report ? boost::json::value(*report) : boost::json::value(nullptr);
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

}  // namespace

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

TEST_CASE("strategy backtest's report is atlc run's for atlc's examples", "[acceptance]") {
  const auto equip = equipment();
  const auto examples = cmdatl::variable("CMDATL_TEST_ATLC_EXAMPLES");
  if (!equip || !examples) {
    SKIP(std::string(UNEQUIPPED) + ", or CMDATL_TEST_ATLC_EXAMPLES names no atlc examples directory");
  }
  const Equipped equipped(*equip);
  int compared = 0;
  for (const auto* name : {"mean-reversion", "pairs", "trailing-stop", "universe", "user-indicator"}) {
    INFO(name);
    const auto spec = (std::filesystem::path(*examples) / (std::string(name) + ".toml")).string();
    const auto ran = backtest({spec});
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    const auto expected = atlc_report(equip->atlc_, {spec});
    REQUIRE(expected.is_object());
    CHECK(reply_of(ran.out_).at("report") == expected);
    ++compared;
  }
  CHECK(compared == 5);
}

TEST_CASE("strategy backtest agrees with atlc run on limits and -P and --fills and warnings and failures", "[acceptance]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP(UNEQUIPPED);
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  const auto at = [&dir](const std::string& file) { return (dir.path() / file).string(); };
  std::filesystem::create_directories(dir.path() / "work");
  write_bars(dir.path() / "bars.csv");
  REQUIRE(run({"strategy", "new", "base", "--dir", dir.path().string()}).exit_ == 0);
  const auto spec = slurp(at("base.toml"));

  SECTION("a limit-order spec whose offset makes pricing refuse") {
    auto limit = spec;
    limit.replace(limit.find("type = \"market\""), std::string("type = \"market\"").size(), "type = \"limit\"\noffset = 0.299999999999999999");
    spit(at("limit.toml"), limit);
    const auto ran = backtest({at("limit.toml")});
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    const auto report = reply_of(ran.out_).at("report");
    CHECK(report.as_object().at("pricing_refused").as_int64() > 0);
    CHECK(report == atlc_report(equip->atlc_, {at("limit.toml")}));
  }
  SECTION("-P") {
    const auto ran = backtest({at("base.toml"), "-P", "lookback=30"});
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("params_given") == boost::json::value(boost::json::array{"lookback=30"}));
    CHECK(reply.at("report") == atlc_report(equip->atlc_, {at("base.toml"), "-P", "lookback=30"}));
    CHECK(reply.at("report") != atlc_report(equip->atlc_, {at("base.toml")}));
  }
  SECTION("--fills, relative to the working directory") {
    const InDir here(dir.path() / "work");
    const auto ran = backtest({at("base.toml"), "--fills", "fills.csv"});
    REQUIRE(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("fills").as_string() == (dir.path() / "work" / "fills.csv").string());
    CHECK(reply.at("report") == atlc_report(equip->atlc_, {at("base.toml"), "--fills", at("atlc-fills.csv")}));
    const auto fills = slurp(dir.path() / "work" / "fills.csv");
    CHECK(fills.rfind("at,symbol,side,quantity,price,commission,tax\n", 0) == 0);
    CHECK(lines_of(fills).size() > 1);
    CHECK(fills == slurp(at("atlc-fills.csv")));
  }
  SECTION("a warning reaches the reader as progress") {
    spit(at("warn.atls"), program_with_a_warning());
    auto warned = spec;
    warned.replace(warned.find("strategy = \"base.atls\""), std::string("strategy = \"base.atls\"").size(), "strategy = \"warn.atls\"");
    spit(at("warn.toml"), warned);
    const auto ran = backtest({at("warn.toml")});
    INFO(ran.out_ << "\n" << ran.err_);
    REQUIRE(ran.exit_ == 0);
    CHECK(ran.err_.find("warning[unused-series]") != std::string::npos);
    CHECK(reply_of(ran.out_).at("report") == atlc_report(equip->atlc_, {at("warn.toml")}));
  }
  SECTION("a build failure") {
    spit(at("broken.toml"), spec + "\n[build]\nalgotradelib = \"/nonexistent/prefix\"\n");
    const auto ran = backtest({at("broken.toml")});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "build.failed");
    CHECK(ran.out_.find("build.log") != std::string::npos);
  }
  SECTION("a replay failure: a parameter outside its bounds") {
    const auto ran = backtest({at("base.toml"), "-P", "lookback=1"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "replay.failed");
  }
  SECTION("a program error") {
    spit(at("bad.atls"), "params {\n  int x = ;\n}\n");
    auto bad = spec;
    bad.replace(bad.find("strategy = \"base.atls\""), std::string("strategy = \"base.atls\"").size(), "strategy = \"bad.atls\"");
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
#endif
