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
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace support;

namespace {

/**
 * @brief An `atlc spec --json` reply.
 * @param status `ok` or `error`.
 * @param errors The errors, a JSON array.
 * @param warnings The warnings, a JSON array.
 * @return The reply.
 */
std::string envelope(const std::string& status, const std::string& errors, const std::string& warnings = "[]") {
  return R"({"atlc_spec":1,"status":")" + status +
    R"(","atlc":"0.0.0-9000","program":null,"build":null,"store":null,"orders":null,"args":null,"warnings":)" +
    warnings + R"(,"errors":)" + errors + "}";
}

/** @brief A program error as `atlc spec --json` gives it. */
const std::string PROGRAM_ERROR =
  R"([{"severity":"error","code":"undefined-name","message":"`lookbak` is not defined","file":"/x/dip.atls",)"
  R"("line":9,"origin":"program"}])";

/** @brief A spec error as `atlc spec --json` gives it. */
const std::string SPEC_ERROR =
  R"([{"severity":"error","code":"spec","message":"unknown key `dta`","file":"/x/dip.toml","line":3,"origin":"file"}])";

/** @brief One backtest against the fake: what atlc does, and what cmdatl must answer. */
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
};

/**
 * @brief Every case of the backtest's mapping.
 * @return The cases.
 */
std::vector<Case> cases() {
  const auto spec_ok = "spec out " + SPEC_OK + "\n";
  return {
    {"a report", spec_ok + "run err atlc: building\nrun out " + REPORT + "\n", 0, ""},
    {"program errors", "spec out " + envelope("error", PROGRAM_ERROR) + "\nspec exit 1\n", 5, "program.invalid"},
    {"spec errors", "spec out " + envelope("error", SPEC_ERROR) + "\nspec exit 2\n", 2, "spec.invalid"},
    {"an atlc without spec", "spec err usage: atlc <command> [arguments]\nspec err \nspec err commands:\nspec exit 2\n",
      1, "atlc.unsupported"},
    {"exit 2 without JSON or usage", "spec err atlc spec: something else\nspec exit 2\n", 1, "atlc.failed"},
    {"exit 2, usage, and a broken object", "spec out {\"atlc_spec\":1\nspec err usage: atlc\nspec exit 2\n", 1,
      "atlc.failed"},
    {"malformed JSON", "spec out {not json\n", 1, "atlc.failed"},
    {"no reply", "spec exit 0\n", 1, "atlc.failed"},
    {"two objects", spec_ok + spec_ok, 1, "atlc.failed"},
    {"an array", "spec out []\n", 1, "atlc.failed"},
    {"ok with errors", "spec out " + envelope("ok", PROGRAM_ERROR) + "\n", 1, "atlc.failed"},
    {"error without errors", "spec out " + envelope("error", "[]") + "\nspec exit 2\n", 1, "atlc.failed"},
    {"error on exit 0", "spec out " + envelope("error", PROGRAM_ERROR) + "\n", 1, "atlc.failed"},
    {"ok on exit 1", spec_ok + "spec exit 1\n", 1, "atlc.failed"},
    {"no warnings", "spec out {\"atlc_spec\":1,\"status\":\"ok\",\"errors\":[]}\n", 1, "atlc.failed"},
    {"warnings not an array", "spec out {\"atlc_spec\":1,\"status\":\"ok\",\"warnings\":{},\"errors\":[]}\n", 1,
      "atlc.failed"},
    {"a diagnostic without a code",
      "spec out " + envelope("error", R"([{"severity":"error","message":"x"}])") + "\nspec exit 2\n", 1, "atlc.failed"},
    {"a status that is not a string", "spec out {\"atlc_spec\":1,\"status\":true,\"warnings\":[],\"errors\":[]}\n", 1,
      "atlc.failed"},
    {"another format", "spec out {\"atlc_spec\":2,\"status\":\"ok\",\"warnings\":[],\"errors\":[]}\n", 1,
      "atlc.failed"},
    {"spec killed by a signal", "spec raise 9\n", 1, "atlc.failed"},
    {"spec exit 101", "spec err atlc: internal error\nspec exit 101\n", 1, "atlc.failed"},
    {"the program broke between the calls", spec_ok + "run err error[undefined-name]: `x` is not defined\nrun exit 1\n",
      5, "program.invalid"},
    {"the spec broke between the calls", spec_ok + "run err atlc run: unknown key `dta`\nrun exit 2\n", 2,
      "spec.invalid"},
    {"a build failure",
      spec_ok +
        "run err atlc: building\nrun err atlc run: cmake failed\nrun err (the build's log is /c/build.log)\n"
        "run exit 3\n",
      1, "build.failed"},
    {"a replay failure", spec_ok + "run err error: lookback=1 is below its lower bound 2\nrun exit 4\n", 1,
      "replay.failed"},
    {"run exit 101", spec_ok + "run exit 101\n", 1, "atlc.failed"},
    {"run killed by a signal", spec_ok + "run raise 9\n", 1, "atlc.failed"},
    {"run exit 130, an exit code and not a signal", spec_ok + "run exit 130\n", 1, "atlc.failed"},
    {"run exit 0 without a report", spec_ok + "run exit 0\n", 1, "atlc.failed"},
    {"run exit 0 with a broken report", spec_ok + "run out {\"bars\":\n", 1, "atlc.failed"},
    {"run exit 0 with two reports", spec_ok + "run out " + REPORT + "\nrun out " + REPORT + "\n", 1, "atlc.failed"},
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
  if (format == "json") {
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 1);
    const auto reply = reply_of(lines.front());
    CHECK(reply.contains("error") == failed);
    if (failed) {
      CHECK(code_of(lines.front()) == expected.code_);
    }
    CHECK(lines_of(ran.err_).front() == "cmdatl: replaying s.toml");
  } else if (format == "jsonl") {
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() >= 2);
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
      const auto event = reply_of(lines[i]);
      CHECK(event.at("event").as_string() == "progress");
      CHECK(event.at("message").is_string());
    }
    CHECK(reply_of(lines.front()).at("message").as_string() == "cmdatl: replaying s.toml");
    CHECK(reply_of(lines.back()).contains("error") == failed);
    if (failed) {
      CHECK(code_of(lines.back()) == expected.code_);
    }
    CHECK(ran.err_.empty());
  } else {
    CHECK(lines_of(ran.err_).front() == "cmdatl: replaying s.toml");
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

TEST_CASE("every answer of atlc spec and atlc run maps to its reply in json and jsonl and text") {
  const Watchdog watchdog("the mapping", 300);
  for (const auto& expected : cases()) {
    for (const auto* format : {"json", "jsonl", "text"}) {
      const FakeAtlc fake(expected.script_, expected.version_);
      check_form(run({"--format", format, "strategy", "backtest", "s.toml"}), format, expected);
    }
  }
}

TEST_CASE("a failure carries atlc's diagnostics or its last stderr lines and names a signal") {
  const Watchdog watchdog("the failure details", 60);
  {
    const FakeAtlc fake("spec out " + envelope("error", PROGRAM_ERROR) + "\nspec exit 1\n");
    const auto reply = reply_of(run({"strategy", "backtest", "s.toml"}).out_);
    const auto& error = reply.at("errors").as_array().at(0).as_object();
    CHECK(error.at("line").as_int64() == 9);
    CHECK(error.at("file").as_string() == "/x/dip.atls");
    CHECK(reply.at("error").as_object().at("message").as_string() == "`lookbak` is not defined");
  }
  {
    const FakeAtlc fake(
      "spec out " + SPEC_OK +
      "\nrun err atlc: building\nrun err atlc run: cmake failed\n"
      "run err (the build's log is /c/build.log)\nrun exit 3\n"
    );
    const auto reply = reply_of(run({"strategy", "backtest", "s.toml"}).out_);
    CHECK(reply.at("stderr").as_array().size() == 3);
    CHECK(reply.at("error").as_object().at("message").as_string().find("/c/build.log") != std::string::npos);
  }
  {
    const FakeAtlc fake("spec out " + SPEC_OK + "\nrun err error[x]: one\nrun err   two\nrun exit 1\n");
    const auto reply = reply_of(run({"strategy", "backtest", "s.toml"}).out_);
    CHECK(reply.at("error").as_object().at("message").as_string() == "error[x]: one\n  two");
  }
  {
    const FakeAtlc fake("spec out " + SPEC_OK + "\nrun raise 9\n");
    const auto reply = reply_of(run({"strategy", "backtest", "s.toml"}).out_);
    CHECK(reply.at("error").as_object().at("message").as_string().find("signal 9") != std::string::npos);
  }
  {
    const FakeAtlc fake("spec out " + SPEC_OK + "\nrun exit 130\n");
    const auto ran = run({"strategy", "backtest", "s.toml"});
    CHECK(ran.exit_ == 1);
    CHECK(
      reply_of(ran.out_).at("error").as_object().at("message").as_string().find("exited with 130") != std::string::npos
    );
  }
}

TEST_CASE("the backtest passes -P to both calls and an absolute --fills to the run and replies with them") {
  const Watchdog watchdog("the arguments", 60);
  const FakeAtlc fake("spec args\nspec out " + SPEC_OK + "\nrun args\nrun out " + REPORT + "\n");
  const InDir here(fake.dir());
  const auto ran = run({"strategy", "backtest", "s.toml", "-P", "a=1", "-P", "b=2", "--fills", "out/fills.csv"});
  REQUIRE(ran.exit_ == 0);
  const auto spec = (fake.dir() / "s.toml").string();
  const auto fills = (fake.dir() / "out/fills.csv").string();
  CHECK(fake.args("spec") == std::vector<std::string>{spec, "-P", "a=1", "-P", "b=2", "--json"});
  CHECK(fake.args("run") == std::vector<std::string>{spec, "-P", "a=1", "-P", "b=2", "--fills", fills});
  const auto reply = reply_of(ran.out_);
  CHECK(reply.at("spec").as_string() == spec);
  CHECK(reply.at("fills").as_string() == fills);
  CHECK(reply.at("params_given") == boost::json::value(boost::json::array{"a=1", "b=2"}));
  CHECK(reply.at("atlc").as_string() == "0.0.0-9000");
  // Nothing in the reply comes from the spec check: a spec changed between the calls cannot be misdescribed.
  std::set<std::string> keys;
  for (const auto& entry : reply) {
    keys.insert(std::string(entry.key()));
  }
  CHECK(keys == std::set<std::string>{"env", "report", "spec", "params_given", "atlc", "fills"});
  CHECK(reply.at("report") == boost::json::parse(REPORT));
}

TEST_CASE("--fields and --value select from the final reply only and never from progress") {
  const Watchdog watchdog("the selections", 60);
  const auto script = "spec out " + SPEC_OK + "\nrun err atlc: building\nrun out " + REPORT + "\n";
  {
    const FakeAtlc fake(script);
    const auto ran = run({"--format", "jsonl", "--fields", "report.executions", "strategy", "backtest", "s.toml"});
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 3);
    CHECK(reply_of(lines[1]).at("message").as_string() == "atlc: building");
    CHECK(!reply_of(lines[1]).contains("warning"));
    const auto final_reply = reply_of(lines[2]);
    CHECK(final_reply.at("report").as_object().size() == 1);
    CHECK(!final_reply.contains("warning"));
  }
  for (const auto* format : {"json", "jsonl", "text"}) {
    INFO(format);
    const FakeAtlc fake(script);
    const auto ran = run({"--format", format, "--value", "report.executions", "strategy", "backtest", "s.toml"});
    CHECK(ran.exit_ == 0);
    CHECK(lines_of(ran.out_).back() == "4");
  }
  {
    // A failure is written whole, whatever was selected.
    const FakeAtlc fake("spec out " + SPEC_OK + "\nrun exit 4\n");
    const auto ran = run({"--fields", "report.executions", "strategy", "backtest", "s.toml"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "replay.failed");
  }
}

TEST_CASE("atlc's stderr reaches the reader as progress while atlc still runs") {
  const Watchdog watchdog("live progress", 60);
  const FakeAtlc fake(
    "spec out " + SPEC_OK + "\nrun err atlc: building\nrun sleep 300\nrun touch done\nrun out " + REPORT + "\n"
  );
  MarkWatcher watcher(fake, "atlc: building", "done");
  std::ostream err(&watcher);
  std::ostringstream out;
  CHECK(cmdatl::run({"--format", "json", "strategy", "backtest", "s.toml"}, out, err, false) == 0);
  CHECK(watcher.seen_);
  CHECK(!watcher.marked_when_seen_);
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

TEST_CASE("two strategy new at once: one wins and the other is refused and no file is truncated") {
  const Watchdog watchdog("the race", 120);
  const ScratchDir dir;
  for (int round = 0; round < 20; ++round) {
    const auto name = "race" + std::to_string(round);
    Spawned first({"strategy", "new", name, "--dir", dir.path().string()});
    Spawned second({"strategy", "new", name, "--dir", dir.path().string()});
    const int a = first.wait();
    const int b = second.wait();
    REQUIRE(WIFEXITED(a));
    REQUIRE(WIFEXITED(b));
    INFO("round " << round);
    CHECK(std::multiset<int>{WEXITSTATUS(a), WEXITSTATUS(b)} == std::multiset<int>{0, 1});
    const auto& loser = WEXITSTATUS(a) == 1 ? first : second;
    CHECK(code_of(loser.out()) == "strategy.exists");
    CHECK(slurp(dir.path() / (name + ".atls")) == template_text("mean-reversion"));
    CHECK(slurp(dir.path() / (name + ".toml")).find("strategy = \"" + name + ".atls\"") != std::string::npos);
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
#endif

TEST_CASE("strategy reference returns atlc reference's text unchanged") {
  {
    const FakeAtlc fake("reference args\nreference out # The atls language reference\n");
    const auto ran = run({"--json", "strategy", "reference"});
    INFO(ran.out_);
    CHECK(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("text").as_string() == "# The atls language reference\n");
    CHECK(fake.args("reference").empty());
  }
  {
    const FakeAtlc fake("reference args\nreference out # Built-in functions\n");
    CHECK(run({"--format", "text", "strategy", "reference", "builtins"}).out_ == "# Built-in functions\n");
    CHECK(fake.args("reference") == std::vector<std::string>{"builtins"});
  }
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
