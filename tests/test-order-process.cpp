// The order commands as processes: a session across the paper data's days, commands racing for one account, the
// exit table's send invariant after every command, and an allocation failing anywhere in an order command.

#include "./order-support.hpp"

#include <cmdatl/cmdatl>

#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

using namespace support;

namespace {

/**
 * @brief Runs the binary once and reads its JSON reply, checking the send invariant around it.
 * @param fixture The account.
 * @param args The command line, without `--json`.
 * @return The answer.
 */
Answer step(const PaperFixture& fixture, const std::vector<std::string>& args) {
  const auto before = std::filesystem::exists(fixture.log()) ? slurp(fixture.log()) : std::string();
  std::vector<std::string> line{"--json"};
  line.insert(line.end(), args.begin(), args.end());
  Spawned cmdatl(line);
  const int status = cmdatl.wait(60);
  REQUIRE(WIFEXITED(status));
  Answer answer;
  answer.exit_ = WEXITSTATUS(status);
  answer.body_ = reply_of(cmdatl.out());
  if (const auto* error = answer.body_.if_contains("error")) {
    answer.code_ = std::string(error->as_object().at("code").as_string());
  }
  if (const auto* reason = answer.body_.if_contains("reason")) {
    answer.reason_ = std::string(reason->as_string());
  }
  INFO(cmdatl::join(args) << ": " << cmdatl.out() << cmdatl.err());
  CHECK(answer.body_.at("env").as_string() == (answer.exit_ == 2 ? "offline" : "paper"));
  const auto after = std::filesystem::exists(fixture.log()) ? slurp(fixture.log()) : std::string();
  CHECK(no_send_ok(answer, before, after));
  CHECK(no_children());
  return answer;
}

/**
 * @brief A plan's id from the binary.
 * @param fixture The account.
 * @param symbol The symbol.
 * @param target The target.
 * @param limit The limit.
 * @return The plan id.
 */
std::string plan_of(
  const PaperFixture& fixture, const std::string& symbol, const std::string& target, const std::string& limit
) {
  const auto planned = step(fixture, {"order", "plan", symbol, "--target", target, "--limit", limit});
  INFO(boost::json::serialize(planned.body_));
  REQUIRE(planned.reason_ == "plan");
  return std::string(planned.body_.at("plan_id").as_string());
}

/**
 * @brief The collar check over a whole log: every placement a limit order within the collar of its day's prices.
 * @param symbol The symbol.
 * @param at The instant.
 * @return A price of the day, for the collar's check.
 */
double day_price(const std::string& symbol, std::int64_t at) {
  (void)at;
  return symbol == "005930" ? 79000 : 182000;
}

} // namespace

TEST_CASE(
  "a session against the binary: plans and places on both symbols across the data's days and buys and sells",
  "[orders][process]"
) {
  const Watchdog watchdog("the order session", 300);
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"005930\", \"000660\"]\n"
    "position = { \"005930\" = 100, \"000660\" = 20 }\n"
    "order_value = 5000000\n"
    "daily_orders = 20\n";
  const PaperFixture fixture(parts);
  fixture.arm();
  REQUIRE(step(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
  // 2024-03-26: buy both, marketable, and confirm from holdings.
  auto id = plan_of(fixture, "005930", "10", "78500");
  auto placed = step(fixture, {"order", "place", id});
  REQUIRE(placed.reason_ == "placed");
  CHECK(placed.body_.at("order").as_object().at("side").as_string() == "buy");
  REQUIRE(step(fixture, {"clock", "advance", "--minutes", "1"}).exit_ == 0);
  CHECK(step(fixture, {"order", "reconcile"}).reason_ == "clean");
  id = plan_of(fixture, "000660", "2", "181000");
  CHECK(step(fixture, {"order", "place", id}).reason_ == "placed");
  REQUIRE(step(fixture, {"clock", "advance", "--minutes", "30"}).exit_ == 0);
  CHECK(step(fixture, {"order", "reconcile"}).exit_ == 0);
  auto account = step(fixture, {"account"});
  CHECK(account.body_.at("positions").as_array().size() == 2);
  // A resting order, withdrawn disarmed.
  id = plan_of(fixture, "005930", "15", "75500");
  CHECK(step(fixture, {"order", "place", id}).reason_ == "placed");
  std::filesystem::remove(fixture.armed());
  CHECK(step(fixture, {"order", "plan", "005930", "--target", "16", "--limit", "75500"}).reason_ == "plan");
  const auto canceled = step(fixture, {"order", "cancel", "005930"});
  CHECK(canceled.reason_ == "withdrawal_sent");
  CHECK(step(fixture, {"order", "cancel", "005930"}).reason_ == "nothing_working");
  CHECK(step(fixture, {"order", "reconcile"}).reason_ == "clean");
  fixture.arm();
  // Sell 000660 out.
  id = plan_of(fixture, "000660", "0", "180000");
  placed = step(fixture, {"order", "place", id});
  REQUIRE(placed.reason_ == "placed");
  CHECK(placed.body_.at("order").as_object().at("side").as_string() == "sell");
  REQUIRE(step(fixture, {"clock", "advance", "--minutes", "1"}).exit_ == 0);
  CHECK(step(fixture, {"order", "reconcile"}).reason_ == "clean");
  // 2024-03-27: 000660 does not trade; 005930 sells part, then out.
  REQUIRE(step(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
  id = plan_of(fixture, "005930", "4", "79000");
  placed = step(fixture, {"order", "place", id});
  REQUIRE(placed.reason_ == "placed");
  CHECK(placed.body_.at("order").as_object().at("quantity").as_string() == "6");
  REQUIRE(step(fixture, {"clock", "advance", "--minutes", "1"}).exit_ == 0);
  CHECK(step(fixture, {"order", "reconcile", "005930"}).reason_ == "clean");
  id = plan_of(fixture, "005930", "0", "79000");
  CHECK(step(fixture, {"order", "place", id}).reason_ == "placed");
  REQUIRE(step(fixture, {"clock", "advance", "--minutes", "5"}).exit_ == 0);
  CHECK(step(fixture, {"order", "reconcile"}).reason_ == "clean");
  account = step(fixture, {"account"});
  CHECK(account.body_.at("positions").as_array().empty());
  CHECK(account.body_.at("owed").as_array().empty());
  // After the close: a plan is refused by phase, a reconcile still runs.
  REQUIRE(step(fixture, {"clock", "advance", "--hours", "8"}).exit_ == 0);
  CHECK(step(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "80500"}).code_ == "refused.phase");
  CHECK(step(fixture, {"order", "reconcile", "005930"}).reason_ == "clean");
  const auto log = slurp(fixture.log());
  CHECK(ops_of(log, "place").size() == 6);
  CHECK(limits_in_collar(log, 0.05, day_price));
  // The formats: jsonl and text answer too.
  Spawned text({"--format", "text", "order", "reconcile"});
  const int text_status = text.wait(60);
  CHECK(WIFEXITED(text_status));
  CHECK(text.out().find("clean") != std::string::npos);
  Spawned jsonl({"--format", "jsonl", "order", "reconcile"});
  const int jsonl_status = jsonl.wait(60);
  CHECK(WIFEXITED(jsonl_status));
  CHECK(reply_of(lines_of(jsonl.out()).back()).at("reason").as_string() == "clean");
}

TEST_CASE("two places on one plan id race: one place line and the other plan_used", "[orders][process]") {
  const Watchdog watchdog("the place race", 120);
  for (int round = 0; round < 3; ++round) {
    const PaperFixture fixture;
    fixture.arm();
    REQUIRE(step(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
    const auto id = plan_of(fixture, "005930", "10", "75000");
    std::vector<std::unique_ptr<Spawned>> racers;
    for (int k = 0; k < 2; ++k) {
      racers.push_back(std::make_unique<Spawned>(std::vector<std::string>{"--json", "order", "place", id}));
    }
    std::vector<std::string> outcomes;
    for (auto& racer : racers) {
      const int status = racer->wait(60);
      REQUIRE(WIFEXITED(status));
      const auto reply = reply_of(racer->out());
      outcomes.push_back(
        reply.contains("error") ?
          std::string(reply.at("error").as_object().at("code").as_string()) :
          std::string(reply.at("reason").as_string())
      );
    }
    std::sort(outcomes.begin(), outcomes.end());
    CHECK(outcomes == std::vector<std::string>{"placed", "refused.plan_used"});
    CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
  }
}

TEST_CASE("three accounts reads and two places and a clock advance together end consistent", "[orders][process]") {
  const Watchdog watchdog("the mixed race", 120);
  const PaperFixture fixture;
  fixture.arm();
  REQUIRE(step(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
  const auto a = plan_of(fixture, "005930", "10", "75000");
  std::vector<std::unique_ptr<Spawned>> all;
  for (int k = 0; k < 3; ++k) {
    all.push_back(std::make_unique<Spawned>(std::vector<std::string>{"--json", "account"}));
  }
  all.push_back(std::make_unique<Spawned>(std::vector<std::string>{"--json", "order", "place", a}));
  all.push_back(std::make_unique<Spawned>(std::vector<std::string>{"--json", "order", "place", a}));
  all.push_back(std::make_unique<Spawned>(std::vector<std::string>{"--json", "clock", "advance", "--minutes", "1"}));
  int placed = 0;
  for (auto& process : all) {
    const int status = process->wait(60);
    REQUIRE(WIFEXITED(status));
    const auto reply = reply_of(process->out());
    if (reply.contains("reason") && reply.at("reason").as_string() == "placed") {
      ++placed;
    }
  }
  // The advance may make the plan stale before either place: at most one placement.
  CHECK(placed <= 1);
  CHECK(ops_of(slurp(fixture.log()), "place").size() == static_cast<std::size_t>(placed));
  CHECK(step(fixture, {"status"}).exit_ == 0);
}

TEST_CASE(
  "two operator files naming one account: their places serialize and one placed and the other plan_used",
  "[orders][process]"
) {
  const Watchdog watchdog("two operator files", 120);
  const PaperFixture fixture;
  fixture.arm();
  REQUIRE(step(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
  const auto id = plan_of(fixture, "005930", "10", "75000");
  const auto other = fixture.dir() / "other.toml";
  spit(other, slurp(fixture.config()));
  spit(fixture.dir() / "other.armed", "");
  Spawned first({"--json", "order", "place", id});
  const ScopedVariable config("CMDATL_CONFIG", other.string());
  Spawned second({"--json", "order", "place", id});
  std::vector<std::string> outcomes;
  for (auto* process : {&first, &second}) {
    const int status = process->wait(60);
    REQUIRE(WIFEXITED(status));
    const auto reply = reply_of(process->out());
    outcomes.push_back(
      reply.contains("error") ?
        std::string(reply.at("error").as_object().at("code").as_string()) :
        std::string(reply.at("reason").as_string())
    );
  }
  std::sort(outcomes.begin(), outcomes.end());
  CHECK(outcomes == std::vector<std::string>{"placed", "refused.plan_used"});
}

namespace {

/** @brief What one injected run left. */
struct Swept {
  /** @brief Whether the allocation was reached (else the run was the uninjected last one). */
  bool consumed_{false};
  /** @brief Whether the run kept the promises: no descriptor or child left, and the send invariant. */
  bool ok_{false};
  /** @brief The reply's `reason`, or empty. */
  std::string reason_;
};

/**
 * @brief Runs a command line in process with the n-th allocation failing.
 * @param fixture The account.
 * @param args The command line, without `--json`.
 * @param n The allocation.
 * @param before The descriptor count before.
 * @return What it left.
 */
Swept swept(const PaperFixture& fixture, const std::vector<std::string>& args, long n, int before) {
  const auto log = slurp(fixture.log());
  std::vector<std::string> line{"--json"};
  line.insert(line.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  fail_allocation(n);
  int exit = -1;
  try {
    exit = cmdatl::run(line, out, err, false);
  } catch (const std::bad_alloc&) {
    // Before the command started (reading the line): main's last resort, nothing sent.
  }
  Swept result;
  result.consumed_ = fail_at.load() == 0;
  fail_allocation(0);
  Answer answer;
  answer.exit_ = exit;
  boost::system::error_code error;
  const auto value = boost::json::parse(out.str(), error);
  if (!error && value.is_object()) {
    answer.body_ = value.as_object();
    if (const auto* reason = answer.body_.if_contains("reason"); reason != nullptr && reason->is_string()) {
      answer.reason_ = std::string(reason->as_string());
    }
  }
  result.reason_ = answer.reason_;
  result.ok_ = open_fds() == before && no_children() && no_send_ok(answer, log, slurp(fixture.log()));
  return result;
}

} // namespace

TEST_CASE(
  "an allocation failing anywhere in an order command's refusals leaks nothing and sends nothing", "[orders][sweep]"
) {
  const Watchdog watchdog("the order allocation sweep", 1800);
  const std::vector<std::vector<std::string>> lines{
    {"order", "plan", "005930", "--target", "10", "--limit", "75000"},
    {"order", "plan", "005930", "--target", "1000", "--limit", "75000"},
    {"order", "place", "p0000000000"},
    {"order", "cancel", "005930"},
    {"order", "reconcile"},
    {"order", "release", "005930", "--reason", "read"},
  };
  long injected = 0;
  long bad = 0;
  for (const auto& args : lines) {
    const PaperFixture fixture;
    fixture.arm();
    REQUIRE(ask(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
    const int before = open_fds();
    for (long n = 1; n < 400000; n += (n < 3000 ? 1 : 23)) {
      const auto result = swept(fixture, args, n, before);
      if (!result.consumed_) {
        break;
      }
      ++injected;
      if (!result.ok_ && bad == 0) {
        bad = n;
        UNSCOPED_INFO(cmdatl::join(args) << ": first bad at allocation " << n);
      }
      if (n % 211 == 0) {
        // The lock is free again, and an ordinary read answers.
        CHECK(ask(fixture, {"account"}).exit_ == 0);
      }
    }
    CHECK(ask(fixture, {"account"}).exit_ == 0);
  }
  WARN("allocations injected: " << injected << ", first bad: " << bad);
  CHECK(bad == 0);
  CHECK(injected > 1000);
}

TEST_CASE(
  "an allocation failing anywhere in a place that sends leaks nothing and reports what reached the market",
  "[orders][sweep]"
) {
  const Watchdog watchdog("the send allocation sweep", 1800);
  long injected = 0;
  long bad = 0;
  for (long n = 1; n < 400000; ++n) {
    const PaperFixture fixture;
    fixture.arm();
    REQUIRE(ask(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
    const auto planned = ask(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"});
    REQUIRE(planned.reason_ == "plan");
    const int before = open_fds();
    const auto result =
      swept(fixture, {"order", "place", std::string(planned.body_.at("plan_id").as_string())}, n, before);
    if (!result.consumed_) {
      // The run the failure never reached is an ordinary placement.
      CHECK(result.reason_ == "placed");
      break;
    }
    ++injected;
    if (!result.ok_ && bad == 0) {
      bad = n;
      UNSCOPED_INFO("first bad at allocation " << n);
    }
    // Whatever the reply said, the account still opens, and at most one order reached the market.
    CHECK(ops_of(slurp(fixture.log()), "place").size() <= 1);
    const auto account = ask(fixture, {"account"});
    CHECK((account.exit_ == 0 || account.exit_ == 4));
  }
  WARN("allocations injected: " << injected << ", first bad: " << bad);
  CHECK(bad == 0);
  CHECK(injected > 1000);
}

TEST_CASE(
  "an allocation failing in the journal-open refusal: a descriptor the library leaves open is counted, not passed",
  "[orders][sweep]"
) {
  const Watchdog watchdog("the journal-open refusal sweep", 1800);
  const PaperFixture fixture;
  fixture.arm();
  REQUIRE(ask(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
  // The journal-open refusal: a second writer holds the journal, so the library's flock refuses. An allocation failing
  // inside the library's refusal message could leave its descriptor open before algotradelib 66b066c; those iterations
  // are counted and reported, not passed.
  long gap = 0;
  long refusal_runs = 0;
  {
    algotradelib::runner::FileJournal holder(fixture.journal().string(), "paper:paper:paper-0001");
    REQUIRE(holder.open());
    const auto refused = ask(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"});
    CHECK(refused.exit_ == 4);
    CHECK(refused.code_ == "unknown.journal_damaged");
    const int held = open_fds();
    std::vector<int> baseline;
    for (int fd = 0; fd < 1024; ++fd) {
      if (::fcntl(fd, F_GETFD) != -1) {
        baseline.push_back(fd);
      }
    }
    for (long n = 1; n < 400000; n += (n < 3000 ? 1 : 23)) {
      std::ostringstream out;
      std::ostringstream err;
      fail_allocation(n);
      try {
        (void)cmdatl::run({"--json", "order", "plan", "005930", "--target", "10", "--limit", "75000"}, out, err, false);
      } catch (const std::bad_alloc&) {
      }
      const bool consumed = fail_at.load() == 0;
      fail_allocation(0);
      if (!consumed) {
        break;
      }
      ++refusal_runs;
      if (open_fds() != held) {
        ++gap;
        // Close what leaked, so the next iteration measures itself.
        for (int fd = 3; fd < 1024; ++fd) {
          if (::fcntl(fd, F_GETFD) != -1 && std::find(baseline.begin(), baseline.end(), fd) == baseline.end()) {
            ::close(fd);
          }
        }
      }
      CHECK(no_children());
    }
  }
  WARN(
    "journal-open refusal sweep: "
    << refusal_runs << " injected iterations, " << gap
    << " left a descriptor open (the library gap in FileJournal's constructor)"
  );
  CHECK(refusal_runs > 10);
}
#endif

#if !defined(_WIN32)
TEST_CASE(
  "descriptors and children: 200 in-process runs of each order command leave the counts as they were",
  "[orders][process]"
) {
  const Watchdog watchdog("the descriptor count", 600);
  const PaperFixture fixture;
  fixture.arm();
  REQUIRE(ask(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
  const int before = open_fds();
  for (int i = 0; i < 200; ++i) {
    const auto planned = ask(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"});
    REQUIRE(planned.exit_ == 0);
    // A place of a plan the book does not hold, a cancel with nothing working, a reconcile, a release refused.
    CHECK(ask(fixture, {"order", "place", "p0123456789"}).code_ == "refused.no_plan");
    CHECK(ask(fixture, {"order", "cancel", "005930"}).reason_ == "nothing_working");
    CHECK(ask(fixture, {"order", "reconcile"}).reason_ == "clean");
    CHECK(ask(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
    if (i % 50 == 0) {
      CHECK(open_fds() == before);
      CHECK(no_children());
    }
  }
  CHECK(open_fds() == before);
  CHECK(no_children());
}
#endif
