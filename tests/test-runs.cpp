// The run driver in process: `run_main` compiled with a hand-written program, loader and `make` (no atlc header), each
// run in a forked child with a fixture operator file, its stdout captured, under a watchdog.

#include "./paper-support.hpp"
#include "./run-strategy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <atomic>
#include <csignal>
#include <functional>
#include <map>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
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
 * @brief Runs `run_main<P>` in a child with the given arguments after `run.id`, in a run directory made as `run start`
 *   makes it, its stdout in a file.
 * @tparam P The program.
 * @param fixture The operator's file and state root.
 * @param args The arguments after `run.id`.
 * @return What it answered.
 */
template <typename P>
RunResult run_child(
  const PaperFixture& fixture,
  const std::vector<std::string>& args,
  const std::function<void(const std::string&, const std::filesystem::path&)>& before = {}
) {
  RunResult ran;
  ran.run_ = next_run();
  ran.dir_ = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001" / ran.run_;
  std::filesystem::create_directories(ran.dir_);
  if (before) {
    before(ran.run_, ran.dir_);
  }
  const auto reply_path = fixture.dir() / (ran.run_ + ".reply");
  const auto stderr_path = fixture.dir() / (ran.run_ + ".stderr");
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
    const int code = cmdatl::driver::run_main<P>(static_cast<int>(argv.size()), argv.data(), runtest::Load{}, runtest::Make<P>{});
    std::fflush(nullptr);
    ::_exit(code);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  ran.exit_ = WEXITSTATUS(status);
  ran.stderr_ = slurp(stderr_path);
  const auto text = slurp(reply_path);
  INFO("reply: " << text);
  REQUIRE(!text.empty());
  ran.reply_ = json::parse(text).as_object();
  if (std::filesystem::exists(ran.dir_ / "events.jsonl")) {
    for (const auto& line : split_lines(slurp(ran.dir_ / "events.jsonl"))) {
      ran.events_.push_back(json::parse(line).as_object());
    }
  }
  return ran;
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

}  // namespace

TEST_CASE("a run trades its script on the mock, its start line first, and ends clean after withdrawing") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}, {5, {0, "1"}}});
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 0);
  CHECK(ran.reply_.at("env").as_string() == "paper");
  CHECK(ran.reply_.at("run").as_string() == ran.run_);
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("stopped").as_string() == "end");
  CHECK(ran.reply_.at("executions").as_int64() == 2);
  CHECK(ran.reply_.at("holdings_at_end").as_object().at("005930").as_string() == "0");
  CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  REQUIRE(!ran.events_.empty());
  CHECK(ran.events_.front().at("event").as_string() == "start");
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

TEST_CASE("a run refuses before trading: arguments, session legs, on_fill, the market, the armed file") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}});
  auto args = spec_args();
  args.push_back("paper.tick=5");
  const auto bad = run_child<runtest::Program>(fixture, args);
  CHECK(bad.exit_ == 2);
  CHECK(code_of(bad) == "usage.bad_value");
  const auto legs = run_child<runtest::LegsProgram>(fixture, spec_args());
  CHECK(legs.exit_ == 5);
  CHECK(code_of(legs) == "program.live_unsupported");
  const auto fill = run_child<runtest::FillProgram>(fixture, spec_args());
  CHECK(fill.exit_ == 5);
  CHECK(code_of(fill) == "program.live_unsupported");
  const auto us = run_child<runtest::Program>(fixture, {"instrument.x=005930", "orders.type=limit", "orders.offset=0.01"});
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
  CHECK(spec_code(with("data.store=kis/krx")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("replay.shorts=true")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("replay.fill=legs")) == std::make_pair(2, std::string("spec.invalid")));
  CHECK(spec_code(with("param.bad=1")) == std::make_pair(5, std::string("program.invalid")));
  CHECK(spec_code(with("instrument.y=035720")) == std::make_pair(3, std::string("refused.symbol_not_allowed")));
  CHECK(spec_code({"instrument.x=005930", "orders.type=market", "replay.market=kr"}) == std::make_pair(3, std::string("refused.order_type")));
  CHECK(spec_code({"instrument.x=005930", "orders.type=limit", "orders.offset=0.06", "replay.market=kr"}) == std::make_pair(3, std::string("refused.collar")));
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
  const auto ran = run_child<runtest::Program>(fixture, spec_args(), [&held](const std::string&, const std::filesystem::path& dir) {
    held = std::make_unique<cmdatl::OwnerLock>(dir.string());
    REQUIRE(held->acquire(std::chrono::milliseconds(0), [] { return false; }, [](const std::string&) {}) == cmdatl::OwnerLock::Outcome::HELD);
  });
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran) == "busy.locked");
  CHECK(!std::filesystem::exists(ran.dir_ / "journal.jsonl"));
}

TEST_CASE("the placement boundary refuses market orders, limits outside the collar, and phases the operator did not permit") {
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
    runtest::script().actions_[5] = [armed] { std::filesystem::remove(armed); };
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
      const auto dir = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001";
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
    runtest::script().actions_[4] = [] { std::raise(SIGTERM); };
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    INFO(json::serialize(ran.reply_));
    CHECK(ran.reply_.at("stopped").as_string() == "signal");
    CHECK(count_of(ran, "placed") == 1);
  }
}

TEST_CASE("the operator's stop file: this run's withdraw cancels, another run's line is noted, a line written before the start acts first") {
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    // A buy below the price at bar 4, the withdraw written at the same bar: read at the next step, before the
    // order's session ends, so the closing pass cancels it.
    set_script({{4, {10, "0.97"}}});
    const auto stop = fixture.dir() / "paper.stop";
    std::string run_id;
    runtest::script().actions_[4] = [&run_id, stop] { spit(stop, "withdraw " + run_id + "\n"); };
    const auto ran = run_child<runtest::Program>(fixture, spec_args(), [&run_id](const std::string& run, const std::filesystem::path&) {
      run_id = run;
    });
    INFO(json::serialize(ran.reply_));
    CHECK(ran.exit_ == 0);
    CHECK(ran.reply_.at("stopped").as_string() == "operator");
    CHECK(count_of(ran, "canceled") >= 1);
    CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  }
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1"}}, {5, {0, "1"}}});
    spit(fixture.dir() / "paper.stop", "withdraw r999999999999\n");
    const auto ran = run_child<runtest::Program>(fixture, spec_args());
    CHECK(ran.exit_ == 0);
    CHECK(ran.events_.front().at("event").as_string() == "start");
    CHECK(count_of(ran, "note") == 1);
    CHECK(ran.reply_.at("stopped").as_string() == "end");
    CHECK(count_of(ran, "filled") == 2);
  }
  {
    const PaperFixture fixture(run_parts());
    fixture.arm();
    set_script({{2, {10, "1"}}});
    const auto ran = run_child<runtest::Program>(fixture, spec_args(), [&fixture](const std::string& run, const std::filesystem::path&) {
      spit(fixture.dir() / "paper.stop", "quiesce " + run + "\n");
    });
    CHECK(ran.events_.at(0).at("event").as_string() == "start");
    CHECK(ran.events_.at(1).at("event").as_string() == "stop");
    CHECK(count_of(ran, "placed") == 0);
    CHECK(ran.reply_.at("stopped").as_string() == "operator");
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
    for (const char* field : {"executions", "commission_paid", "tax_paid", "closing_equity", "holdings_at_end", "owed_at_end"}) {
      CAPTURE(field);
      CHECK(ran.reply_.at(field) == normal.reply_.at(field));
    }
    CHECK(count_of(ran, "filled") == 2);
    CHECK(count_of(ran, "placed") == 2);
  };
  // The buy's acknowledgement lost.
  set_script(decisions);
  cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) { client.lose_next_reply(Call::PLACE); };
  const auto buy_lost = run_child<runtest::Program>(fixture, spec_args());
  same_summary(buy_lost);
  CHECK(events_of(buy_lost, "placed").front().at("acknowledged") == json::value(false));
  // The sale's acknowledgement lost.
  set_script(decisions);
  cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) { current_client() = &client; };
  runtest::script().actions_[6] = [] { current_client()->lose_next_reply(Call::PLACE); };
  same_summary(run_child<runtest::Program>(fixture, spec_args()));
  // An order whose acknowledgement was lost and which then expires: placed, expired, nothing owed.
  set_script({{3, {10, "0.97"}}});
  cmdatl::driver::client_hook() = [](algotradelib::mock::MockClient& client) { client.lose_next_reply(Call::PLACE); };
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
  cmdatl::driver::RunClock clock(market, bound, [&steps] { ++steps; });
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
    cash = filled.at("side").as_string() == "buy" ? *algotradelib::decimal_subtract(cash, amount) : *algotradelib::decimal_add(cash, amount);
    cash = *algotradelib::decimal_subtract(cash, algotradelib::Decimal64::parse(std::string(filled.at("commission").as_string())));
  }
  CHECK(ran.reply_.at("holdings_at_end").as_object().at("005930").as_string() == "0");
  CHECK(ran.reply_.at("closing_equity").as_string() == cash.str());
  CHECK(ran.reply_.at("commission_paid").as_string() != "0");
}

TEST_CASE("an exception after the start line is an unknown outcome, with the reply written before the lock is released") {
  const PaperFixture fixture(run_parts());
  fixture.arm();
  set_script({{2, {10, "1"}}});
  runtest::script().actions_[4] = [] { throw std::runtime_error("the program failed"); };
  const auto ran = run_child<runtest::Program>(fixture, spec_args());
  INFO(json::serialize(ran.reply_));
  CHECK(ran.exit_ == 4);
  CHECK(code_of(ran) == "unknown.internal");
  CHECK(ran.events_.front().at("event").as_string() == "start");
}

TEST_CASE("the boundary's own checks, one by one, and the phase from an unreadable calendar") {
  namespace atl = algotradelib;
  using cmdatl::driver::placement_refusal;
  const ScratchDir dir;
  spit(dir.path() / "armed", "");
  cmdatl::OwnerLock lock(dir.path().string());
  REQUIRE(lock.acquire(std::chrono::milliseconds(0), [] { return false; }, [](const std::string&) {}) == cmdatl::OwnerLock::Outcome::HELD);
  cmdatl::driver::PlacementPolicy policy;
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
  std::vector<cmdatl::driver::PlacementEvent> seen;
  cmdatl::driver::GuardedSession<atl::BlockingSession<atl::mock::MockClient>> session(
    inner, policy, [&market] { return market.now(); }, [&seen](const cmdatl::driver::PlacementEvent& e) { seen.push_back(e); }
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
  CHECK(!placed);  // nothing held: a sell with no holding is the broker's refusal, reached
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
#endif

#if !defined(_WIN32)
TEST_CASE("the stop file holds one line per run, and each run reads the strongest line naming it") {
  using cmdatl::driver::stop_intent_of;
  using algotradelib::runner::StopIntent;
  const std::string a = "r00000000000a";
  const std::string b = "r00000000000b";
  const std::string text = "withdraw " + a + "\nquiesce " + b + "\n";
  CHECK(stop_intent_of(text, a) == StopIntent::WITHDRAW);
  CHECK(stop_intent_of(text, b) == StopIntent::QUIESCE);
  CHECK(!stop_intent_of(text, "r00000000000c"));
  // The strongest of a run's own lines, whatever their order; the last line may lack its newline.
  CHECK(stop_intent_of("withdraw " + a + "\nquiesce " + a, a) == StopIntent::WITHDRAW);
  CHECK(stop_intent_of("quiesce " + a + "\nwithdraw " + a, a) == StopIntent::WITHDRAW);
  // Malformed lines name nothing, and do not hide a valid one.
  CHECK(stop_intent_of("withdraw  " + a + "\nflatten " + a + "\nquiesce " + a + " x\n", a) == std::nullopt);
  CHECK(stop_intent_of("junk\nquiesce " + a + "\n", a) == StopIntent::QUIESCE);
  CHECK(stop_intent_of("quiesce " + a + "\r\n", a) == std::nullopt);
}
#endif
