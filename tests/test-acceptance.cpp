#include "./paper-support.hpp"
#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
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
  "strategy backtest's reports and arguments for atlc's examples are the recorded ones, byte for byte", "[acceptance]"
) {
  const auto equip = equipment();
  const auto examples = cmdatl::variable("CMDATL_TEST_ATLC_EXAMPLES");
  if (!equip || !examples) {
    SKIP(std::string(UNEQUIPPED) + ", or CMDATL_TEST_ATLC_EXAMPLES names no atlc examples directory");
  }
  const Equipped equipped(*equip);
  const ScratchDir dir;
  // Recorded once from atlc 3f9ce62's `atlc run` (tests/fixtures/examples/how-made.txt).
  const auto fixtures = std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "examples";
  int compared = 0;
  for (const auto* name : {"mean-reversion", "pairs", "trailing-stop", "universe", "user-indicator"}) {
    INFO(name);
    const auto spec = (std::filesystem::path(*examples) / (std::string(name) + ".toml")).string();
    const auto expected = slurp(fixtures / (std::string(name) + ".json"));
    REQUIRE(!expected.empty());
    CHECK(report_of({spec}) == boost::json::parse(expected));
    // The driver's own bytes and arguments: a stand-in records both, then runs the driver cmdatl built.
    const auto structure =
      struct_of(equip->atlc_, (std::filesystem::path(*examples) / (std::string(name) + ".atls")).string());
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
        line.replace(at, placeholder.size(), *examples);
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
