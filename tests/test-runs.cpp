// The run driver in process: `run_main` compiled with a hand-written program, loader and `make` (no atlc header), each
// run in a forked child with a fixture operator file, its stdout captured, under a watchdog.

#include "./paper-support.hpp"
#include "./run-strategy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace json = boost::json;

namespace {

/**
 * @brief The lines of a text, without their newlines.
 * @param text The text.
 * @return The lines.
 */
std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

/**
 * @brief A fresh run id for each run of a test process.
 * @return The id.
 */
std::string next_run() {
  static int count = 0;
  char text[16];
  std::snprintf(text, sizeof text, "r%012x", ++count);
  return text;
}

/** @brief What a run in a child answered. */
struct RunResult {
  /** @brief The run's id. */
  std::string run_;
  /** @brief The child's exit. */
  int exit_{-1};
  /** @brief Its reply, parsed. */
  json::object reply_;
  /** @brief The events, one object per line. */
  std::vector<json::object> events_;
  /** @brief The run's directory. */
  std::filesystem::path dir_;
  /** @brief What it printed on stderr: the recovery, the loop's report and the finalization. */
  std::string stderr_;
};

/** @brief The operator's file for runs: the default limits with room for a run's orders, and every phase but closed. */
ConfigParts run_parts() {
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"005930\", \"000660\"]\n"
    "position = { \"005930\" = 100, \"000660\" = 20 }\n"
    "order_value = 50000000\n"
    "daily_orders = 50\n";
  return parts;
}

/**
 * @brief Runs `run_main<P>` in a child with the given arguments after `run.id`, in a run directory made as `cmdatl run`
 *   makes it, its stdout in a file.
 * @tparam P The program.
 * @param fixture The operator's file and state root.
 * @param args The arguments after `run.id`.
 * @param before Called with the run's id and directory before the child starts.
 * @param during Called in the parent with the child's pid and its stdout file while the child runs.
 * @return What it answered.
 */
template <typename P>
RunResult run_child(
  const PaperFixture& fixture, const std::vector<std::string>& args,
  const std::function<void(const std::string&, const std::filesystem::path&)>& before = {},
  const std::function<void(pid_t, const std::filesystem::path&)>& during = {}
) {
  RunResult ran;
  ran.run_ = next_run();
  ran.dir_ = fixture.dir() / "state" / "cmdatl" / "runs" / "mock-mock-paper-0001" / ran.run_;
  std::filesystem::create_directories(ran.dir_);
  if (before) {
    before(ran.run_, ran.dir_);
  }
  const auto reply_path = fixture.dir() / (ran.run_ + ".reply");
  const auto stderr_path = fixture.dir() / (ran.run_ + ".stderr");
  // The child flushes stdio when the run ends, so whatever the parent still buffers (Catch2's first lines, when stdout is a pipe) would land in the reply.
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(120);
    const int fd = ::open(reply_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    const int err = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(err, STDERR_FILENO);
    ::close(err);
    std::vector<std::string> owned{"driver", "run.id=" + ran.run_};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned) {
      argv.push_back(arg.data());
    }
    const int code =
      cmdatl::driver::run_main<P>(static_cast<int>(argv.size()), argv.data(), runtest::Load{}, runtest::Make<P>{});
    std::fflush(nullptr);
    ::_exit(code);
  }
  if (during) {
    during(pid, reply_path);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  ran.exit_ = WEXITSTATUS(status);
  ran.stderr_ = slurp(stderr_path);
  // stdout: the events, one per line, then the reply as the last line.
  const auto text = slurp(reply_path);
  INFO("stdout: " << text);
  const auto lines = split_lines(text);
  for (const auto& line : lines) {
    auto object = json::parse(line).as_object();
    if (object.contains("event")) {
      REQUIRE(ran.reply_.empty());
      ran.events_.push_back(std::move(object));
    } else {
      REQUIRE(ran.reply_.empty());
      ran.reply_ = std::move(object);
    }
  }
  return ran;
}

/**
 * @brief Runs `run_main<P>` in a child as `run_child` does, its stdout a pipe the parent holds both ends of while
 *   `during` runs; then the parent reads what is left and parses what it read.
 * @tparam P The program.
 * @param fixture The operator's file and state root.
 * @param args The arguments after `run.id`.
 * @param during Called with the child's pid, the pipe's read end, its write end, and the text read so far.
 * @param merged Whether stderr shares the pipe, as `2>&1` does.
 * @return What it answered, from what the parent read.
 */
template <typename P>
RunResult run_child_piped(
  const PaperFixture& fixture, const std::vector<std::string>& args,
  const std::function<void(pid_t, int, int, std::string&)>& during, bool merged = false
) {
  RunResult ran;
  ran.run_ = next_run();
  ran.dir_ = fixture.dir() / "state" / "cmdatl" / "runs" / "mock-mock-paper-0001" / ran.run_;
  std::filesystem::create_directories(ran.dir_);
  const auto stderr_path = fixture.dir() / (ran.run_ + ".stderr");
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(120);
    ::dup2(fds[1], STDOUT_FILENO);
    if (merged) {
      ::dup2(fds[1], STDERR_FILENO);
    } else {
      const int err = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }
    ::close(fds[0]);
    ::close(fds[1]);
    std::vector<std::string> owned{"driver", "run.id=" + ran.run_};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned) {
      argv.push_back(arg.data());
    }
    const int code =
      cmdatl::driver::run_main<P>(static_cast<int>(argv.size()), argv.data(), runtest::Load{}, runtest::Make<P>{});
    std::fflush(nullptr);
    ::_exit(code);
  }
  std::string text;
  during(pid, fds[0], fds[1], text);
  ::close(fds[1]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  ::close(fds[0]);
  REQUIRE(WIFEXITED(status));
  ran.exit_ = WEXITSTATUS(status);
  if (!merged) {
    ran.stderr_ = slurp(stderr_path);
  }
  for (const auto& line : split_lines(text)) {
    // With stderr merged, its lines (`recovery:`, `loop:`) are not JSON.
    if (merged && (line.empty() || line.front() != '{')) {
      continue;
    }
    auto object = json::parse(line).as_object();
    if (object.contains("event")) {
      ran.events_.push_back(std::move(object));
    } else {
      ran.reply_ = std::move(object);
    }
  }
  return ran;
}

/**
 * @brief Fills a pipe as a blocking writer would: a helper process writes until it blocks, then is killed. (On macOS a
 *   blocking writer grows a pipe to 64 KiB, beyond what non-blocking writes fill.)
 * @param write_end The pipe's write end.
 */
void fill_pipe(int write_end) {
  const pid_t filler = ::fork();
  REQUIRE(filler >= 0);
  if (filler == 0) {
    const std::string chunk(512, ' ');
    while (::write(write_end, chunk.data(), chunk.size()) > 0) {
    }
    ::_exit(0);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  ::kill(filler, SIGKILL);
  ::waitpid(filler, nullptr, 0);
}

/**
 * @brief Reads a pipe until what was read holds a text, at most 60 s; only whole lines are kept.
 * @param fd The read end.
 * @param text What was read; whole lines only.
 * @param want The text.
 */
void read_until(int fd, std::string& text, const std::string& want) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  std::string buffer;
  while (text.find(want) == std::string::npos) {
    REQUIRE(std::chrono::steady_clock::now() < until);
    pollfd in{fd, POLLIN, 0};
    if (::poll(&in, 1, 100) <= 0) {
      continue;
    }
    char chunk[4096];
    const auto n = ::read(fd, chunk, sizeof chunk);
    REQUIRE(n > 0);
    buffer.append(chunk, static_cast<std::size_t>(n));
    const auto last = buffer.rfind('\n');
    if (last != std::string::npos) {
      text += buffer.substr(0, last + 1);
      buffer.erase(0, last + 1);
    }
  }
}

/**
 * @brief The error code of a run's reply, or empty.
 * @param ran The run.
 * @return The code.
 */
std::string code_of(const RunResult& ran) {
  const auto* error = ran.reply_.if_contains("error");
  return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
}

/**
 * @brief How many events of a kind a run wrote.
 * @param ran The run.
 * @param kind The event.
 * @return The count.
 */
std::size_t count_of(const RunResult& ran, const std::string& kind) {
  std::size_t count = 0;
  for (const auto& event : ran.events_) {
    count += event.at("event").as_string() == kind ? 1 : 0;
  }
  return count;
}

/** @brief The arguments every test run gives: one named symbol, limit orders inside the collar, the KRX market. */
std::vector<std::string> spec_args() {
  return {"instrument.x=005930", "orders.type=limit", "orders.offset=0.01", "replay.market=kr"};
}

/**
 * @brief Sets the script and data for a test.
 * @param decisions The decisions.
 * @param days The days of data.
 */
void set_script(std::map<std::size_t, runtest::Decision> decisions, int days = 10) {
  runtest::script() = runtest::Script{};
  runtest::script().decisions_ = std::move(decisions);
  runtest::data() = runtest::Data{};
  runtest::data().days_ = days;
  cmdatl::driver::client_hook() = nullptr;
  cmdatl::driver::closed_hook() = nullptr;
}

/** @brief The mock client of the run in progress, for actions inside a run. */
algotradelib::mock::MockClient*& current_client() {
  static algotradelib::mock::MockClient* client = nullptr;
  return client;
}

/**
 * @brief The events of a kind.
 * @param ran The run.
 * @param kind The event.
 * @return Them, in order.
 */
std::vector<json::object> events_of(const RunResult& ran, const std::string& kind) {
  std::vector<json::object> out;
  for (const auto& event : ran.events_) {
    if (event.at("event").as_string() == kind) {
      out.push_back(event);
    }
  }
  return out;
}

/**
 * @brief Waits until a file holds a text, at most 60 s.
 * @param path The file.
 * @param text The text.
 */
void wait_for_text(const std::filesystem::path& path, const std::string& text) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (slurp(path).find(text) == std::string::npos) {
    REQUIRE(std::chrono::steady_clock::now() < until);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

/** @brief Holds a run at each step while an order rests, until `release()`: the driver's test barrier. */
class HeldOnOrder {
public:
  /**
   * @brief Creates the barrier file and points the driver at it.
   * @param fixture Where the file lives.
   */
  explicit HeldOnOrder(const PaperFixture& fixture)
  : path_(fixture.dir() / "barrier"),
    barrier_("CMDATL_TEST_BARRIER", path_.string()),
    on_order_("CMDATL_TEST_BARRIER_ON_ORDER", "1") {
    spit(path_, "");
  }

  /** @brief Lets the run go on. */
  void release() const {
    std::filesystem::remove(path_);
  }

private:
  std::filesystem::path path_;
  ScopedVariable barrier_;
  ScopedVariable on_order_;
};

/** @brief A broker error of a category, as the mock's scripted faults take it. */
algotradelib::BrokerError broker_error(algotradelib::ErrorCategory category) {
  algotradelib::BrokerError error;
  error.category_ = category;
  error.reached_broker_ = true;
  error.message_ = "scripted";
  return error;
}

/** @brief The operator's file with other limits. */
ConfigParts parts_with(const std::string& extra_limits) {
  auto parts = run_parts();
  parts.limits_ += extra_limits;
  return parts;
}

} // namespace

TEST_CASE("a run trades its script on the mock, its start line first, and ends clean after withdrawing") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}, {5, {0, "1"}}});
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 0);
  CHECK(ran.reply_.at("env").as_string() == "mock");
  CHECK(ran.reply_.at("run").as_string() == ran.run_);
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("stopped").as_string() == "end");
  CHECK(ran.reply_.at("executions").as_int64() == 2);
  CHECK(ran.reply_.at("holdings_at_end").as_object().at("005930").as_string() == "0");
  CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  const auto& refused = ran.reply_.at("refused").as_object();
  CHECK(refused.at("boundary").as_int64() == 0);
  CHECK(refused.at("guard").as_int64() == 0);
  CHECK(refused.at("pricing").as_int64() == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(ran.events_.front().at("event").as_string() == "start");
  CHECK(ran.events_.front().at("pid").is_int64());
  CHECK(ran.events_.back().at("event").as_string() == "end");
  CHECK(count_of(ran, "placed") == 2);
  CHECK(count_of(ran, "filled") == 2);
}

TEST_CASE("a run withdraws an order resting at its end and confirms it before exiting 0") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  // On the last bar, a buy far below the price: it can only rest.
  set_script({{10, {10, "0.96"}}});
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 0);
  CHECK(count_of(ran, "placed") == 1);
  CHECK(count_of(ran, "canceled") == 1);
  CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  const auto journal = slurp(ran.dir_ / "journal.jsonl");
  CHECK(journal.find("\"outcome\":\"confirmed\"") != std::string::npos);
}

TEST_CASE("a run refuses before trading: arguments, session handlers, on_fill, the market, the armed file") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}});
  auto args = spec_args();
  args.push_back("paper.tick=5");
  const auto bad = run_child<runtest::Program>(fixture, args);
  CHECK(bad.exit_ == 2);
  CHECK(code_of(bad) == "usage.bad_value");
  const auto session = run_child<runtest::SessionProgram>(fixture, spec_args());
  CHECK(session.exit_ == 5);
  CHECK(code_of(session) == "program.live_unsupported");
  const auto fill = run_child<runtest::FillProgram>(fixture, spec_args());
  CHECK(fill.exit_ == 5);
  CHECK(code_of(fill) == "program.live_unsupported");
  const auto us =
    run_child<runtest::Program>(fixture, {"instrument.x=005930", "orders.type=limit", "orders.offset=0.01"});
  CHECK(us.exit_ == 2);
  CHECK(code_of(us) == "spec.invalid");
  std::filesystem::remove(fixture.armed());
  const auto unarmed = run_child<runtest::Program>(fixture, spec_args());
  CHECK(unarmed.exit_ == 3);
  CHECK(code_of(unarmed) == "refused.not_armed");
  CHECK(unarmed.events_.empty());
}

TEST_CASE("a run refuses each start condition in order, before anything is sent") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}});
  const auto spec_code = [&](std::vector<std::string> args) {
    const auto ran = run_child<runtest::Program>(fixture, args);
    CHECK(ran.events_.empty());
    CHECK(!std::filesystem::exists(ran.dir_ / "journal.jsonl"));
    return std::make_pair(ran.exit_, code_of(ran));
  };
  auto with = [](std::string extra) {
    auto args = spec_args();
    args.push_back(std::move(extra));
    return args;
  };
  CHECK(spec_code(with("run.until=2024-04-01")) == std::make_pair(2, std::string("usage.bad_value")));
  CHECK(spec_code(with("replay.market=us")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("bars.store=kis/krx")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("replay.shorts=true")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("bars.open_point=true")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("param.bad=1")) == std::make_pair(5, std::string("program.invalid")));
  CHECK(spec_code(with("instrument.y=035720")) == std::make_pair(3, std::string("refused.symbol_not_allowed")));
  CHECK(
    spec_code({"instrument.x=005930", "orders.type=market", "replay.market=kr"}) ==
    std::make_pair(3, std::string("refused.order_type"))
  );
  CHECK(
    spec_code({"instrument.x=005930", "orders.type=limit", "orders.offset=0.06", "replay.market=kr"}) ==
    std::make_pair(3, std::string("refused.collar"))
  );
  runtest::data().refuse_ = true;
  CHECK(spec_code(spec_args()) == std::make_pair(2, std::string("spec.invalid")));
  runtest::data().refuse_ = false;
  runtest::data().interval_ = algotradelib::BarInterval::ONE_MINUTE;
  CHECK(spec_code(spec_args()) == std::make_pair(1, std::string("unsupported.interval")));
  runtest::data().interval_ = algotradelib::BarInterval::ONE_DAY;
  runtest::script().stated_warmup_ = 10001;
  CHECK(spec_code(spec_args()) == std::make_pair(5, std::string("program.live_unsupported")));
  runtest::script().stated_warmup_ = 10000;
  const auto long_warmup = run_child<runtest::Program>(fixture, spec_args());
  CHECK(long_warmup.exit_ == 0);
}

TEST_CASE("a run refuses without an operator file, and a second driver of one run") {
  set_script({{2, {10, "1"}}});
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    std::filesystem::remove(fixture.config());
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran) == "config.missing");
  }
  const PaperFixture fixture(run_parts());
  fixture.arm();
  // Another process holds this run's lock: the driver answers at once, nothing created.
  std::unique_ptr<cmdatl::OwnerLock> held;
  const auto ran =
    run_child<runtest::Program>(fixture, spec_args(), [&held](const std::string&, const std::filesystem::path& dir) {
      held = std::make_unique<cmdatl::OwnerLock>(dir.string());
      REQUIRE(
        held->acquire(
          std::chrono::milliseconds(0),
          [] {
            return false;
          },
          [](const std::string&) {}
        ) == cmdatl::OwnerLock::Outcome::HELD
      );
    });
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran) == "busy.locked");
  CHECK(!std::filesystem::exists(ran.dir_ / "journal.jsonl"));
}

TEST_CASE(
  "the placement boundary refuses market orders, limits outside the collar, and phases the operator did not permit"
) {
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1", true}}, {3, {10, "1.08"}}, {4, {10, "1.04"}}});
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    std::vector<std::string> reasons;
    for (const auto& refused : events_of(ran, "refused")) {
      reasons.push_back(std::string(refused.at("reason").as_string()));
    }
    REQUIRE(reasons.size() >= 2);
    CHECK(reasons[0] == "order_type");
    CHECK(reasons[1] == "collar");
    // The limit 4% above the close is inside the 5% collar: placed.
    CHECK(count_of(ran, "placed") >= 1);
    for (const auto& placed : events_of(ran, "placed")) {
      CHECK(placed.contains("price"));
    }
  }
  {
    // Daily bars stamped 00:00 UTC end at 09:00 KST, in `open`; the operator permits only `post_close`.
    const PaperFixture fixture(parts_with("phases = [\"post_close\"]\n"));
    fixture.arm();
    set_script({{2, {10, "1"}}});
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    // The runner's guard holds the 09:00 order (the same phases); it keeps the target and plans it again later, so
    // the order goes out only once the phase is permitted. The boundary's own phase refusal is the unit case below.
    CHECK(count_of(ran, "placed") >= 1);
    for (const auto& placed : events_of(ran, "placed")) {
      CHECK(std::string(placed.at("at").as_string()).substr(11, 5) >= "15:30");
    }
    CHECK(ran.exit_ == 0);
  }
}

TEST_CASE("the step guard: a disarm, a replaced lock and a signal each stop placing at once and latch") {
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1"}}, {5, {0, "1"}}, {6, {10, "1"}}});
    const auto armed = fixture.armed();
    runtest::script().actions_[5] = [armed] {
      std::filesystem::remove(armed);
    };
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    CHECK(ran.reply_.at("stopped").as_string() == "not_armed");
    CHECK(count_of(ran, "placed") == 1);
    const auto refused = events_of(ran, "refused");
    REQUIRE(!refused.empty());
    CHECK(refused.front().at("reason").as_string() == "not_armed");
    CHECK(ran.exit_ == 0);
  }
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1"}}, {5, {0, "1"}}});
    runtest::script().actions_[5] = [&fixture] {
      const auto dir = fixture.dir() / "state" / "cmdatl" / "runs" / "mock-mock-paper-0001";
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto lock = entry.path() / "lock";
        if (std::filesystem::exists(lock) && !std::filesystem::exists(entry.path() / "reply.done")) {
          std::filesystem::rename(lock, entry.path() / "lock.old");
          spit(lock, "");
        }
      }
    };
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran) == "unknown.lock_lost");
    CHECK(count_of(ran, "placed") == 1);
  }
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1"}}, {5, {0, "1"}}});
    runtest::script().actions_[4] = [] {
      std::raise(SIGTERM);
    };
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    CHECK(ran.reply_.at("stopped").as_string() == "signal");
    CHECK(count_of(ran, "placed") == 1);
  }
}

TEST_CASE("an order list that never answers abandons the loop, an unknown outcome") {
  using Call = algotradelib::mock::Call;
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1"}}});
    cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) {
      client.fail_next(Call::WORKING_ORDERS, broker_error(algotradelib::ErrorCategory::TRANSPORT), 100000);
    };
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran) == "unknown.loop_abandoned");
  }
}

TEST_CASE("a lost acknowledgement, of a buy or a sale, changes nothing in the summary once resolved") {
  using Call = algotradelib::mock::Call;
  const PaperFixture fixture(run_parts());
  fixture.arm();
  const std::map<std::size_t, runtest::Decision> decisions{{2, {10, "1"}}, {6, {0, "1"}}};
  set_script(decisions);
  const auto normal = run_child<runtest::Program>(fixture, spec_args());
  REQUIRE(normal.exit_ == 0);
  REQUIRE(normal.reply_.at("executions").as_int64() == 2);
  const auto same_summary = [&normal](const RunResult& ran) {
    INFO(json::serialize(ran.reply_));
    INFO(ran.stderr_);
    CHECK(ran.exit_ == 0);
    for (
      const char* field :
      {"executions", "commission_paid", "tax_paid", "closing_equity", "holdings_at_end", "owed_at_end"}
    ) {
      CAPTURE(field);
      CHECK(ran.reply_.at(field) == normal.reply_.at(field));
    }
    CHECK(count_of(ran, "filled") == 2);
    CHECK(count_of(ran, "placed") == 2);
  };
  // The buy's acknowledgement lost.
  set_script(decisions);
  cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) {
    client.lose_next_reply(Call::PLACE);
  };
  const auto buy_lost = run_child<runtest::Program>(fixture, spec_args());
  same_summary(buy_lost);
  CHECK(events_of(buy_lost, "placed").front().at("acknowledged") == json::value(false));
  // The sale's acknowledgement lost.
  set_script(decisions);
  cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) {
    current_client() = &client;
  };
  runtest::script().actions_[6] = [] {
    current_client()->lose_next_reply(Call::PLACE);
  };
  same_summary(run_child<runtest::Program>(fixture, spec_args()));
  // An order whose acknowledgement was lost and which then expires: placed, expired, nothing owed.
  set_script({{3, {10, "0.97"}}});
  cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) {
    client.lose_next_reply(Call::PLACE);
  };
  const auto expired = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(expired.reply_));
  INFO(expired.stderr_);
  CHECK(expired.exit_ == 0);
  CHECK(count_of(expired, "expired") >= 1);
  CHECK(expired.reply_.at("executions").as_int64() == 0);
  CHECK(expired.reply_.at("closing_equity").as_string() == "100000000");
}

TEST_CASE("the finalization stage gives up after its rounds when the history stays unreadable") {
  using Call = algotradelib::mock::Call;
  const PaperFixture fixture(run_parts());
  fixture.arm();
  // An unfillable buy on the last bar, withdrawn at the end; then the history fails, so the cancellation is never
  // confirmed.
  set_script({{10, {10, "0.96"}}});
  cmdatl::driver::closed_hook() = [](algotradelib::mock::MockClient& client) {
    client.fail_next(Call::ORDER_HISTORY, broker_error(algotradelib::ErrorCategory::TRANSPORT), 1000);
  };
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  INFO(ran.stderr_);
  CHECK(ran.exit_ == 4);
  CHECK(code_of(ran) == "unknown.owed_at_end");
  CHECK(ran.stderr_.find("finalization: 10 round(s), 1 owed") != std::string::npos);
  CHECK(ran.reply_.at("owed_at_end").as_array().size() == 1);
  CHECK(ran.events_.back().at("event").as_string() == "end");
  CHECK(ran.events_.back().at("result") == ran.reply_.at("result"));
  CHECK(ran.events_.back().at("exit").as_int64() == 4);
}

TEST_CASE("the run's clock never moves past its bound, and a wait at the bound throws") {
  namespace atl = algotradelib;
  auto series = runtest::bars_of("005930", 3, atl::BarInterval::ONE_DAY);
  atl::mock::MarketSettings settings;
  settings.cash_ = atl::Money{atl::Decimal64::parse("1000000"), atl::Currency::krw()};
  settings.start_ = series.bars_.front().timestamp_;
  atl::mock::Market market({series}, settings);
  int steps = 0;
  const auto bound = settings.start_ + std::chrono::hours(1);
  cmdatl::driver::RunClock clock(market, bound, [&steps] {
    ++steps;
  });
  clock.wait_for(std::chrono::minutes(30));
  CHECK(clock.now() == settings.start_ + std::chrono::minutes(30));
  clock.wait_for(std::chrono::hours(2));
  CHECK(clock.now() == bound);
  CHECK(steps == 2);
  CHECK_THROWS_AS(clock.wait_for(std::chrono::minutes(1)), cmdatl::driver::RunBound);
  CHECK(clock.now() == bound);
  CHECK(steps == 2);
}

TEST_CASE("buying power is read and enforced: a buy above the cash is the guard's refusal, never an unread account") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {100, "1"}}});
  runtest::data().cash_ = "1000000";
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  INFO(ran.stderr_);
  CHECK(ran.exit_ == 0);
  CHECK(count_of(ran, "placed") == 0);
  CHECK(ran.stderr_.find("refused by the guard") != std::string::npos);
  CHECK(ran.stderr_.find("refused by the guard, ") != std::string::npos);
  CHECK(ran.stderr_.find(" 0 refused by the guard") == std::string::npos);
  CHECK(ran.stderr_.find("buying_power_unread") == std::string::npos);
}

TEST_CASE("the summary's cash and equity follow the fills, commission included, and expiries are their own events") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}, {4, {20, "0.97"}}, {7, {0, "1"}}});
  runtest::data().commission_ = "0.001";
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 0);
  CHECK(count_of(ran, "expired") >= 1);
  auto cash = algotradelib::Decimal64::parse("100000000");
  for (const auto& filled : events_of(ran, "filled")) {
    const auto quantity = algotradelib::Decimal64::parse(std::string(filled.at("quantity").as_string()));
    const auto price = algotradelib::Decimal64::parse(std::string(filled.at("price").as_string()));
    const auto amount = *algotradelib::decimal_multiply(quantity, price);
    cash = filled.at("side").as_string() == "buy" ?
      *algotradelib::decimal_subtract(cash, amount) :
      *algotradelib::decimal_add(cash, amount);
    cash = *algotradelib::decimal_subtract(
      cash, algotradelib::Decimal64::parse(std::string(filled.at("commission").as_string()))
    );
  }
  CHECK(ran.reply_.at("holdings_at_end").as_object().at("005930").as_string() == "0");
  CHECK(ran.reply_.at("closing_equity").as_string() == cash.str());
  CHECK(ran.reply_.at("commission_paid").as_string() != "0");
}

TEST_CASE(
  "an exception after the start line is an unknown outcome, with the reply written before the lock is released"
) {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}});
  runtest::script().actions_[4] = [] {
    throw std::runtime_error("the program failed");
  };
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 4);
  CHECK(code_of(ran) == "unknown.internal");
  CHECK(ran.events_.front().at("event").as_string() == "start");
}

TEST_CASE("the boundary's own checks, one by one, and the phase from an unreadable calendar") {
  namespace atl = algotradelib;
  using cmdatl::placement_refusal;
  const ScratchDir dir;
  spit(dir.path() / "armed", "");
  cmdatl::OwnerLock lock(dir.path().string());
  REQUIRE(
    lock.acquire(
      std::chrono::milliseconds(0),
      [] {
        return false;
      },
      [](const std::string&) {}
    ) == cmdatl::OwnerLock::Outcome::HELD
  );
  cmdatl::PlacementPolicy policy;
  policy.armed_ = (dir.path() / "armed").string();
  policy.lock_ = &lock;
  policy.collar_ = 0.05;
  policy.phases_ = {atl::strategy::MarketPhase::OPEN};
  atl::OrderRequest sell;
  sell.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "005930"};
  sell.side_ = atl::OrderSide::SELL;
  sell.type_ = atl::OrderType::LIMIT;
  sell.quantity_ = atl::Decimal64::parse("10");
  sell.price_ = atl::Decimal64::parse("78000");
  const auto mark = atl::Decimal64::parse("78000");
  const auto open = atl::strategy::MarketPhase::OPEN;
  CHECK(!placement_refusal(policy, false, sell, mark, open));
  // A reduction outside the permitted phases is refused, though the library's guard would admit it.
  CHECK(placement_refusal(policy, false, sell, mark, atl::strategy::MarketPhase::POST_CLOSE) == std::string("phase"));
  CHECK(placement_refusal(policy, false, sell, mark, atl::strategy::MarketPhase::UNKNOWN) == std::string("phase"));
  CHECK(placement_refusal(policy, true, sell, mark, open) == std::string("closed"));
  CHECK(placement_refusal(policy, false, sell, std::nullopt, open) == std::string("no_price"));
  CHECK(placement_refusal(policy, false, sell, atl::Decimal64::parse("0"), open) == std::string("no_price"));
  // At the collar's edge, inside; one won past it, outside.
  sell.price_ = atl::Decimal64::parse("81900");
  CHECK(!placement_refusal(policy, false, sell, mark, open));
  sell.price_ = atl::Decimal64::parse("81901");
  CHECK(placement_refusal(policy, false, sell, mark, open) == std::string("collar"));
  sell.price_ = atl::Decimal64::parse("78000");
  sell.type_ = atl::OrderType::MARKET;
  sell.price_.reset();
  CHECK(placement_refusal(policy, false, sell, mark, open) == std::string("order_type"));
  sell.type_ = atl::OrderType::LIMIT;
  sell.price_ = atl::Decimal64::parse("78000");
  std::filesystem::remove(dir.path() / "armed");
  CHECK(placement_refusal(policy, false, sell, mark, open) == std::string("not_armed"));
  spit(dir.path() / "armed", "");

  // The phase through a session whose calendar cannot be read: unknown, so every placement is refused.
  auto series = runtest::bars_of("005930", 5, atl::BarInterval::ONE_DAY);
  atl::mock::MarketSettings settings;
  settings.cash_ = atl::Money{atl::Decimal64::parse("100000000"), atl::Currency::krw()};
  settings.start_ = series.bars_.front().timestamp_;
  atl::mock::Market market({series}, settings);
  auto client = std::make_shared<atl::mock::MockClient>(market);
  atl::BlockingSession<atl::mock::MockClient> inner(client, atl::mock::MockAccount{});
  std::vector<cmdatl::PlacementEvent> seen;
  cmdatl::GuardedSession<atl::BlockingSession<atl::mock::MockClient>> session(
    inner, policy,
    [&market] {
      return market.now();
    },
    [&seen](const cmdatl::PlacementEvent& e) {
      seen.push_back(e);
    }
  );
  CHECK(session.phase_now() == open);
  client->fail_next(atl::mock::Call::CALENDAR, broker_error(atl::ErrorCategory::TRANSPORT), 2);
  CHECK(session.phase_now() == atl::strategy::MarketPhase::UNKNOWN);
  const auto refused = session.place(sell);
  REQUIRE(!refused);
  CHECK(!refused.error().reached_broker_);
  CHECK(refused.error().broker_code_ == "phase");
  CHECK(market.order_history({}).orders_.empty());
  const auto placed = session.place(sell);
  CHECK(!placed); // nothing held: a sell with no holding is the broker's refusal, reached
  REQUIRE(seen.size() == 2);
  CHECK(seen[1].reason_ == "insufficient_holdings");
  // Closed: nothing reaches the broker any more.
  session.close();
  const auto closed = session.place(sell);
  REQUIRE(!closed);
  CHECK(!closed.error().reached_broker_);
  CHECK(closed.error().broker_code_ == "closed");
  CHECK(market.order_history({}).orders_.empty());
}

/** @brief A broker session whose placements and cancellations throw, as a call that fails midway does. */
struct ThrowingSession {
  /** @brief The session it stands in for. */
  using Inner = algotradelib::BlockingSession<algotradelib::mock::MockClient>;
  /** @brief The broker's capabilities. */
  using Caps = Inner::Caps;
  /** @brief The broker's tag. */
  using BrokerTag = Inner::BrokerTag;
  /** @brief The broker's traits. */
  using Traits = Inner::Traits;
  /** @brief An order reference. */
  using Ref = Inner::Ref;
  /** @brief What a placement or cancellation answers. */
  using Answer = decltype(std::declval<Inner&>().place(algotradelib::OrderRequest{}));

  /**
   * @brief Throws.
   * @param order The order.
   * @return Never.
   */
  Answer place(const algotradelib::OrderRequest& order) {
    (void)order;
    throw std::runtime_error("the connection dropped during the call");
  }

  /**
   * @brief Throws.
   * @param ref The order.
   * @param instruction What to cancel.
   * @return Never.
   */
  Answer cancel(const Ref& ref, const algotradelib::AmendInstruction& instruction) {
    (void)ref;
    (void)instruction;
    throw std::runtime_error("the connection dropped during the call");
  }
};

TEST_CASE("the boundary checks the lock before a cancellation and counts each send until it is known not to arrive") {
  namespace atl = algotradelib;
  const ScratchDir dir;
  spit(dir.path() / "armed", "");
  cmdatl::OwnerLock lock(dir.path().string());
  REQUIRE(
    lock.acquire(
      std::chrono::milliseconds(0),
      [] {
        return false;
      },
      [](const std::string&) {}
    ) == cmdatl::OwnerLock::Outcome::HELD
  );
  cmdatl::PlacementPolicy policy;
  policy.armed_ = (dir.path() / "armed").string();
  policy.lock_ = &lock;
  policy.collar_ = 0.05;
  policy.phases_ = {atl::strategy::MarketPhase::OPEN};
  const auto series = runtest::bars_of("005930", 5, atl::BarInterval::ONE_DAY);
  atl::mock::MarketSettings settings;
  settings.cash_ = atl::Money{atl::Decimal64::parse("100000000"), atl::Currency::krw()};
  settings.start_ = series.bars_.front().timestamp_;
  atl::mock::Market market({series}, settings);
  auto client = std::make_shared<atl::mock::MockClient>(market);
  atl::BlockingSession<atl::mock::MockClient> inner(client, atl::mock::MockAccount{});
  const auto now = [&market] {
    return market.now();
  };
  cmdatl::GuardedSession<atl::BlockingSession<atl::mock::MockClient>> session(inner, policy, now, {});
  const auto quote = inner.quote(series.instrument_);
  REQUIRE(quote);
  atl::OrderRequest buy;
  buy.instrument_ = series.instrument_;
  buy.side_ = atl::OrderSide::BUY;
  buy.type_ = atl::OrderType::LIMIT;
  buy.quantity_ = atl::Decimal64::parse("1");
  buy.price_ = quote.value().last_price_;
  atl::AmendInstruction whole;
  whole.all_remaining_ = true;

  // Forwarded and accepted: counted.
  const auto first = session.place(buy);
  REQUIRE(first);
  CHECK(session.sends() == 1);
  const auto second = session.place(buy);
  REQUIRE(second);
  CHECK(session.sends() == 2);
  // Refused here, before forwarding: not counted.
  std::filesystem::remove(dir.path() / "armed");
  CHECK(!session.place(buy));
  CHECK(session.sends() == 2);
  spit(dir.path() / "armed", "");
  // Forwarded, and answered that it did not reach the broker: given back.
  atl::BrokerError unsent;
  unsent.category_ = atl::ErrorCategory::TRANSPORT;
  unsent.reached_broker_ = false;
  unsent.message_ = "scripted";
  client->fail_next(atl::mock::Call::PLACE, unsent);
  const auto refused = session.place(buy);
  REQUIRE(!refused);
  CHECK(!refused.error().reached_broker_);
  CHECK(session.sends() == 2);
  client->fail_next(atl::mock::Call::CANCEL, unsent);
  CHECK(!session.cancel(first.value().ref_, whole));
  CHECK(session.sends() == 2);
  // Forwarded, and answered by an error that reached the broker: stays counted.
  client->fail_next(atl::mock::Call::PLACE, broker_error(atl::ErrorCategory::TRANSPORT));
  CHECK(!session.place(buy));
  CHECK(session.sends() == 3);
  // A call that throws stays counted, placement and cancellation alike.
  ThrowingSession throwing;
  cmdatl::GuardedSession<ThrowingSession, atl::BlockingSession<atl::mock::MockClient>> thrown(
    throwing, inner, policy, now, {}
  );
  CHECK_THROWS(thrown.place(buy));
  CHECK(thrown.sends() == 1);
  CHECK_THROWS(thrown.cancel(first.value().ref_, whole));
  CHECK(thrown.sends() == 2);
  // Disarmed, a cancellation still goes out, and counts.
  std::filesystem::remove(dir.path() / "armed");
  REQUIRE(session.cancel(first.value().ref_, whole));
  CHECK(session.sends() == 4);
  CHECK(session.cancelled(first.value().ref_.primary_));
  REQUIRE(market.working_orders().orders_.size() == 1);
  // With the lock replaced, a cancellation is refused before the broker sees it, and does not count.
  std::filesystem::remove(dir.path() / "lock");
  spit(dir.path() / "lock", "");
  const auto lost = session.cancel(second.value().ref_, whole);
  REQUIRE(!lost);
  CHECK(!lost.error().reached_broker_);
  CHECK(lost.error().broker_code_ == "lock_lost");
  CHECK(session.sends() == 4);
  CHECK(!session.cancelled(second.value().ref_.primary_));
  CHECK(market.working_orders().orders_.size() == 1);

  // A boundary given no lock cancels nothing.
  auto unlocked = policy;
  unlocked.lock_ = nullptr;
  cmdatl::GuardedSession<atl::BlockingSession<atl::mock::MockClient>> no_lock(inner, unlocked, now, {});
  const auto unlocked_cancel = no_lock.cancel(second.value().ref_, whole);
  REQUIRE(!unlocked_cancel);
  CHECK(!unlocked_cancel.error().reached_broker_);
  CHECK(unlocked_cancel.error().broker_code_ == "lock_lost");
  CHECK(no_lock.sends() == 0);
  CHECK(market.working_orders().orders_.size() == 1);
}
#endif

#if !defined(_WIN32)
TEST_CASE("SIGINT, SIGTERM and SIGHUP each withdraw: the resting order cancelled and confirmed, the run clean") {
  for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
    INFO("signal " << sig);
    const PaperFixture fixture(run_parts());
    fixture.arm();
    const HeldOnOrder held(fixture);
    // A buy below the price at bar 4 rests; the run is held there and signalled.
    set_script({{4, {10, "0.97"}}});
    const auto ran =
      run_child<runtest::Program>(fixture, spec_args(), {}, [&held, sig](pid_t pid, const std::filesystem::path& out) {
        wait_for_text(out, "\"event\":\"placed\"");
        ::kill(pid, sig);
        held.release();
      });
    INFO(json::serialize(ran.reply_));
    CHECK(ran.exit_ == 0);
    CHECK(ran.reply_.at("stopped").as_string() == "signal");
    CHECK(ran.reply_.at("outcome").as_string() == "clean");
    CHECK(count_of(ran, "placed") == 1);
    CHECK(count_of(ran, "canceled") == 1);
    const auto stops = events_of(ran, "stop");
    REQUIRE(!stops.empty());
    CHECK(stops.front().at("intent").as_string() == "withdraw");
    CHECK(ran.reply_.at("owed_at_end").as_array().empty());
    const auto journal = slurp(ran.dir_ / "journal.jsonl");
    CHECK(journal.find("\"action\":\"cancel\"") != std::string::npos);
    CHECK(journal.find("\"outcome\":\"confirmed\"") != std::string::npos);
  }
}

TEST_CASE("a run whose lock was replaced before a signal leaves its order resting and its reply names the lock") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  const HeldOnOrder held(fixture);
  set_script({{4, {10, "0.97"}}});
  // The order rests, the run's lock is replaced and the run signalled: the withdrawal's cancellation is refused.
  const auto ran =
    run_child<runtest::Program>(fixture, spec_args(), {}, [&](pid_t pid, const std::filesystem::path& out) {
      wait_for_text(out, "\"event\":\"placed\"");
      const auto dir = fixture.dir() / "state" / "cmdatl" / "runs" / "mock-mock-paper-0001";
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (std::filesystem::exists(entry.path() / "lock")) {
          std::filesystem::rename(entry.path() / "lock", entry.path() / "lock.old");
          spit(entry.path() / "lock", "");
        }
      }
      ::kill(pid, SIGTERM);
      held.release();
    });
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 4);
  CHECK(code_of(ran) == "unknown.closing_incomplete");
  CHECK(ran.reply_.at("error").at("message").as_string() == "the run's lock file was replaced during the run");
  CHECK(count_of(ran, "placed") == 1);
  CHECK(count_of(ran, "canceled") == 0);
}

TEST_CASE("a placement after a signal is refused before sending, counted apart, and the run stays clean") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  // The signal arrives at bar 4, before that bar's decision: the guard has not yet run, the boundary refuses.
  set_script({{4, {10, "0.97"}}});
  runtest::script().actions_[4] = [] {
    std::raise(SIGINT);
  };
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 0);
  CHECK(count_of(ran, "placed") == 0);
  const auto refused = events_of(ran, "refused");
  REQUIRE(refused.size() >= 1);
  CHECK(refused.front().at("reason").as_string() == "signal");
  CHECK(ran.reply_.at("refused").as_object().at("signal").as_int64() >= 1);
  CHECK(ran.reply_.at("refused").as_object().at("boundary").as_int64() == 0);
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("stopped").as_string() == "signal");
}

TEST_CASE("a closed output withdraws the run: the resting order is cancelled and confirmed although nobody reads") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  const HeldOnOrder held(fixture);
  set_script({{4, {10, "0.97"}}});
  // Once the order rests, the reader goes away, as when the caller's pipe closes: the next event (the heartbeat) fails.
  const auto ran = run_child_piped<runtest::Program>(
    fixture, spec_args(), [&held](pid_t, int read_end, int write_end, std::string& text) {
      read_until(read_end, text, "\"event\":\"placed\"");
      ::close(write_end);
      ::close(read_end);
      // Past the heartbeat's second, so the step after the hold writes one, which fails while the order rests.
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      held.release();
    }
  );
  CHECK(ran.exit_ == 0);
  CHECK(ran.reply_.empty());
  CHECK(count_of(ran, "placed") == 1);
  const auto journal = slurp(ran.dir_ / "journal.jsonl");
  CHECK(journal.find("\"action\":\"cancel\"") != std::string::npos);
  CHECK(journal.find("\"outcome\":\"confirmed\"") != std::string::npos);
  CHECK(ran.stderr_.find(", 0 owed") != std::string::npos);
}

TEST_CASE("stopping, a line longer than the room a nearly full output polls with gives up instead of blocking") {
  // A pipe that polls writable (at least PIPE_BUF free) without room for the whole line: once a stop is noted, the
  // writer takes what fits a PIPE_BUF at a time and then gives up, where one whole write would sleep until read.
  for (const std::size_t freed : {std::size_t{0}, std::size_t{700}, std::size_t{1500}}) {
    INFO("bytes freed: " << freed);
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      ::alarm(20);
      ::dup2(fds[1], STDOUT_FILENO);
      const int flags = ::fcntl(STDOUT_FILENO, F_GETFL);
      ::fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
      const std::string filler(4096, ' ');
      while (::write(STDOUT_FILENO, filler.data(), filler.size()) > 0) {
      }
      while (::write(STDOUT_FILENO, filler.data(), 1) > 0) {
      }
      ::fcntl(STDOUT_FILENO, F_SETFL, flags);
      std::vector<char> drained(freed);
      std::size_t got = 0;
      while (got < freed) {
        const auto n = ::read(fds[0], drained.data() + got, freed - got);
        if (n <= 0) {
          ::_exit(3);
        }
        got += static_cast<std::size_t>(n);
      }
      cmdatl::interruption = 1;
      const bool written = cmdatl::driver::write_line(std::string(2000, 'x') + "\n");
      ::_exit(written ? 1 : 0);
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    ::close(fds[0]);
    ::close(fds[1]);
    // An exit, not the alarm: the write gave up.
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }
}

TEST_CASE("a signal stops a run whose output nobody reads: it gives up writing, withdraws and exits") {
  // A full pipe, and a nearly full one: room for a few hundred bytes, which polls writable but holds no whole line;
  // each with stderr apart, and sharing the pipe as `2>&1` does.
  for (const auto& [freed, merged] : std::vector<std::pair<std::size_t, bool>>{{0, false}, {700, false}, {0, true}}) {
    INFO("bytes freed: " << freed << ", stderr merged: " << merged);
    const PaperFixture fixture(run_parts());
    fixture.arm();
    const HeldOnOrder held(fixture);
    set_script({{4, {10, "0.97"}}});
    // Once the order rests, the reader stops reading and the pipe is filled, as a caller waiting on the process
    // without reading its output. Then SIGTERM.
    const auto ran = run_child_piped<runtest::Program>(
      fixture, spec_args(),
      [&held, freed](pid_t pid, int read_end, int write_end, std::string& text) {
        read_until(read_end, text, "\"event\":\"placed\"");
        fill_pipe(write_end);
        std::vector<char> drained(freed);
        std::size_t got = 0;
        while (got < freed) {
          const auto n = ::read(read_end, drained.data() + got, freed - got);
          REQUIRE(n > 0);
          got += static_cast<std::size_t>(n);
        }
        ::kill(pid, SIGTERM);
        held.release();
      },
      merged
    );
    // It ended by itself, though nobody read (a hang would end in the child's alarm, not an exit), after withdrawing.
    CHECK(ran.exit_ == 0);
    const auto journal = slurp(ran.dir_ / "journal.jsonl");
    CHECK(journal.find("\"action\":\"cancel\"") != std::string::npos);
    CHECK(journal.find("\"outcome\":\"confirmed\"") != std::string::npos);
    if (!merged) {
      CHECK(ran.stderr_.find(", 0 owed") != std::string::npos);
    }
  }
}
#endif
