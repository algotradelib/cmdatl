#include "./paper-support.hpp"
#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using namespace support;

namespace {

/**
 * @brief An `atlc check --info` reply without a program.
 * @param diagnostics The diagnostics, a JSON array.
 * @return The reply.
 */
std::string failed_info(const std::string& diagnostics) {
  return R"({"atlc_info":1,"status":"error","atlc":"0.0.0-9000","program":null,"diagnostics":)" + diagnostics + "}";
}

/** @brief A program error as `atlc check --info` gives it. */
const std::string PROGRAM_ERROR =
  R"([{"severity":"error","code":"undefined-name","message":"`lookbak` is not defined","file":"@DIR@/dip.atls",)"
  R"("line":9,"origin":"program"}])";

/** @brief A file atlc could not read, as `atlc check --info` gives it. */
const std::string UNREADABLE =
  R"([{"severity":"error","code":"unreadable","message":"cannot read @DIR@/dip.atls","file":"@DIR@/dip.atls",)"
  R"("line":null,"origin":"program"}])";

/** @brief A usage error as `atlc check --info` gives it. */
const std::string USAGE_ERROR =
  R"([{"severity":"error","code":"usage","message":"unknown option `--x`","file":null,"line":null,)"
  R"("origin":"command_line"}])";

/**
 * @brief The fake's `--info` reply with one text replaced.
 * @param from The text.
 * @param to Its replacement.
 * @return The reply.
 */
std::string info_with(const std::string& from, const std::string& to) {
  auto text = INFO_OK;
  text.replace(text.find(from), from.size(), to);
  return text;
}

/** @brief One backtest against the fake: what atlc and the driver do, and what cmdatl must answer. */
struct Case {
  /** @brief What it is. */
  std::string name_;
  /** @brief The fake's script. */
  std::string script_;
  /** @brief The exit code expected. */
  int exit_;
  /** @brief The error code expected, or empty for a success. */
  std::string code_;
  /** @brief Whether the fake answers `--version` first. */
  bool version_{true};
  /** @brief Settings after the target. */
  std::vector<std::string> settings_{};
};

/**
 * @brief Every case of the backtest's mapping.
 * @return The cases.
 */
std::vector<Case> cases() {
  const auto info_ok = "check out " + INFO_OK + "\n";
  return {
    {"a report", info_ok + "driver err building\ndriver out " + REPORT + "\n", 0, ""},
    {"program errors", "check out " + failed_info(PROGRAM_ERROR) + "\ncheck exit 1\n", 5, "program.invalid"},
    {"a program atlc cannot read", "check out " + failed_info(UNREADABLE) + "\ncheck exit 2\n", 5, "program.invalid"},
    {"atlc refuses cmdatl's arguments", "check out " + failed_info(USAGE_ERROR) + "\ncheck exit 2\n", 1, "atlc.failed"},
    {"a setting the spec cannot take", info_ok, 2, "spec.invalid", true, {"replay.cash=0"}},
    {"a setting the program cannot take", info_ok, 2, "spec.invalid", true, {"params.nope=1"}},
    {"an unknown setting", info_ok, 2, "spec.invalid", true, {"replay.cahs=1"}},
    {"a setting given twice", info_ok, 2, "spec.invalid", true, {"replay.cash=1", "replay.cash=2"}},
    {"no target", info_ok, 2, "usage.missing_argument", true, {"replay.cash=1", "--"}},
    {"two targets", info_ok, 2, "usage.bad_value", true, {"t.toml"}},
    {"an atlc without --info",
      "check err atlc check: unknown option `--info`\ncheck err usage: atlc <command> [arguments]\ncheck exit 2\n", 1,
      "atlc.unsupported"},
    {"exit 2 without JSON or usage", "check err atlc check: something else\ncheck exit 2\n", 1, "atlc.failed"},
    {"exit 2, usage, and a broken object", "check out {\"atlc_info\":1\ncheck err usage: atlc\ncheck exit 2\n", 1,
      "atlc.failed"},
    {"malformed JSON", "check out {not json\n", 1, "atlc.failed"},
    {"no reply", "check exit 0\n", 1, "atlc.failed"},
    {"two objects", info_ok + info_ok, 1, "atlc.failed"},
    {"an array", "check out []\n", 1, "atlc.failed"},
    {"error with a program", "check out " + info_with("\"ok\"", "\"error\"") + "\ncheck exit 1\n", 1, "atlc.failed"},
    {"error on exit 0", "check out " + failed_info(PROGRAM_ERROR) + "\n", 1, "atlc.failed"},
    {"ok on exit 1", info_ok + "check exit 1\n", 1, "atlc.failed"},
    {"another format", "check out " + info_with("\"atlc_info\":1", "\"atlc_info\":2") + "\n", 1, "atlc.failed"},
    {"another program's path", "check out " + info_with("@DIR@/dip.atls", "/elsewhere/dip.atls") + "\n", 1,
      "atlc.failed"},
    {"a struct that is code", "check out " + info_with("\"Dip\"", "\"Dip; int main\"") + "\n", 1, "atlc.failed"},
    {"a parameter of another type", "check out " + info_with("\"int\"", "\"float\"") + "\n", 1, "atlc.failed"},
    {"check killed by a signal", "check raise 9\n", 1, "atlc.failed"},
    {"check exit 101", "check err atlc: internal error\ncheck exit 101\n", 1, "atlc.failed"},
    {"a replay failure", info_ok + "driver err error: lookback=1 is below its lower bound 2\ndriver exit 1\n", 1,
      "replay.failed"},
    {"driver exit 101", info_ok + "driver exit 101\n", 1, "replay.failed"},
    {"driver killed by a signal", info_ok + "driver raise 9\n", 1, "replay.failed"},
    {"driver exit 130, an exit code and not a signal", info_ok + "driver exit 130\n", 1, "replay.failed"},
    {"driver exit 0 without a report", info_ok + "driver exit 0\n", 1, "replay.failed"},
    {"driver exit 0 with a broken report", info_ok + "driver out {\"bars\":\n", 1, "replay.failed"},
    {"driver exit 0 with two reports", info_ok + "driver out " + REPORT + "\ndriver out " + REPORT + "\n", 1,
      "replay.failed"},
    {"--version fails", "version exit 1\n", 1, "atlc.missing", false},
    {"--version says nothing readable", "version out atlc banana\n", 1, "atlc.unsupported", false},
  };
}

/**
 * @brief Checks one backtest's output in one format.
 * @param ran What it wrote.
 * @param format `json`, `jsonl` or `text`.
 * @param expected The case.
 */
void check_form(const Ran& ran, const std::string& format, const Case& expected) {
  INFO(expected.name_ << " (" << format << ")\nstdout: " << ran.out_ << "\nstderr: " << ran.err_);
  CHECK(ran.exit_ == expected.exit_);
  CHECK(declares({"strategy", "backtest"}, ran.exit_, expected.code_));
  const auto failed = !expected.code_.empty();
  // The words are sorted before anything is said: a usage error comes alone.
  const bool usage = expected.code_.rfind("usage.", 0) == 0;
  if (format == "json") {
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 1);
    const auto reply = reply_of(lines.front());
    CHECK(reply.contains("error") == failed);
    if (failed) {
      CHECK(code_of(lines.front()) == expected.code_);
    }
    if (!usage) {
      CHECK(lines_of(ran.err_).front() == "cmdatl: replaying s.toml");
    }
  } else if (format == "jsonl") {
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() >= (usage ? 1u : 2u));
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
      const auto event = reply_of(lines[i]);
      CHECK(event.at("event").as_string() == "progress");
      CHECK(event.at("message").is_string());
    }
    if (!usage) {
      CHECK(reply_of(lines.front()).at("message").as_string() == "cmdatl: replaying s.toml");
    }
    CHECK(reply_of(lines.back()).contains("error") == failed);
    if (failed) {
      CHECK(code_of(lines.back()) == expected.code_);
    }
    CHECK(ran.err_.empty());
  } else {
    if (!usage) {
      CHECK(lines_of(ran.err_).front() == "cmdatl: replaying s.toml");
    }
    if (failed) {
      CHECK(ran.out_.empty());
      CHECK(ran.err_.find("  code: " + expected.code_ + "\n") != std::string::npos);
    } else {
      CHECK(ran.out_.find("report:\n") != std::string::npos);
      CHECK(ran.out_.find("executions: 4") != std::string::npos);
    }
  }
}

/** @brief A stream buffer that notes, at each flush, whether the fake had already made a mark. */
class MarkWatcher : public std::stringbuf {
public:
  /**
   * @brief Watches for a line.
   * @param fake The fake.
   * @param line The line.
   * @param mark The mark that must not exist yet when the line arrives.
   */
  MarkWatcher(const FakeAtlc& fake, std::string line, std::string mark)
  : fake_(fake), line_(std::move(line)), mark_(std::move(mark)) {}

  /** @brief Whether the line arrived. */
  bool seen_{false};
  /** @brief Whether the mark existed when it arrived. */
  bool marked_when_seen_{false};

protected:
  /**
   * @brief Looks for the line at a flush.
   * @return 0.
   */
  int sync() override {
    if (!seen_ && str().find(line_ + "\n") != std::string::npos) {
      seen_ = true;
      marked_when_seen_ = fake_.marked(mark_);
    }
    return 0;
  }

private:
  const FakeAtlc& fake_;
  std::string line_;
  std::string mark_;
};

/**
 * @brief Runs `strategy new` in process.
 * @param args What follows `strategy new`.
 * @return What it wrote.
 */
Ran strategy_new(const std::vector<std::string>& args) {
  std::vector<std::string> line{"strategy", "new"};
  line.insert(line.end(), args.begin(), args.end());
  return run(line);
}

/**
 * @brief The template a name was written from, as `strategy new` writes it.
 * @param name The template.
 * @return Its text.
 */
std::string template_text(const std::string& name) {
  for (const auto& entry : cmdatl::TEMPLATES) {
    if (name == entry.name_) {
      return entry.text_;
    }
  }
  return "";
}

} // namespace

TEST_CASE("every answer of atlc check --info and the driver maps to its reply in json and jsonl and text") {
  const Watchdog watchdog("the mapping", 300);
  for (const auto& expected : cases()) {
    for (const auto* format : {"json", "jsonl", "text"}) {
      const FakeAtlc fake(expected.script_, expected.version_);
      std::vector<std::string> line{"--format", format, "strategy", "backtest"};
      if (expected.code_ != "usage.missing_argument") {
        line.push_back("s.toml");
      }
      for (const auto& setting : expected.settings_) {
        if (setting != "--") {
          line.push_back(setting);
        }
      }
      check_form(run(line), format, expected);
    }
  }
}

TEST_CASE("the backtest takes the program's measures from atlc and refuses a spec measure of the same name") {
  const Watchdog watchdog("the program's measures", 60);
  auto info = INFO_OK;
  const std::string params = R"("params":[{"name":"lookback","type":"int"}])";
  info.replace(info.find(params), params.size(), params + R"(,"measures":[{"name":"total","type":"real"}])");
  const FakeAtlc fake("check out " + info + "\ndriver args\ndriver out " + REPORT + "\n");
  spit("other.toml", "strategy = 'dip.atls'\nmeasures = ['calmar']\n[bars]\nfile = 'bars.csv'\n");
  CHECK(run({"strategy", "backtest", "other.toml"}).exit_ == 0);
  CHECK(!fake.args("driver").empty());
  std::filesystem::remove(fake.dir() / "script.driver.args");
  spit("clash.toml", "strategy = 'dip.atls'\nmeasures = ['calmar', 'total']\n[bars]\nfile = 'bars.csv'\n");
  const auto clash = run({"strategy", "backtest", "clash.toml"});
  CHECK(clash.exit_ == 2);
  CHECK(code_of(clash.out_) == "spec.invalid");
  const auto reply = reply_of(clash.out_);
  const auto& error = reply.at("errors").as_array().at(0).as_object();
  CHECK(error.at("line").as_int64() == 2);
  CHECK(error.at("message").as_string() == "`measures`: \"total\" is the program's own measure");
  CHECK(fake.args("driver").empty());
}

TEST_CASE("the backtest refuses --fills and --equity naming one file before anything runs") {
  const Watchdog watchdog("one output file", 60);
  const FakeAtlc fake("check out " + INFO_OK + "\ndriver out " + REPORT + "\n");
  spit("s.toml", FAKE_SPEC);
  spit("kept.csv", "kept\n");
  std::filesystem::create_symlink("kept.csv", "link.csv");
  std::filesystem::create_hard_link("kept.csv", "hard.csv");
  for (
    const auto& [fills, equity] : std::vector<std::pair<std::string, std::string>>{
      {"kept.csv", "kept.csv"},
      {"./kept.csv", "kept.csv"},
      {"no/../kept.csv", "kept.csv"},
      {"link.csv", "kept.csv"},
      {"hard.csv", "kept.csv"},
      {"new.csv", "./new.csv"},
    }
  ) {
    INFO(fills << " " << equity);
    const auto ran = run({"strategy", "backtest", "s.toml", "--fills", fills, "--equity", equity});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "usage.bad_value");
    CHECK(
      reply_of(ran.out_).at("error").as_object().at("message").as_string() ==
      "--fills and --equity name the same file: " + fills
    );
  }
  // Nothing ran and nothing was written.
  CHECK(fake.args("check").empty());
  CHECK(slurp("kept.csv") == "kept\n");
  CHECK(!std::filesystem::exists("new.csv"));
}

TEST_CASE("a program as the target: settings only, the spec null, paths against the working directory") {
  const Watchdog watchdog("a program target", 60);
  const FakeAtlc fake("check args\ncheck out " + INFO_OK + "\ndriver args\ndriver out " + REPORT + "\n");
  std::filesystem::create_directories("sub");
  const InDir sub("sub");
  const auto ran = run({"strategy", "backtest", "bars.file=b.csv", "../sub/./../dip.atls"});
  INFO(ran.out_);
  REQUIRE(ran.exit_ == 0);
  const auto dir = fake.dir().string();
  CHECK(fake.args("check") == std::vector<std::string>{dir + "/dip.atls", "--info"});
  CHECK(fake.args("driver") == std::vector<std::string>{"bars.source=wide", "bars.file=" + dir + "/sub/b.csv"});
  const auto reply = reply_of(ran.out_);
  CHECK(reply.at("spec").is_null());
  CHECK(reply.at("program").as_string() == dir + "/dip.atls");
  // Without a spec, a setting missing from the command line is reported with no place.
  const auto bare = run({"strategy", "backtest", "../dip.atls"});
  CHECK(bare.exit_ == 2);
  CHECK(code_of(bare.out_) == "spec.invalid");
  CHECK(
    reply_of(bare.out_).at("error").as_object().at("message").as_string().find("give the closes") != std::string::npos
  );
  // A spec that names no program, and one that cannot be read.
  spit("none.toml", "[bars]\nfile = 'x.csv'\n");
  CHECK(code_of(run({"strategy", "backtest", "none.toml"}).out_) == "spec.invalid");
  const auto missing = run({"strategy", "backtest", "missing.toml"});
  CHECK(missing.exit_ == 2);
  CHECK(reply_of(missing.out_).at("errors").as_array().at(0).at("code").as_string() == "unreadable");
  // strategy is not a setting on the command line.
  CHECK(code_of(run({"strategy", "backtest", "../s.toml", "strategy.x=1"}).out_) == "spec.invalid");
}

TEST_CASE("a program with a run spec: the spec's settings apply to it, and the spec names that program or none") {
  const Watchdog watchdog("a program with a spec", 60);
  const FakeAtlc fake("check args\ncheck out " + INFO_OK + "\ndriver args\ndriver out " + REPORT + "\n");
  const auto dir = fake.dir().string();
  std::filesystem::create_directories("specs");
  std::filesystem::create_directories("sub");
  spit("specs/shared.toml", "[bars]\nfile = 'bars.csv'\n[replay]\ncash = 5\n");
  const auto message = [](const Ran& ran) {
    return std::string(reply_of(ran.out_).at("error").as_object().at("message").as_string());
  };
  {
    // The words in any order; the program's path against the working directory, the spec's paths against the spec.
    const InDir sub("sub");
    const auto ran = run({"strategy", "backtest", "replay.commission=0.001", "../specs/shared.toml", "../dip.atls"});
    INFO(ran.out_ << ran.err_);
    REQUIRE(ran.exit_ == 0);
    CHECK(lines_of(ran.err_).front() == "cmdatl: replaying ../dip.atls with ../specs/shared.toml");
    CHECK(fake.args("check") == std::vector<std::string>{dir + "/dip.atls", "--info"});
    CHECK(
      fake.args("driver") ==
      std::vector<std::string>{
        "bars.source=wide", "bars.file=" + dir + "/specs/bars.csv", "replay.cash=5", "replay.commission=0.001"
      }
    );
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("spec").as_string() == dir + "/specs/shared.toml");
    CHECK(reply.at("program").as_string() == dir + "/dip.atls");
  }
  const auto checks = [&fake] {
    return fake.args("check").size();
  };
  // A spec naming the same program, written with `..`, applies; the program's line comes from the command line.
  spit("specs/same.toml", "strategy = '../a/../dip.atls'\n[bars]\nfile = 'bars.csv'\n");
  CHECK(run({"strategy", "backtest", "specs/same.toml", "./dip.atls"}).exit_ == 0);
  // Another program, or the same one through a link, is refused at the spec's line before atlc runs.
  const auto before = checks();
  std::filesystem::create_symlink("dip.atls", "link.atls");
  for (const auto* named : {"two.atls", "../link.atls"}) {
    INFO(named);
    spit("specs/other.toml", std::string("# shared\nstrategy = '") + named + "'\n[bars]\nfile = 'bars.csv'\n");
    const auto ran = run({"strategy", "backtest", "dip.atls", "specs/other.toml"});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "spec.invalid");
    CHECK(message(ran).find("another program than " + dir + "/dip.atls") != std::string::npos);
    const auto reply = reply_of(ran.out_);
    const auto& error = reply.at("errors").as_array().at(0).as_object();
    CHECK(error.at("file").as_string() == dir + "/specs/other.toml");
    CHECK(error.at("line").as_int64() == 2);
  }
  // `strategy` keeps its own type check when a program is given.
  spit("specs/typed.toml", "strategy = 3\n[bars]\nfile = 'bars.csv'\n");
  CHECK(code_of(run({"strategy", "backtest", "dip.atls", "specs/typed.toml"}).out_) == "spec.invalid");
  CHECK(checks() == before);
  // Without a program, the spec must name one.
  const auto alone = run({"strategy", "backtest", "specs/shared.toml"});
  CHECK(alone.exit_ == 2);
  CHECK(code_of(alone.out_) == "spec.invalid");
  CHECK(message(alone).find("no `strategy") != std::string::npos);
  // Two programs are refused, naming them; a mistyped setting read as the spec says what a setting is.
  const auto two = run({"strategy", "backtest", "dip.atls", "params.lookback=3", "two.atls"});
  CHECK(two.exit_ == 2);
  CHECK(code_of(two.out_) == "usage.bad_value");
  CHECK(message(two).find("dip.atls, two.atls") != std::string::npos);
  const auto typo = run({"strategy", "backtest", "dip.atls", "lookback=30"});
  CHECK(typo.exit_ == 2);
  CHECK(code_of(typo.out_) == "spec.invalid");
  CHECK(message(typo).find("a setting is KEY=VALUE with a dotted key") != std::string::npos);
  CHECK(checks() == before);
}

TEST_CASE("strategy new: the name rules with atlc's reserved names") {
  const ScratchDir dir;
  const auto refused = [&dir](const std::string& name) {
    const auto ran = strategy_new({name, "--dir", dir.path().string()});
    INFO("'" << name << "': " << ran.out_);
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "strategy.name");
    CHECK(declares({"strategy", "new"}, 2, "strategy.name"));
  };
  for (
    const auto* name :
    {"a.b", "a b", "1dip", "d\xc3\xafp", "a/b", "..", "params", "Params", "lane", "Lane", "params-", "lane__", "_dip"}
  ) {
    refused(name);
  }
  refused(std::string(65, 'a'));
  CHECK(cmdatl::name_problem(""));
  CHECK(cmdatl::camel_case("p-arams") == "PArams");
  CHECK(cmdatl::camel_case("pa_rams") == "PaRams");
  CHECK(cmdatl::camel_case("mean-reversion") == "MeanReversion");
  for (const auto* name : {"p-arams", "pa_rams", "la-ne", "dip", "a", "mean-reversion", "Dip_2"}) {
    INFO(name);
    CHECK(strategy_new({name, "--dir", dir.path().string()}).exit_ == 0);
  }
  CHECK(strategy_new({std::string(64, 'a'), "--dir", dir.path().string()}).exit_ == 0);
  // Nothing was written for a refused name.
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    (void)entry;
    ++files;
  }
  CHECK(files == 16);
}

TEST_CASE("strategy new writes each template and a spec atlc reads and lists the templates on a bad one") {
  const ScratchDir dir;
  for (const auto& entry : cmdatl::TEMPLATES) {
    const std::string name = std::string("t-") + entry.name_;
    const auto ran = strategy_new({name, "--template", entry.name_, "--dir", dir.path().string()});
    REQUIRE(ran.exit_ == 0);
    CHECK(slurp(dir.path() / (name + ".atls")) == entry.text_);
    const auto spec = slurp(dir.path() / (name + ".toml"));
    CHECK(spec.find("strategy = \"" + name + ".atls\"") != std::string::npos);
    CHECK(spec.find("@NAME@") == std::string::npos);
    CHECK(spec.find("type = \"market\"") != std::string::npos);
  }
  CHECK(std::string(cmdatl::TEMPLATES[0].name_) == "mean-reversion");
  CHECK(strategy_new({"plain", "--dir", dir.path().string()}).exit_ == 0);
  CHECK(slurp(dir.path() / "plain.atls") == template_text("mean-reversion"));
  const auto bad = strategy_new({"other", "--template", "nope", "--dir", dir.path().string()});
  CHECK(bad.exit_ == 2);
  CHECK(code_of(bad.out_) == "strategy.template");
  CHECK(reply_of(bad.out_).at("error").as_object().at("next").as_string().find("momentum") != std::string::npos);
  CHECK(!std::filesystem::exists(dir.path() / "other.toml"));
}

TEST_CASE("strategy new refuses an existing destination of any kind and writes nothing") {
  const ScratchDir dir;
  const auto path = [&dir](const std::string& file) {
    return dir.path() / file;
  };
  spit(path("a.atls"), "mine");
  spit(path("b.toml"), "mine");
  spit(path("c.atls"), "mine");
  spit(path("c.toml"), "mine");
  std::filesystem::create_directory(path("d.atls"));
  std::filesystem::create_symlink(path("nowhere"), path("e.toml"));
  for (const auto* name : {"a", "b", "c", "d", "e"}) {
    INFO(name);
    const auto ran = strategy_new({name, "--dir", dir.path().string()});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "strategy.exists");
    CHECK(declares({"strategy", "new"}, 1, "strategy.exists"));
  }
  CHECK(slurp(path("a.atls")) == "mine");
  CHECK(!std::filesystem::exists(path("a.toml")));
  CHECK(!std::filesystem::exists(path("b.atls")));
  CHECK(slurp(path("c.toml")) == "mine");
  CHECK(!std::filesystem::exists(path("d.toml")));
  CHECK(!std::filesystem::exists(path("e.atls")));
  CHECK(std::filesystem::is_symlink(path("e.toml")));
  CHECK(!std::filesystem::exists(path("nowhere")));
}

TEST_CASE("strategy new removes the first file when the second cannot be created or written") {
  const ScratchDir dir;
  const auto program = dir.path() / "dip.atls";
  const auto spec = dir.path() / "dip.toml";
  {
    // The second open finds no descriptor left.
    rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
    int highest = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/fd")) {
      highest = std::max(highest, std::stoi(entry.path().filename().string()));
    }
    rlimit low = saved;
    low.rlim_cur = static_cast<rlim_t>(highest + 8);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &low) == 0);
    // Take every descriptor left, then give one back: the program's open gets it, the spec's finds none.
    std::vector<int> taken;
    for (int fd = 0; (fd = ::open("/dev/null", O_RDONLY)) >= 0;) {
      taken.push_back(fd);
    }
    REQUIRE(!taken.empty());
    ::close(taken.back());
    taken.pop_back();
    const auto result = cmdatl::write_pair(program, "program", spec, "spec", false);
    for (const int fd : taken) {
      ::close(fd);
    }
    ::setrlimit(RLIMIT_NOFILE, &saved);
    REQUIRE(result);
    CHECK(code_of(boost::json::serialize(result->body_)) == "file.unwritable");
    CHECK(!std::filesystem::exists(program));
    CHECK(!std::filesystem::exists(spec));
  }
  {
    // The second write passes the size limit: the program fits under it, the spec does not.
    const auto previous = ::signal(SIGXFSZ, SIG_IGN);
    rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    rlimit low = saved;
    low.rlim_cur = 100;
    REQUIRE(::setrlimit(RLIMIT_FSIZE, &low) == 0);
    const auto result = cmdatl::write_pair(program, std::string(50, 'p'), spec, std::string(500, 's'), false);
    ::setrlimit(RLIMIT_FSIZE, &saved);
    ::signal(SIGXFSZ, previous);
    REQUIRE(result);
    CHECK(code_of(boost::json::serialize(result->body_)) == "file.unwritable");
    CHECK(!std::filesystem::exists(program));
    CHECK(!std::filesystem::exists(spec));
  }
}

TEST_CASE("strategy new --force replaces regular files and refuses a directory or a link or a FIFO first") {
  const Watchdog watchdog("--force", 60);
  const ScratchDir dir;
  const auto path = [&dir](const std::string& file) {
    return dir.path() / file;
  };
  spit(path("one.atls"), "old");
  spit(path("both.atls"), "old");
  spit(path("both.toml"), "old");
  for (const auto* name : {"one", "both"}) {
    INFO(name);
    CHECK(strategy_new({name, "--dir", dir.path().string(), "--force"}).exit_ == 0);
    CHECK(slurp(path(std::string(name) + ".atls")) == template_text("mean-reversion"));
    CHECK(
      slurp(path(std::string(name) + ".toml")).find("strategy = \"" + std::string(name) + ".atls\"") !=
      std::string::npos
    );
  }
  spit(path("target"), "the link's target");
  spit(path("dir.atls"), "keep me");
  std::filesystem::create_directory(path("dir.toml"));
  spit(path("link.atls"), "keep me");
  std::filesystem::create_symlink(path("target"), path("link.toml"));
  spit(path("fifo.atls"), "keep me");
  REQUIRE(::mkfifo(path("fifo.toml").c_str(), 0600) == 0);
  std::filesystem::create_symlink(path("target"), path("first.atls"));
  for (const auto* name : {"dir", "link", "fifo", "first"}) {
    INFO(name);
    const auto ran = strategy_new({name, "--dir", dir.path().string(), "--force"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "strategy.exists");
  }
  CHECK(slurp(path("dir.atls")) == "keep me");
  CHECK(slurp(path("link.atls")) == "keep me");
  CHECK(slurp(path("fifo.atls")) == "keep me");
  CHECK(slurp(path("target")) == "the link's target");
  CHECK(!std::filesystem::exists(path("first.toml")));
}

TEST_CASE("strategy new --force refuses two names for one file and leaves its bytes") {
  const ScratchDir dir;
  const auto program = dir.path() / "dip.atls";
  const auto spec = dir.path() / "dip.toml";
  spit(program, "saved program\n");
  std::filesystem::create_hard_link(program, spec);
  const auto ran = strategy_new({"dip", "--dir", dir.path().string(), "--force"});
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "strategy.exists");
  CHECK(slurp(program) == "saved program\n");
  CHECK(slurp(spec) == "saved program\n");
  CHECK(std::filesystem::equivalent(program, spec));
}

TEST_CASE("strategy new in a directory with spaces gives a next command usable from the working directory") {
  const ScratchDir dir;
  std::filesystem::create_directories(dir.path() / "work");
  std::filesystem::create_directories(dir.path() / "my strats" / "it's here");
  const InDir here(dir.path() / "work");
  const auto ran = strategy_new({"dip", "--dir", "../my strats/it's here"});
  REQUIRE(ran.exit_ == 0);
  const auto reply = reply_of(ran.out_);
  const auto expected = dir.path() / "my strats" / "it's here";
  CHECK(reply.at("program").as_string() == (expected / "dip.atls").string());
  CHECK(reply.at("spec").as_string() == (expected / "dip.toml").string());
  const auto& next = reply.at("next").as_array();
  REQUIRE(next.size() == 2);
  const auto backtest = shell_words(std::string(next[0].as_string()));
  REQUIRE(backtest);
  REQUIRE(backtest->size() == 4);
  CHECK((*backtest)[0] == "cmdatl");
  CHECK(std::filesystem::equivalent((*backtest)[3], expected / "dip.toml"));
  const cmdatl::Table table(cmdatl::command_list());
  const std::vector<std::string> words(backtest->begin() + 1, backtest->end());
  CHECK(cmdatl::prepare(words, table, false).action_ == cmdatl::Prepared::Action::RUN);
  const auto check = shell_words(std::string(next[1].as_string()));
  REQUIRE(check);
  REQUIRE(check->size() == 3);
  CHECK((*check)[0] == "atlc");
  CHECK(std::filesystem::equivalent((*check)[2], expected / "dip.atls"));
}

TEST_CASE("strategy new in a directory starting with a dash gives next commands that are not options") {
  const ScratchDir dir;
  const InDir here(dir.path());
  std::filesystem::create_directories(dir.path() / "-strategies");
  const auto ran = strategy_new({"dip", "--dir", "./-strategies"});
  REQUIRE(ran.exit_ == 0);
  const auto reply = reply_of(ran.out_);
  const auto& next = reply.at("next").as_array();
  REQUIRE(next.size() == 2);
  const auto backtest = shell_words(std::string(next[0].as_string()));
  REQUIRE(backtest);
  REQUIRE(backtest->size() == 4);
  CHECK((*backtest)[3].front() != '-');
  CHECK(std::filesystem::equivalent((*backtest)[3], dir.path() / "-strategies" / "dip.toml"));
  const cmdatl::Table table(cmdatl::command_list());
  const std::vector<std::string> words(backtest->begin() + 1, backtest->end());
  CHECK(cmdatl::prepare(words, table, false).action_ == cmdatl::Prepared::Action::RUN);
  const auto check = shell_words(std::string(next[1].as_string()));
  REQUIRE(check);
  REQUIRE(check->size() == 3);
  CHECK((*check)[2].front() != '-');
}

TEST_CASE("strategy new stops before writing anything once interrupted") {
  const ScratchDir dir;
  cmdatl::interruption = 1;
  const auto ran = strategy_new({"dip", "--dir", dir.path().string()});
  cmdatl::interruption = 0;
  CHECK(ran.exit_ == 130);
  CHECK(code_of(ran.out_) == "interrupted");
  CHECK(!std::filesystem::exists(dir.path() / "dip.atls"));
}

TEST_CASE("strategy reference maps atlc's refusals: a wrong chapter and an old atlc and a crash and no atlc") {
  {
    const FakeAtlc fake(
      "reference err atlc reference: no chapter `zz`; the chapters are builtins, syntax\nreference exit 1\n"
    );
    const auto ran = run({"--json", "strategy", "reference", "zz"});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "usage.bad_value");
    const auto reply = reply_of(ran.out_);
    const auto& error = reply.at("error").as_object();
    CHECK(error.at("message").as_string().find("no chapter `zz`") != std::string::npos);
    CHECK(error.at("next").as_string() == "cmdatl strategy reference lists the chapters");
  }
  {
    const FakeAtlc fake("reference err usage: atlc <command> [arguments]\nreference exit 2\n");
    const auto ran = run({"--json", "strategy", "reference"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "atlc.unsupported");
  }
  {
    // Exit 2 without atlc's usage is not taken for an old atlc.
    const FakeAtlc fake("reference err something else\nreference exit 2\n");
    const auto ran = run({"--json", "strategy", "reference"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "atlc.failed");
  }
  for (const auto* script : {"reference exit 7\n", "reference raise 11\n"}) {
    INFO(script);
    const FakeAtlc fake(script);
    const auto ran = run({"--json", "strategy", "reference"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "atlc.failed");
  }
  {
    const ScopedVariable atlc("CMDATL_ATLC", std::string("cmdatl-no-such-atlc"));
    const auto ran = run({"--json", "strategy", "reference"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "atlc.missing");
  }
  CHECK(no_children());
  CHECK(declares({"strategy", "reference"}, 2, "usage.bad_value"));
  CHECK(declares({"strategy", "reference"}, 1, "atlc.unsupported"));
  CHECK(declares({"strategy", "reference"}, 1, "atlc.failed"));
  CHECK(declares({"strategy", "reference"}, 1, "atlc.missing"));
  CHECK(declares({"strategy", "reference"}, 130, "interrupted"));
}

TEST_CASE("strategy reference refuses a topic atlc could not have before atlc runs") {
  const FakeAtlc fake("reference args\n");
  const std::vector<std::string> topics{
    "", "-h", "--json", "Builtins", "1st", "a/b", "../README", "run spec", "\xed\x95\x9c", std::string(65, 'a')
  };
  for (const auto& topic : topics) {
    INFO(topic);
    const auto ran = run({"--json", "strategy", "reference", "--", topic});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "usage.bad_value");
  }
  CHECK(!std::filesystem::exists(fake.dir() / "script.reference.args"));
  CHECK(cmdatl::valid_topic(std::string(64, 'a')));
  CHECK(cmdatl::valid_topic("types-and-definedness"));
}

namespace {

/**
 * @brief A replay line as the evaluation driver prints it.
 * @param index Its index.
 * @param tail What follows `path`: its report, or `report` null and a failure.
 * @return The line.
 */
std::string replay_line(int index, const std::string& tail = "\"report\":" + REPORT) {
  return "{\"index\":" + std::to_string(index) + ",\"params\":{\"lookback\":" + std::to_string(10 * (index + 1)) +
    "},\"window\":null,\"path\":null," + tail + "}";
}

/** @brief The summary of two replays, with a key the command does not declare. */
const std::string SUMMARY =
  R"({"summary":{"method":"grid","jobs":1,"param_sets":2,"invalid":0,"tried":2,"replays":2,"paths":null,)"
  R"("failed":0,"objective":"total_return","direction":"max","best":{"index":1,"params":{"lookback":20},)"
  R"("value":0.012345},"undeclared":1}})";

/**
 * @brief A script whose driver prints these lines.
 * @param lines The lines.
 * @return The script.
 */
std::string evaluation(const std::vector<std::string>& lines) {
  std::string script = "check out " + INFO_OK + "\ndriver args\n";
  for (const auto& line : lines) {
    script += "driver out " + line + "\n";
  }
  return script;
}

} // namespace

TEST_CASE("strategy evaluate emits each replay as an event and replies with the summary's declared keys") {
  const Watchdog watchdog("the evaluation", 60);
  const FakeAtlc fake(evaluation({replay_line(0), replay_line(1), SUMMARY}));
  const auto ran = run({"--json", "strategy", "evaluate", "s.toml", "grid.lookback=10,20"});
  INFO(ran.out_ << ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 3);
  const auto first = reply_of(lines[0]);
  CHECK(first.at("event").as_string() == "replay");
  CHECK(first.at("env").as_string() == "offline");
  CHECK(first.at("index").as_int64() == 0);
  CHECK(first.at("report").as_object().at("executions").as_int64() == 4);
  const auto final_reply = reply_of(lines[2]);
  CHECK(final_reply.at("best").as_object().at("index").as_int64() == 1);
  CHECK(final_reply.at("param_sets").as_int64() == 2);
  CHECK(!final_reply.contains("undeclared"));
  CHECK(final_reply.at("program").as_string() == (fake.dir() / "dip.atls").string());
  const auto args = fake.args("driver");
  CHECK(std::find(args.begin(), args.end(), "evaluate.method=grid") != args.end());
  CHECK(std::find(args.begin(), args.end(), "grid.lookback=10,20") != args.end());
  // A summary key the table declares: every key the driver may give is one.
  const auto table = cmdatl::Table(cmdatl::command_list());
  const auto* evaluate = table.resolve({"strategy", "evaluate"}).command_;
  for (const auto& key : cmdatl::EVALUATE_SUMMARY) {
    CHECK(cmdatl::declares_field(*evaluate, key));
  }
}

TEST_CASE("strategy evaluate: --fields selects in the lines and the reply, --value in the reply only") {
  const Watchdog watchdog("the evaluation's selections", 60);
  const auto script = evaluation({replay_line(0), replay_line(1), SUMMARY});
  {
    const FakeAtlc fake(script);
    const auto ran = run({"--json", "--fields", "index,report.total_return,best", "strategy", "evaluate", "s.toml"});
    REQUIRE(ran.exit_ == 0);
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 3);
    const auto line = reply_of(lines[1]);
    CHECK(line.size() == 4);
    CHECK(line.at("report").as_object().size() == 1);
    CHECK(line.at("event").as_string() == "replay");
    const auto final_reply = reply_of(lines[2]);
    CHECK(final_reply.contains("best"));
    CHECK(!final_reply.contains("warning"));
    CHECK(!final_reply.contains("param_sets"));
  }
  {
    const FakeAtlc fake(script);
    const auto ran = run({"--json", "--value", "best.index", "strategy", "evaluate", "s.toml"});
    CHECK(lines_of(ran.out_).back() == "1");
  }
  for (const auto* field : {"index", "report.total_return"}) {
    const FakeAtlc fake(script);
    const auto ran = run({"--json", "--value", field, "strategy", "evaluate", "s.toml"});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "usage.no_such_field");
    CHECK(fake.args("driver").empty());
  }
}

TEST_CASE("strategy evaluate: a broken line protocol, a stop or no report is replay.failed with the lines counted") {
  const Watchdog watchdog("the evaluation's failures", 60);
  const auto failed = replay_line(0, R"("report":null,"failure":{"kind":"params","message":"k is below 0"})");
  const auto stopped = evaluation({replay_line(0), R"({"stopped":{"code":"evaluate.unheard","message":"no fold"}})"}) +
    "driver err error: no fold\ndriver exit 1\n";
  const std::vector<std::tuple<std::string, std::string, std::int64_t>> cases{
    {evaluation({replay_line(1)}), "the evaluation driver broke its line protocol: replay 1 where 0 was next", 0},
    {evaluation({replay_line(0), "nope"}),
      "the evaluation driver broke its line protocol: a line that is not a JSON object", 1},
    {evaluation({replay_line(0)}), "the evaluation failed: the evaluation driver exited 0 without its summary", 1},
    {evaluation({replay_line(0), SUMMARY}), "the evaluation driver's summary disagrees with its lines", 1},
    {stopped, "the evaluation stopped: no fold", 1},
    {evaluation({replay_line(0)}) + "driver err error: cannot open bars.csv\ndriver exit 1\n",
      "the evaluation failed: error: cannot open bars.csv", 1},
    {evaluation({failed, R"({"summary":{"replays":1}})"}), "every replay failed or was refused: k is below 0", 1},
  };
  for (const auto& [script, message, replays] : cases) {
    INFO(message);
    const FakeAtlc fake(script);
    const auto ran = run({"--json", "strategy", "evaluate", "s.toml"});
    CHECK(ran.exit_ == 1);
    const auto reply = reply_of(lines_of(ran.out_).back());
    CHECK(reply.at("error").as_object().at("code").as_string() == "replay.failed");
    CHECK(reply.at("error").as_object().at("message").as_string() == message);
    CHECK(reply.at("replays").as_int64() == replays);
  }
  // A refusal of the settings before any replay is exit 2, with the driver's next.
  const FakeAtlc refusing(
    evaluation(
      {R"({"stopped":{"code":"evaluate.refused","message":"no window fits","next":"give a shorter windows.length"}})"}
    ) +
    "driver err error: no window fits\ndriver exit 1\n"
  );
  const auto refused = run({"--json", "strategy", "evaluate", "s.toml"});
  CHECK(refused.exit_ == 2);
  const auto error = reply_of(lines_of(refused.out_).back()).at("error").as_object();
  CHECK(error.at("code").as_string() == "evaluate.refused");
  CHECK(error.at("message").as_string() == "the evaluation stopped: no window fits");
  CHECK(error.at("next").as_string() == "give a shorter windows.length");
}

TEST_CASE("an interruption while strategy evaluate streams stops its driver, keeps the lines and answers 130") {
  const Watchdog watchdog("interrupted evaluation", 60);
  const FakeAtlc fake(evaluation({replay_line(0)}) + "driver pid\ndriver touch streaming\ndriver block\n");
  // A real SIGINT to this thread, noted by cmdatl's handler, once the driver has printed its line (the stand-in
  // driver is a test build's hook, so the command runs in this process).
  struct sigaction noted{};
  struct sigaction saved{};
  noted.sa_handler = cmdatl::note_interruption;
  sigemptyset(&noted.sa_mask);
  REQUIRE(::sigaction(SIGINT, &noted, &saved) == 0);
  std::thread signal([&fake, command = ::pthread_self()]() {
    if (fake.wait_for("streaming")) {
      ::pthread_kill(command, SIGINT);
    }
  });
  const auto ran = run({"--json", "strategy", "evaluate", "s.toml"});
  signal.join();
  ::sigaction(SIGINT, &saved, nullptr);
  cmdatl::interruption = 0;
  CHECK(ran.exit_ == 130);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() == 2);
  CHECK(reply_of(lines[0]).at("index").as_int64() == 0);
  CHECK(code_of(lines[1]) == "interrupted");
  CHECK(reply_of(lines[1]).at("replays").as_int64() == 1);
  CHECK(all_gone(fake.pids()));
}

TEST_CASE("SIGINT or SIGTERM while atlc reference runs stops it and answers 130 with no child left") {
  const Watchdog watchdog("interrupted reference", 60);
  for (const int sig : {SIGINT, SIGTERM}) {
    INFO(sig);
    const FakeAtlc fake("reference pid\nreference touch running\nreference block\n");
    Spawned cmdatl({"--json", "strategy", "reference"});
    REQUIRE(fake.wait_for("running"));
    cmdatl.signal(sig);
    const int status = cmdatl.wait(20);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 130);
    CHECK(code_of(lines_of(cmdatl.out()).back()) == "interrupted");
    CHECK(all_gone(fake.pids()));
  }
}
#endif
