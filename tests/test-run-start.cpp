// `run`: the pure parts (grammars, the cache key, the build's file writes) in process, and the command as a process
// against the real atlc and algotradelib (tagged `[acceptance][runs]`: they build a driver with CMake, and skip,
// reported, without CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX). `run` replaces itself with its driver, so it is never run
// in process.

#include "./paper-support.hpp"
#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace json = boost::json;
namespace runs = cmdatl::runs;
namespace build = cmdatl::build;

TEST_CASE("run grammars: run ids, argument keys, structs and offsets") {
  CHECK(cmdatl::valid_run_id("r0123456789ab"));
  CHECK(!cmdatl::valid_run_id("r0123456789aB"));
  CHECK(!cmdatl::valid_run_id("r0123456789a"));
  CHECK(!cmdatl::valid_run_id("x0123456789ab"));
  CHECK(cmdatl::spec::valid_key("param.lookback"));
  CHECK(cmdatl::spec::valid_key(std::string(128, 'a')));
  CHECK(!cmdatl::spec::valid_key(std::string(129, 'a')));
  CHECK(!cmdatl::spec::valid_key("a b"));
  CHECK(!cmdatl::spec::valid_key(""));
  CHECK(cmdatl::spec::valid_struct("MeanReversion"));
  CHECK(cmdatl::spec::valid_struct("M" + std::string(63, 'a')));
  CHECK(!cmdatl::spec::valid_struct("M" + std::string(64, 'a')));
  CHECK(!cmdatl::spec::valid_struct("Params"));
  CHECK(!cmdatl::spec::valid_struct("Lane"));
  CHECK(!cmdatl::spec::valid_struct("meanReversion"));
  CHECK(!cmdatl::spec::valid_struct("Mean_Reversion"));
  CHECK(runs::valid_offset("0.01"));
  CHECK(runs::valid_offset("0"));
  CHECK(!runs::valid_offset("1"));
  CHECK(!runs::valid_offset("1.5"));
  CHECK(!runs::valid_offset("-0.1"));
  CHECK(!runs::valid_offset("0.01x"));
}

TEST_CASE("the market settings a run refuses before anything is built") {
  CHECK(!cmdatl::spec_refusal({"replay.market=kr"}));
  CHECK(cmdatl::spec_refusal({})->first == "replay.market");
  CHECK(cmdatl::spec_refusal({"replay.market=us"})->first == "replay.market");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "bars.store=kis/krx"})->first == "bars.store");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "replay.shorts=true"})->first == "replay.shorts");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "bars.open_point=true"})->first == "bars.open_point");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "replay.fill=decision-close"})->first == "replay.fill");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "replay.fill=next-close"})->first == "replay.fill");
  CHECK(!cmdatl::spec_refusal({"replay.market=kr", "replay.fill=next-open"}));
  // The last value wins, as atlc's settings read it.
  CHECK(!cmdatl::spec_refusal({"replay.market=us", "replay.market=kr"}));
}

TEST_CASE("a driver's program header gives the fill model the run's market-price rule, or leaves it") {
  build::Request request;
  request.struct_ = "Dip";
  request.kind_ = build::DriverKind::BACKTEST;
  const auto plain = build::program_header(request);
  CHECK(plain.find("using Program = atls::Dip;") != std::string::npos);
  CHECK(plain.find("  return model;\n") != std::string::npos);
  CHECK(plain.find("with_market_price") == std::string::npos);
  request.fill_header_ = "/specs/rules/close.hpp";
  request.fill_function_ = "rules::decision_close";
  const auto ruled = build::program_header(request);
  CHECK(ruled.find("#include \"/specs/rules/close.hpp\"\n") != std::string::npos);
  CHECK(
    ruled.find("  return model.with_market_price(\"rules::decision_close\", rules::decision_close);\n") !=
    std::string::npos
  );
}

TEST_CASE("a broker folder's driver header names the folder it is built from, every byte escaped") {
  build::Request request;
  request.struct_ = "Dip";
  CHECK(build::program_header(request).find("CMDATL_ADAPTER_FOLDER") == std::string::npos);
  // A folder whose path holds a quote, a backslash, a space, a line break, a digit after an escape and UTF-8.
  const std::string folder = "/p/a \"b\\c\n7/\xed\x8f\xb4\xeb\x8d\x94";
  request.adapter_ = cmdatl::auth::AdapterSnapshot{"vendor", folder, {}};
  const auto header = build::program_header(request);
  const std::string define = "#define CMDATL_ADAPTER_FOLDER \"";
  const auto at = header.find(define);
  REQUIRE(at != std::string::npos);
  const auto begin = at + define.size();
  const auto end = header.find('"', begin);
  REQUIRE(end != std::string::npos);
  // The literal is only three-digit octal escapes, which a C++ compiler reads back byte for byte.
  const auto literal = header.substr(begin, end - begin);
  REQUIRE(literal.size() == 4 * folder.size());
  std::string decoded;
  for (std::size_t k = 0; k < literal.size(); k += 4) {
    REQUIRE(literal[k] == '\\');
    for (std::size_t d = 1; d <= 3; ++d) {
      REQUIRE((literal[k + d] >= '0' && literal[k + d] <= '7'));
    }
    decoded.push_back(static_cast<char>(std::stoi(literal.substr(k + 1, 3), nullptr, 8)));
  }
  CHECK(decoded == folder);
}

TEST_CASE("a shape's key changes with each input it holds, and with nothing else") {
  std::map<std::string, std::string> env;
  const auto lookup = [&env](const std::string& name) -> std::optional<std::string> {
    const auto found = env.find(name);
    return found == env.end() ? std::nullopt : std::optional<std::string>(found->second);
  };
  const auto base = build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg");
  CHECK(base.rfind("A-", 0) == 0);
  CHECK(base.size() == 2 + 16);
  CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") == base);
  CHECK(build::shape_name("/p/b.atls", "A", "incremental", lookup, "/pkg") != base);
  CHECK(build::shape_name("/p/a.atls", "A", "exact", lookup, "/pkg") != base);
  CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/other") != base);
  for (const auto& name : build::KEY_VARIABLES) {
    env = {{name, "x"}};
    CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") != base);
    // Set and empty differ from unset.
    env = {{name, ""}};
    CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") != base);
  }
  env = {{"CMAKE_PREFIX_PATH", "/x"}, {"PATH", "/y"}};
  CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") == base);
}

TEST_CASE("a build's children run without the locators the key does not hold") {
  const auto out = build::child_environment(
    {"PATH=/bin", "CMAKE_PREFIX_PATH=/x", "algotradelib_DIR=/y", "ALGOTRADELIB_ROOT=/z", "OPENSSL_ROOT_DIR=/o",
      "BOOST_ROOT=/b", "Foo_DIR=/f", "HOME=/h", "CXX=clang++"}
  );
  CHECK(out == std::vector<std::string>{"PATH=/bin", "OPENSSL_ROOT_DIR=/o", "BOOST_ROOT=/b", "HOME=/h", "CXX=clang++"});
}

TEST_CASE("the build writes a file only when its bytes change, and clears a killed build's temporaries") {
  const ScratchDir dir;
  const auto file = dir.path() / "src" / "deep" / "a.hpp";
  CHECK(build::write_if_changed(file, "one") == build::Written::REPLACED);
  struct stat before{};
  REQUIRE(::stat(file.c_str(), &before) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(build::write_if_changed(file, "one") == build::Written::UNCHANGED);
  struct stat same{};
  REQUIRE(::stat(file.c_str(), &same) == 0);
  CHECK(same.st_ino == before.st_ino);
  CHECK(same.st_mtime == before.st_mtime);
  CHECK(build::write_if_changed(file, "two") == build::Written::REPLACED);
  CHECK(slurp(file) == "two");
  spit(dir.path() / "src" / ".a.hpp.123.00ff.tmp", "half");
  spit(dir.path() / "src" / "keep.tmp", "not ours");
  build::remove_stale_temporaries(dir.path() / "src");
  CHECK(!std::filesystem::exists(dir.path() / "src" / ".a.hpp.123.00ff.tmp"));
  CHECK(std::filesystem::exists(dir.path() / "src" / "keep.tmp"));
  CHECK(build::files_under(dir.path() / "src") == std::set<std::string>{"deep/a.hpp", "keep.tmp"});
}

TEST_CASE("the driver's sources are carried inside cmdatl, and only the modules it may include") {
  std::set<std::string> paths;
  for (const auto& file : cmdatl::DRIVER_FILES) {
    paths.insert(file.path_);
    std::size_t size = 0;
    for (std::size_t k = 0; k < file.count_; ++k) {
      CHECK(file.sizes_[k] <= 16u * 1024u);
      size += file.sizes_[k];
    }
    CHECK((size > 0 || std::string(file.path_).find("LICENSE") != std::string::npos));
  }
  CHECK(paths.count("main.cpp") == 1);
  CHECK(paths.count("CMakeLists.txt") == 1);
  CHECK(paths.count("cmdatl/driver/run.hpp") == 1);
  // The replay module, which both drivers include; nothing of atlc's runtime is carried.
  CHECK(paths.count("cmdatl/replay/replay.hpp") == 1);
  CHECK(paths.count("cmdatl/replay/settings.hpp") == 1);
  CHECK(paths.count("third_party/toml++/toml.hpp") == 1);
  for (const auto& path : paths) {
    CHECK(path.find("cmdatl/cli/") == std::string::npos);
    CHECK(path.find("cmdatl/strategy/") == std::string::npos);
    CHECK(path.find("atls/") == std::string::npos);
  }
  // A file's bytes come back whole.
  for (const auto& file : cmdatl::DRIVER_FILES) {
    if (std::string(file.path_) == "cmdatl/core/runs.hpp") {
      std::string text;
      for (std::size_t k = 0; k < file.count_; ++k) {
        text.append(file.chunks_[k], file.sizes_[k]);
      }
      CHECK(text == slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "include" / "cmdatl" / "core" / "runs.hpp"));
    }
  }
}

TEST_CASE("the table's run command: arguments, codes and the schema version") {
  const cmdatl::Table table(cmdatl::command_list());
  const auto* run = table.resolve({"run"}).command_;
  REQUIRE(run != nullptr);
  CHECK(run->options_.empty());
  REQUIRE(run->args_.size() == 2);
  CHECK(run->args_[0].name_ == "TARGET");
  CHECK(run->args_[0].required_);
  CHECK(!run->args_[0].variadic_);
  CHECK(run->args_[1].name_ == "SETTING");
  CHECK(!run->args_[1].required_);
  CHECK(run->args_[1].variadic_);
  CHECK(cmdatl::usage(*run).rfind("TARGET [SETTING...]", 0) == 0);
  CHECK(run->may_send_);
  CHECK(static_cast<bool>(run->run_config_));
  // The old subcommands are gone: each word is now run's TARGET.
  for (const auto* gone : {"start", "status", "log", "stop"}) {
    const auto* found = table.resolve({"run", gone}).command_;
    REQUIRE(found != nullptr);
    CHECK(found->path_ == std::vector<std::string>{"run"});
  }
  std::set<std::string> catalog;
  for (const auto& entry : cmdatl::error_catalog()) {
    catalog.insert(entry.code_);
  }
  for (const auto& code : cmdatl::RUN_CODES) {
    INFO(code);
    CHECK(catalog.count(code) == 1);
    CHECK(std::find(run->errors_.begin(), run->errors_.end(), code) != run->errors_.end());
  }
  CHECK(cmdatl::check_table(table).empty());
  CHECK(cmdatl::SCHEMA_VERSION == 15);
}

// --- Processes, against the real atlc and algotradelib ---

namespace {

/** @brief The real atlc and the library prefix, or nothing. */
std::optional<std::pair<std::string, std::string>> run_equipment() {
  const auto atlc = cmdatl::variable("CMDATL_TEST_ATLC");
  const auto prefix = cmdatl::variable("CMDATL_TEST_PREFIX");
  if (!atlc || !prefix) {
    return std::nullopt;
  }
  return std::make_pair(*atlc, *prefix);
}

/**
 * @brief Daily bars of 005930, date-only rows (atlc stamps them at 00:00 UTC): a walk with a sharp dip every 23 days,
 *   so the mean-reversion starter trades.
 * @param days How many weekdays from 2024-01-02.
 * @param extra_symbols Other symbols in the file, each with the same bars, to make the file larger.
 * @return The CSV.
 */
std::string dip_bars(int days, int extra_symbols = 0) {
  std::string out = "symbol,date,open,high,low,close,volume\n";
  long day = 19724; // 2024-01-02
  long close = 78000;
  std::vector<std::string> rows;
  for (int k = 0, made = 0; made < days; ++day) {
    const long weekday = (day + 3) % 7;
    if (weekday >= 5) {
      continue;
    }
    const long open = close;
    long step = ((k * 37) % 7 - 3) * 100;
    if (k % 23 == 20) {
      step = -5000;
    }
    if (k % 23 == 21 || k % 23 == 22) {
      step = 2500;
    }
    close = std::max(1000L, open + step);
    long y = 0;
    long m = 0;
    long d = 0;
    {
      long z = day + 719468;
      const long era = z / 146097;
      const long doe = z - era * 146097;
      const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const long mp = (5 * doy + 2) / 153;
      d = doy - (153 * mp + 2) / 5 + 1;
      m = mp < 10 ? mp + 3 : mp - 9;
      y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    }
    char date[64];
    std::snprintf(date, sizeof date, "%04ld-%02ld-%02ld", y, m, d);
    char row[160];
    std::snprintf(
      row, sizeof row, ",%s,%ld,%ld,%ld,%ld,100000\n", date, open, std::max(open, close) + 300,
      std::min(open, close) - 300, close
    );
    out += std::string("005930") + row;
    for (int s = 0; s < extra_symbols; ++s) {
      char symbol[16];
      std::snprintf(symbol, sizeof symbol, "9%05d", s);
      out += symbol + std::string(row);
    }
    ++k;
    ++made;
  }
  return out;
}

/** @brief The program, the specs and the build cache every process case shares, so the driver is built once. */
struct Shared {
  /** @brief Where they live. */
  ScratchDir dir_;

  Shared() {
    spit(
      dir_.path() / "dip.atls", slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "templates" / "mean-reversion.atls")
    );
    spit(dir_.path() / "bars.csv", dip_bars(160));
    spit(dir_.path() / "long.csv", dip_bars(500));
    spit(dir_.path() / "wide.csv", dip_bars(400, 600));
    for (
      const auto& [name, data] :
      std::vector<std::pair<std::string, std::string>>{{"dip", "bars.csv"}, {"long", "long.csv"}, {"wide", "wide.csv"}}
    ) {
      spit(
        dir_.path() / (name + ".toml"),
        "strategy = \"dip.atls\"\n\n[bars]\nsource = \"csv\"\nfile = \"" + data +
          "\"\nsymbol_column = \"symbol\"\ntime = \"date\"\ntime_format = \"iso\"\n\n[instruments]\nstock = \"005930\"\n\n"
          "[replay]\ncash = 100000000\ncommission = 0.0005\nmarket = \"kr\"\n\n[orders]\ntype = \"limit\"\noffset = 0.01\n"
      );
    }
    std::filesystem::create_directories(dir_.path() / "cache");
  }

  /**
   * @brief A spec's path.
   * @param name `dip`, `long` or `wide`.
   * @return The path.
   */
  std::string spec(const std::string& name) const {
    return (dir_.path() / (name + ".toml")).string();
  }
};

/**
 * @brief The shared fixture, made once per test process.
 * @return It.
 */
Shared& shared() {
  static Shared s;
  return s;
}

/** @brief The operator's file for the process cases: 005930 allowed with room for every order of the dip data. */
ConfigParts run_operator() {
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"005930\"]\n"
    "position = { \"005930\" = 100000 }\n"
    "order_value = 100000000000\n"
    "daily_orders = 1000\n";
  return parts;
}

/** @brief Points cmdatl at the real atlc, the build at the library prefix and the shared cache, for a case's length. */
class RunEquipped {
public:
  /**
   * @brief Sets the variables.
   * @param atlc atlc's path.
   * @param prefix The library prefix.
   */
  RunEquipped(const std::string& atlc, const std::string& prefix)
  : atlc_("CMDATL_ATLC", atlc),
    prefix_("CMAKE_PREFIX_PATH", prefix),
    cache_("XDG_CACHE_HOME", (shared().dir_.path() / "cache").string()) {}

private:
  ScopedVariable atlc_;
  ScopedVariable prefix_;
  ScopedVariable cache_;
};

/** @brief What a `run` process wrote and how it ended. */
struct RunProcess {
  /** @brief Its exit. */
  int exit_{-1};
  /** @brief Its pid. */
  pid_t pid_{0};
  /** @brief The events, in order. */
  std::vector<json::object> events_;
  /** @brief The last line: the run's reply, or cmdatl's own refusal. */
  json::object reply_;
  /** @brief stdout, whole. */
  std::string out_;
  /** @brief stderr. */
  std::string err_;
};

/**
 * @brief Runs `cmdatl --json run ARGS...` as a process to its end.
 * @param args The arguments after `run`.
 * @param seconds How long it may take.
 * @return What it wrote.
 */
RunProcess run_process(const std::vector<std::string>& args, int seconds = 600) {
  std::vector<std::string> line{"--json", "run"};
  line.insert(line.end(), args.begin(), args.end());
  Spawned cmdatl(line);
  RunProcess ran;
  ran.pid_ = cmdatl.pid();
  const int status = cmdatl.wait(seconds);
  ran.out_ = cmdatl.out();
  ran.err_ = cmdatl.err();
  INFO(ran.out_ << ran.err_);
  REQUIRE(WIFEXITED(status));
  ran.exit_ = WEXITSTATUS(status);
  const auto lines = lines_of(ran.out_);
  REQUIRE(!lines.empty());
  for (std::size_t k = 0; k + 1 < lines.size(); ++k) {
    auto event = json::parse(lines[k]).as_object();
    REQUIRE(event.contains("event"));
    ran.events_.push_back(std::move(event));
  }
  ran.reply_ = json::parse(lines.back()).as_object();
  REQUIRE(!ran.reply_.contains("event"));
  return ran;
}

/**
 * @brief A run's error code, or empty.
 * @param ran The run.
 * @return The code.
 */
std::string run_code(const RunProcess& ran) {
  const auto* error = ran.reply_.if_contains("error");
  return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
}

/**
 * @brief A run's fills from its events, as `date,side,quantity,price`.
 * @param ran The run.
 * @return The fills.
 */
std::vector<std::string> run_fills(const RunProcess& ran) {
  std::vector<std::string> out;
  for (const auto& event : ran.events_) {
    if (event.at("event").as_string() == "filled") {
      out.push_back(
        std::string(event.at("at").as_string()).substr(0, 10) + "," + std::string(event.at("side").as_string()) + "," +
        std::string(event.at("quantity").as_string()) + "," + std::string(event.at("price").as_string())
      );
    }
  }
  return out;
}

/**
 * @brief A backtest's fills from atlc's fills file, as `date,side,quantity,price`.
 * @param path The fills file.
 * @return The fills.
 */
std::vector<std::string> backtest_fills(const std::filesystem::path& path) {
  std::vector<std::string> out;
  const auto lines = lines_of(slurp(path));
  for (std::size_t k = 1; k < lines.size(); ++k) {
    std::vector<std::string> cells;
    std::size_t start = 0;
    while (true) {
      const auto comma = lines[k].find(',', start);
      cells.push_back(lines[k].substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    REQUIRE(cells.size() >= 5);
    out.push_back(cells[0].substr(0, 10) + "," + cells[2] + "," + cells[3] + "," + cells[4]);
  }
  return out;
}

} // namespace

TEST_CASE(
  "run trades as strategy backtest does on the same spec, fill for fill, as the process it was started as",
  "[acceptance][runs]"
) {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  const auto fills = shared().dir_.path() / "fills.csv";
  const auto backtest = fixture.json({"strategy", "backtest", shared().spec("dip"), "--fills", fills.string()});
  REQUIRE(backtest.exit_ == 0);
  const auto expected = backtest_fills(fills);
  // The conditions that make equality expected, asserted before comparing: bars that trade, every limit inside the
  // collar and the operator's caps (the spec's offset is 1 %, the collar 5 %, the cap 100,000 shares), enough cash.
  REQUIRE(expected.size() >= 6);
  const auto ran = run_process({shared().spec("dip")});
  INFO(ran.out_);
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  CHECK(ran.reply_.at("result").as_string() == "ended");
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("executions").as_int64() == static_cast<std::int64_t>(expected.size()));
  CHECK(run_fills(ran) == expected);
  CHECK(
    reply_of(backtest.out_).at("report").as_object().at("executions").as_int64() ==
    static_cast<std::int64_t>(expected.size())
  );
  // cmdatl became the driver: the start line's pid is the process the caller started.
  REQUIRE(!ran.events_.empty());
  CHECK(ran.events_.front().at("event").as_string() == "start");
  CHECK(ran.events_.front().at("pid").as_int64() == static_cast<std::int64_t>(ran.pid_));
  CHECK(ran.events_.back().at("event").as_string() == "end");
  // The cache: a second run of the same program builds nothing.
  const auto driver = shared().dir_.path() / "cache" / "cmdatl" / "drivers";
  std::optional<std::filesystem::file_time_type> before;
  std::filesystem::path shape;
  for (const auto& entry : std::filesystem::directory_iterator(driver)) {
    // The run's driver, not the backtest's (both build Dip, in shapes of their own).
    if (
      entry.path().filename().string().rfind("Dip-", 0) == 0 &&
      slurp(entry.path() / "src" / "program.hpp").find("cmdatl run") != std::string::npos
    ) {
      shape = entry.path();
      before = std::filesystem::last_write_time(shape / "build" / "driver");
    }
  }
  REQUIRE(before);
  const auto again = run_process({shared().spec("dip"), "params.lookback=21"});
  REQUIRE(again.exit_ == 0);
  CHECK((std::filesystem::last_write_time(shape / "build" / "driver") == *before));
  // The driver's include closure, from the compiler's own dependency files: no CLI11, nothing of cmdatl's cli/ or strategy/ (the layout check's driver row, seen in the real build).
  // Only `<source>.o.d`: on Linux the linker also writes `link.d`, which lists libraries.
  std::size_t read = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(shape / "build")) {
    if (entry.is_regular_file() && entry.path().extension() == ".d" && entry.path().stem().extension() == ".o") {
      const auto deps = slurp(entry.path());
      ++read;
      CHECK(deps.find("/CLI/") == std::string::npos);
      CHECK(deps.find("cmdatl/cli/") == std::string::npos);
      CHECK(deps.find("cmdatl/strategy/") == std::string::npos);
      CHECK(deps.find("cmdatl/driver/run.hpp") != std::string::npos);
      CHECK(deps.find("cmdatl/replay/replay.hpp") != std::string::npos);
      CHECK(deps.find("atls/run.hpp") == std::string::npos);
      CHECK(deps.find("atls/args.hpp") == std::string::npos);
    }
  }
  CHECK(read >= 1);
}

TEST_CASE(
  "run takes a program with a run spec that names none, and keeps the operator's limits", "[acceptance][runs]"
) {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  // dip.toml without its `strategy` line: a spec any single-instrument program could share.
  const auto dip = slurp(shared().spec("dip"));
  const auto settings = dip.substr(dip.find('\n') + 1);
  const auto spec = (shared().dir_.path() / "shared.toml").string();
  spit(spec, settings);
  const auto program = (shared().dir_.path() / "dip.atls").string();
  const auto backtest = fixture.json({"strategy", "backtest", program, spec});
  INFO(backtest.out_);
  REQUIRE(backtest.exit_ == 0);
  CHECK(reply_of(backtest.out_).at("spec").as_string() == spec);
  CHECK(reply_of(backtest.out_).at("program").as_string() == program);
  const auto ran = run_process({spec, program});
  INFO(ran.out_);
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  CHECK(ran.reply_.at("executions") == reply_of(backtest.out_).at("report").as_object().at("executions"));
  CHECK(ran.err_.find("cmdatl: starting a run of " + program + " with " + spec) != std::string::npos);
  // A shared spec's [instruments] and [orders] meet the operator's limits as a spec naming its program does.
  const auto replaced = [&settings](const std::string& from, const std::string& to) {
    auto text = settings;
    text.replace(text.find(from), from.size(), to);
    return text;
  };
  const auto other = (shared().dir_.path() / "other-symbol.toml").string();
  spit(other, replaced("stock = \"005930\"", "stock = \"000660\""));
  const auto symbol = run_process({program, other});
  CHECK(symbol.exit_ == 3);
  CHECK(run_code(symbol) == "refused.symbol_not_allowed");
  const auto market = (shared().dir_.path() / "market-orders.toml").string();
  spit(market, replaced("type = \"limit\"\noffset = 0.01\n", "type = \"market\"\n"));
  const auto orders = run_process({program, market});
  CHECK(orders.exit_ == 3);
  CHECK(run_code(orders) == "refused.order_type");
}

TEST_CASE("two runs started at once on one account build the program once and trade apart", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  // A cache of this case's own, so both meet an unbuilt shape and one waits on the other's lock.
  const ScratchDir cache;
  const ScopedVariable own_cache("XDG_CACHE_HOME", cache.path().string());
  fixture.arm();
  Spawned first({"--json", "run", shared().spec("dip")});
  Spawned second({"--json", "run", shared().spec("dip")});
  const int a = first.wait(600);
  const int b = second.wait(600);
  INFO(first.out() << first.err());
  INFO(second.out() << second.err());
  REQUIRE(WIFEXITED(a));
  REQUIRE(WIFEXITED(b));
  CHECK(WEXITSTATUS(a) == 0);
  CHECK(WEXITSTATUS(b) == 0);
  const auto one = json::parse(lines_of(first.out()).back()).as_object();
  const auto two = json::parse(lines_of(second.out()).back()).as_object();
  CHECK(one.at("run") != two.at("run"));
  CHECK(one.at("executions") == two.at("executions"));
  CHECK(one.at("closing_equity") == two.at("closing_equity"));
  std::size_t shapes = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cache.path() / "cmdatl" / "drivers")) {
    shapes += entry.is_directory() ? 1 : 0;
  }
  CHECK(shapes == 1);
}

TEST_CASE("run gives its own refusals before anything is built, and the driver's after", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  {
    // The default limits allow 005930; the armed file is not there.
    const PaperFixture fixture;
    const RunEquipped equipped(equipment->first, equipment->second);
    const auto unarmed = run_process({shared().spec("dip")});
    CHECK(unarmed.exit_ == 3);
    CHECK(run_code(unarmed) == "refused.not_armed");
    CHECK(!unarmed.reply_.at("error").as_object().at("retry").as_bool());
    const auto runs_dir = fixture.dir() / "state" / "cmdatl" / "runs" / "mock-mock-paper-0001";
    CHECK((!std::filesystem::exists(runs_dir) || std::filesystem::is_empty(runs_dir)));
  }
  {
    auto parts = run_operator();
    parts.limits_ = "symbols = [\"000660\"]\nposition = { \"000660\" = 10 }\norder_value = 1000000\ndaily_orders = 3\n";
    const PaperFixture fixture(parts);
    const RunEquipped equipped(equipment->first, equipment->second);
    fixture.arm();
    const auto other = run_process({shared().spec("dip")});
    CHECK(other.exit_ == 3);
    CHECK(run_code(other) == "refused.symbol_not_allowed");
  }
  {
    // A program with session handlers: run gives the driver's own refusal before anything is built, whatever the
    // settings say, from a spec, with a setting, or with the program as the target. The account is not armed, so the refusal
    // is shown to come before the operator's checks; the case's own cache shows that nothing was built.
    const PaperFixture fixture(run_operator());
    const RunEquipped equipped(equipment->first, equipment->second);
    const ScratchDir cache;
    const ScopedVariable own_cache("XDG_CACHE_HOME", cache.path().string());
    spit(fixture.dir() / "legs.atls", "universe {}\non_open {\n  target(10);\n}\non_close {\n  target(0);\n}\n");
    spit(fixture.dir() / "bars.csv", dip_bars(60));
    const std::string spec =
      "strategy = \"legs.atls\"\n\n[bars]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol_column = \"symbol\"\n"
      "time = \"date\"\ntime_format = \"iso\"\n\n[instruments]\nstock = \"005930\"\n\n[replay]\nmarket = \"kr\"\n";
    const std::string orders = "\n[orders]\ntype = \"limit\"\noffset = 0.01\n";
    const auto legs_refused = [](const RunProcess& ran) {
      INFO(ran.out_);
      CHECK(ran.exit_ == 5);
      CHECK(run_code(ran) == "program.live_unsupported");
      CHECK(ran.reply_.at("unsupported").as_string() == "session");
      CHECK(ran.reply_.at("error").as_object().at("message").as_string() == cmdatl::SESSION_UNSUPPORTED);
      CHECK(ran.reply_.at("error").as_object().at("next").as_string() == cmdatl::SESSION_UNSUPPORTED_NEXT);
    };
    spit(fixture.dir() / "legs.toml", spec + orders);
    legs_refused(run_process({(fixture.dir() / "legs.toml").string()}));
    legs_refused(run_process({(fixture.dir() / "legs.toml").string(), "bars.open_point=true"}));
    legs_refused(run_process({(fixture.dir() / "legs.atls").string(), "replay.market=kr"}));
    const auto runs_dir = fixture.dir() / "state" / "cmdatl" / "runs" / "mock-mock-paper-0001";
    CHECK((!std::filesystem::exists(runs_dir) || std::filesystem::is_empty(runs_dir)));
    CHECK(!std::filesystem::exists(cache.path() / "cmdatl" / "drivers"));
    // A market-price rule prices a backtest's fills; a run's orders fill at the market.
    spit(fixture.dir() / "plain.atls", "on_bar {\n  target(10);\n}\n");
    spit(fixture.dir() / "rule.hpp", "#pragma once\n");
    spit(
      fixture.dir() / "plain.toml",
      "strategy = \"plain.atls\"\n" + spec.substr(spec.find('\n')) +
        "fill_header = \"rule.hpp\"\nfill_function = \"decision_close\"\n" + orders
    );
    const auto plain = run_process({(fixture.dir() / "plain.toml").string()});
    INFO(plain.out_);
    CHECK(plain.exit_ == 2);
    CHECK(run_code(plain) == "spec.invalid");
    const auto plain_message = std::string(plain.reply_.at("error").as_object().at("message").as_string());
    // Refused at the spec (a rule with limit orders) or by the run (a rule at all): either names the rule.
    CHECK(plain_message.find("rule") != std::string::npos);
    // So is the library's rule, named on the command line: refused by the check the run's driver repeats.
    spit(fixture.dir() / "preset.toml", "strategy = \"plain.atls\"\n" + spec.substr(spec.find('\n')));
    const auto preset = run_process({(fixture.dir() / "preset.toml").string(), "replay.fill=decision-close"});
    INFO(preset.out_);
    CHECK(preset.exit_ == 2);
    CHECK(run_code(preset) == "spec.invalid");
    CHECK(preset.reply_.at("key").as_string() == "replay.fill");
  }
  {
    // A program with on_fill: the build succeeds and the driver refuses it, exit 5.
    const PaperFixture fixture(run_operator());
    const RunEquipped equipped(equipment->first, equipment->second);
    fixture.arm();
    spit(
      fixture.dir() / "trail.atls", slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "examples" / "trailing-stop.atls")
    );
    spit(fixture.dir() / "bars.csv", dip_bars(60));
    spit(
      fixture.dir() / "trail.toml",
      "strategy = \"trail.atls\"\n\n[bars]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol_column = \"symbol\"\ntime = \"date\"\n"
      "time_format = \"iso\"\n\n[instruments]\nstock = \"005930\"\n\n[replay]\nmarket = \"kr\"\n\n[orders]\ntype = \"limit\"\noffset = 0.01\n"
    );
    const auto ran = run_process({(fixture.dir() / "trail.toml").string()});
    INFO(ran.out_);
    INFO(ran.err_);
    CHECK(ran.exit_ == 5);
    CHECK(run_code(ran) == "program.live_unsupported");
    CHECK(ran.reply_.at("result").as_string() == "failed");
  }
}

TEST_CASE(
  "cmdatl run becomes its driver: SIGINT withdraws a resting order, and a SIGHUP ignored when it started stays ignored",
  "[acceptance][runs]"
) {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  // A driver with the test barrier, held at each step while an order rests (its own shape: CXXFLAGS is in the key).
  const ScopedVariable hooks("CXXFLAGS", "-DCMDATL_RUN_TEST_HOOKS");
  const auto barrier = fixture.dir() / "barrier";
  const ScopedVariable held("CMDATL_TEST_BARRIER", barrier.string());
  const ScopedVariable on_order("CMDATL_TEST_BARRIER_ON_ORDER", "1");
  // Waits for the first resting order; a build can take minutes.
  const auto until_placed = [](Spawned& cmdatl) {
    while (true) {
      const auto line = cmdatl.line(false, 600);
      REQUIRE(line);
      if (line->find("\"event\":\"placed\"") != std::string::npos) {
        return;
      }
    }
  };
  {
    spit(barrier, "");
    Spawned cmdatl({"--json", "run", shared().spec("dip")});
    until_placed(cmdatl);
    cmdatl.signal(SIGINT);
    std::filesystem::remove(barrier);
    const int status = cmdatl.wait(120);
    INFO(cmdatl.out() << cmdatl.err());
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    // Nothing the run started keeps its output open.
    CHECK(cmdatl.stdout_ended());
    const auto reply = json::parse(lines_of(cmdatl.out()).back()).as_object();
    CHECK(reply.at("stopped").as_string() == "signal");
    CHECK(reply.at("outcome").as_string() == "clean");
    CHECK(reply.at("owed_at_end").as_array().empty());
    CHECK(cmdatl.out().find("\"event\":\"canceled\"") != std::string::npos);
  }
  {
    // As under nohup: SIGHUP ignored when cmdatl starts. The run takes no notice and ends at the end of its data.
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    struct sigaction before{};
    REQUIRE(::sigaction(SIGHUP, &ignored, &before) == 0);
    spit(barrier, "");
    Spawned cmdatl({"--json", "run", shared().spec("dip")});
    ::sigaction(SIGHUP, &before, nullptr);
    until_placed(cmdatl);
    cmdatl.signal(SIGHUP);
    std::filesystem::remove(barrier);
    const int status = cmdatl.wait(120);
    INFO(cmdatl.out() << cmdatl.err());
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    const auto reply = json::parse(lines_of(cmdatl.out()).back()).as_object();
    CHECK(reply.at("stopped").as_string() == "end");
    CHECK(reply.at("executions").as_int64() > 0);
  }
}

TEST_CASE(
  "a driver is built against the package it names and no other, and a missing package is refused", "[acceptance][runs]"
) {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const ScratchDir work;
  // Two installations of the library, each a whole prefix, and a toolchain file that makes the second reachable: a
  // build that fell back to any package CMake could find would succeed against it.
  const auto a = work.path() / "a";
  const auto b = work.path() / "b";
  std::filesystem::copy(equipment->second, a, std::filesystem::copy_options::recursive);
  std::filesystem::copy(equipment->second, b, std::filesystem::copy_options::recursive);
  const auto package_a = (a / "lib" / "cmake" / "algotradelib").string();
  const auto package_b = (b / "lib" / "cmake" / "algotradelib").string();
  spit(work.path() / "toolchain.cmake", "list(APPEND CMAKE_PREFIX_PATH \"" + b.string() + "\")\n");
  const ScopedVariable cache("XDG_CACHE_HOME", (work.path() / "cache").string());
  const ScopedVariable toolchain("CMAKE_TOOLCHAIN_FILE", (work.path() / "toolchain.cmake").string());
  const auto program = (shared().dir_.path() / "dip.atls").string();
  const auto quiet = [](const std::string&) {};
  const auto request = [&](const std::string& package, const std::string& path = "") {
    return build::Request{equipment->first, "test", path.empty() ? program : path, "Dip", "incremental", package};
  };
  const auto used_package = [](const std::filesystem::path& shape) {
    std::string text;
    REQUIRE(build::read_file(shape / "build" / "CMakeCache.txt", text));
    return build::real_path(build::cache_value(text, "algotradelib_DIR").value_or(""));
  };
  for (const auto* name : {"run-a", "run-b", "run-gone", "run-fresh"}) {
    std::filesystem::create_directories(work.path() / name);
  }
  // Two packages sharing one cache: two shapes, each built against its own.
  REQUIRE(!build::build_driver(request(package_a), work.path() / "run-a", quiet));
  REQUIRE(!build::build_driver(request(package_b), work.path() / "run-b", quiet));
  CHECK(std::filesystem::exists(work.path() / "run-a" / "driver"));
  CHECK(std::filesystem::exists(work.path() / "run-b" / "driver"));
  std::map<std::string, std::filesystem::path> shapes;
  for (const auto& entry : std::filesystem::directory_iterator(work.path() / "cache" / "cmdatl" / "drivers")) {
    shapes[used_package(entry.path())] = entry.path();
  }
  REQUIRE(shapes.size() == 2);
  CHECK(shapes.count(build::real_path(package_a)) == 1);
  CHECK(shapes.count(build::real_path(package_b)) == 1);
  // The first package removed, the second still reachable: refused before anything is built, copied or run, with an
  // old build of the first's shape in the cache, and for a program never built.
  std::filesystem::rename(a, work.path() / "a-gone");
  const auto gone = build::build_driver(request(package_a), work.path() / "run-gone", quiet);
  REQUIRE(gone);
  CHECK(gone->exit_ == cmdatl::Exit::FAILED);
  CHECK(std::string(gone->body_.at("error").as_object().at("code").as_string()) == "build.failed");
  CHECK(!std::filesystem::exists(work.path() / "run-gone" / "driver"));
  spit(work.path() / "other.atls", slurp(program));
  const auto fresh =
    build::build_driver(request(package_a, (work.path() / "other.atls").string()), work.path() / "run-fresh", quiet);
  REQUIRE(fresh);
  CHECK(!std::filesystem::exists(work.path() / "run-fresh" / "driver"));
  // CMake's own check, below cmdatl's: the driver's project configured for the missing package fails though the
  // toolchain file makes another one findable.
  const auto cmake = cmdatl::find_on_path("cmake");
  REQUIRE(cmake);
  const auto configured = cmdatl::run_child(
    {*cmake, "-S", (shapes.at(build::real_path(package_b)) / "src").string(), "-B",
      (work.path() / "probe-build").string(), "-DCMAKE_BUILD_TYPE=Release", "-DCMDATL_ALGOTRADELIB_DIR=" + package_a},
    quiet
  );
  CHECK(configured.kind_ == cmdatl::ChildResult::Kind::FINISHED);
  CHECK(configured.code_ != 0);
}

TEST_CASE(
  "the strategy guide's session runs as written from a starter and its TOML block to a run", "[acceptance][runs]"
) {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const auto guide = slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "skills" / "cmdatl-strategy" / "SKILL.md");
  // The walkthrough this case checks, line for line: a changed walkthrough changes this case in the same commit.
  const std::vector<std::string> expected{
    "cmdatl strategy new dip --template mean-reversion",
    "cmdatl strategy reference builtins",
    "atlc check dip.atls",
    "cmdatl strategy backtest dip.toml params.lookback=30",
    "cmdatl run dip.toml",
  };
  REQUIRE(block_after(guide, "## A session", "```sh") == expected);
  const auto block = block_after(guide, "## Runs", "```toml");
  REQUIRE(!block.empty());
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  ScratchDir work;
  const InDir here(work.path());

  const auto made = fixture.json({"strategy", "new", "dip", "--template", "mean-reversion"});
  REQUIRE(made.exit_ == 0);
  CHECK(std::filesystem::exists("dip.atls"));
  auto spec = slurp("dip.toml");
  // The starter places market orders and names no market: a run refuses it until the guide's block replaces its
  // last three tables.
  CHECK(spec.find("type = \"market\"") != std::string::npos);
  CHECK(spec.find("market = ") == std::string::npos);
  const auto reference = fixture.json({"strategy", "reference", "builtins"});
  CHECK(reference.exit_ == 0);
  CHECK(reply_of(reference.out_).at("text").as_string().find("zscore") != std::string::npos);
  const auto checked = cmdatl::run_child({equipment->first, "check", "dip.atls"}, [](const std::string&) {});
  CHECK(checked.kind_ == cmdatl::ChildResult::Kind::FINISHED);
  CHECK(checked.code_ == 0);
  spit("bars.csv", dip_bars(160));
  const auto refused = run_process({"dip.toml"});
  INFO(refused.out_);
  CHECK(refused.exit_ == 2);
  CHECK(run_code(refused) == "spec.invalid");
  CHECK(refused.reply_.at("key").as_string() == "replay.market");
  const auto tables = spec.find("[instruments]");
  REQUIRE(tables != std::string::npos);
  REQUIRE(spec.find("[replay]") > tables);
  REQUIRE(spec.find("[orders]") > spec.find("[replay]"));
  spec.erase(tables);
  for (const auto& line : block) {
    spec += line + "\n";
  }
  spit("dip.toml", spec);

  const auto backtest = fixture.json({"strategy", "backtest", "dip.toml", "params.lookback=30"});
  INFO(backtest.err_);
  REQUIRE(backtest.exit_ == 0);
  const auto backtested = reply_of(backtest.out_);
  const auto& report = backtested.at("report").as_object();
  const std::vector<std::string> keys{
    "total_return", "sharpe_annualized", "max_drawdown", "executions", "commission_paid", "tax_paid"
  };
  for (const auto& key : keys) {
    CHECK(report.contains(key));
  }

  const auto account_before = fixture.json({"account"}).out_;
  const auto orders_before = fixture.json({"order", "list"}).out_;
  const auto ran = run_process({"dip.toml"});
  INFO(ran.out_);
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  CHECK(ran.reply_.at("result").as_string() == "ended");
  for (const auto* key : {"executions", "commission_paid", "closing_equity", "holdings_at_end", "refused"}) {
    CHECK(ran.reply_.contains(key));
  }
  CHECK(ran.reply_.at("executions").as_int64() > 0);
  CHECK(!ran.events_.empty());
  // The mock account's commands never see the run's market.
  CHECK(fixture.json({"account"}).out_ == account_before);
  CHECK(fixture.json({"order", "list"}).out_ == orders_before);
}

TEST_CASE("the README's quick start runs on the example mock account and its run spec", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const RunEquipped equipped(equipment->first, equipment->second);
  // The README's lines this case runs, as the README has them.
  const auto readme = slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "README.md");
  const std::vector<std::string> lines{
    "cmdatl order plan AAA --target 10 --limit 102 #", "cmdatl run examples/run.toml #"
  };
  for (const auto& line : lines) {
    INFO(line);
    CHECK(readme.find("\n" + line) != std::string::npos);
  }
  // A copy of the examples, as a clone holds them, with the account's state in the copy's own directory.
  const ScratchDir dir;
  std::filesystem::copy(
    std::filesystem::path(CMDATL_SOURCE_DIR) / "examples", dir.path() / "examples",
    std::filesystem::copy_options::recursive
  );
  // Without what a reader of the README may have written beside the example account in the source tree.
  std::vector<std::filesystem::path> written;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "examples")) {
    const auto name = entry.path().filename().string();
    if (name.rfind("mock.", 0) == 0 && name != "mock.toml") {
      written.push_back(entry.path());
    }
  }
  for (const auto& path : written) {
    std::filesystem::remove_all(path);
  }
  const ScopedVariable config("CMDATL_CONFIG", (dir.path() / "examples" / "mock.toml").string());
  const ScopedVariable state("XDG_STATE_HOME", (dir.path() / "state").string());
  const InDir here(dir.path());
  const auto ask = [](std::vector<std::string> args) {
    args.insert(args.begin(), "--json");
    return run(args);
  };
  // The bars are read when the account opens, not when the operator's file is.
  const auto status = ask({"status"});
  INFO(status.out_ << status.err_);
  REQUIRE(status.exit_ == 0);
  CHECK(reply_of(status.out_).at("phase").as_string() == "open");
  CHECK(reply_of(status.out_).at("armed").as_bool() == false);
  spit(dir.path() / "examples" / "mock.armed", "");
  const auto plan = ask({"order", "plan", "AAA", "--target", "10", "--limit", "102"});
  INFO(plan.out_ << plan.err_);
  REQUIRE(plan.exit_ == 0);
  const auto placed = ask({"order", "place", std::string(reply_of(plan.out_).at("plan_id").as_string())});
  INFO(placed.out_ << placed.err_);
  REQUIRE(placed.exit_ == 0);
  CHECK(reply_of(placed.out_).at("reason").as_string() == "placed");
  // Placed during the 3rd's session, the order fills against the 4th's bar, at its limit, at that bar's close.
  const auto advanced = ask({"clock", "advance", "--to", "2024-01-04T15:30"});
  INFO(advanced.out_ << advanced.err_);
  REQUIRE(advanced.exit_ == 0);
  const auto events = reply_of(advanced.out_).at("events").as_array();
  REQUIRE(events.size() == 1);
  CHECK(events.at(0).as_object().at("kind").as_string() == "filled");
  CHECK(events.at(0).as_object().at("price").as_string() == "102");
  CHECK(events.at(0).as_object().at("at").as_string() == "2024-01-04T15:30:00+09:00");
  const auto reconciled = ask({"order", "reconcile", "AAA"});
  INFO(reconciled.out_ << reconciled.err_);
  REQUIRE(reconciled.exit_ == 0);
  CHECK(reply_of(reconciled.out_).at("reason").as_string() == "clean");
  CHECK(reply_of(ask({"account"}).out_).at("cash").as_string() == "998980");
  // The run spec run accepts: it ends clean, every order placed is filled, none is refused, and it trades as the
  // backtest of the same program and bars does (10 executions).
  const auto ran = run_process({"examples/run.toml"});
  INFO(ran.out_ << ran.err_);
  REQUIRE(ran.exit_ == 0);
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("executions").as_int64() == 10);
  const auto& refused = ran.reply_.at("refused").as_object();
  CHECK(refused.at("boundary").as_int64() == 0);
  CHECK(refused.at("guard").as_int64() == 0);
  CHECK(refused.at("pricing").as_int64() == 0);
  std::size_t placed_events = 0;
  std::size_t filled_events = 0;
  for (const auto& event : ran.events_) {
    placed_events += event.at("event").as_string() == "placed" ? 1 : 0;
    filled_events += event.at("event").as_string() == "filled" ? 1 : 0;
  }
  CHECK(placed_events > 0);
  CHECK(filled_events == placed_events);
}
#endif

TEST_CASE("a run reads the program's data {} as its backtest does and keeps the rails", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const auto write = [](const std::filesystem::path& dir) {
    spit(dir / "follow.atls", "data { real level; }\non_bar {\n  target(level);\n}\n");
    spit(dir / "bars.csv", dip_bars(60));
    // Known at midnight UTC of the 10th and 30th bar's dates, before those days' closes.
    std::vector<std::string> dates;
    std::istringstream lines(dip_bars(60));
    std::string line;
    std::getline(lines, line);
    while (std::getline(lines, line)) {
      const auto first = line.find(',');
      dates.push_back(line.substr(first + 1, line.find(',', first + 1) - first - 1));
    }
    spit(dir / "level.csv", "date,level\n" + dates[9] + ",1\n" + dates[29] + ",0\n");
    spit(
      dir / "follow.toml",
      "strategy = \"follow.atls\"\n\n[bars]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol_column = \"symbol\"\n"
      "time = \"date\"\ntime_format = \"iso\"\n\n[data.level]\nfile = \"level.csv\"\ntime = \"date\"\ntime_format = \"iso\"\n\n"
      "[instruments]\nstock = \"005930\"\n\n[replay]\ncash = 100000000\nmarket = \"kr\"\n\n[orders]\ntype = \"limit\"\n"
      "offset = 0.01\n"
    );
  };
  {
    const PaperFixture fixture(run_operator());
    const RunEquipped equipped(equipment->first, equipment->second);
    fixture.arm();
    write(fixture.dir());
    const auto spec = (fixture.dir() / "follow.toml").string();
    const auto backtest = fixture.json({"strategy", "backtest", spec});
    INFO(backtest.out_);
    REQUIRE(backtest.exit_ == 0);
    const auto report = reply_of(backtest.out_).at("report").as_object();
    CHECK(report.at("records").as_int64() == 2);
    CHECK(report.at("executions").as_int64() == 2);
    const auto ran = run_process({spec});
    INFO(ran.out_);
    INFO(ran.err_);
    REQUIRE(ran.exit_ == 0);
    CHECK(ran.reply_.at("executions") == report.at("executions"));
  }
  {
    // Not armed: refused before anything is built, as any other run.
    const PaperFixture fixture(run_operator());
    const RunEquipped equipped(equipment->first, equipment->second);
    write(fixture.dir());
    const auto unarmed = run_process({(fixture.dir() / "follow.toml").string()});
    CHECK(unarmed.exit_ == 3);
    CHECK(run_code(unarmed) == "refused.not_armed");
  }
}
