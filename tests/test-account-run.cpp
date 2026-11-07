// A run on a broker's account in process: `run_account` with the library's mock client standing in for the broker,
// the wall clock replaced by the mock market's (each slice of a wait moves the market), each run in a forked child
// with a fixture operator file naming a KIS demo account, its stdout captured, under a watchdog.

#include "paper-support.hpp"
#include "run-strategy.hpp"
#include "support.hpp"

#include <cmdatl/account/operator.hpp>
#include <cmdatl/driver/account.hpp>
#include <cmdatl/driver/clock.hpp>
#include <cmdatl/system/credential_pipe.hpp>
#include <cmdatl/system/process.hpp>

#include <algotradelib/mock>

#include <boost/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace atl = algotradelib;
namespace json = boost::json;

namespace {

/** @brief The operator's file for a KIS demo account: no [paper], the default limits, three orders a day unless given. */
std::string kis_config(int daily_orders = 3) {
  std::string text = "[account]\nbroker = \"kis\"\nnumber = \"12345678-01\"\n\n[limits]\n";
  text += "symbols = [\"005930\", \"000660\"]\n";
  text += "position = { \"005930\" = 100, \"000660\" = 20 }\n";
  text += "order_value = 5000000\n";
  text += "daily_orders = " + std::to_string(daily_orders) + "\n";
  return text;
}

/** @brief What a run in a child answered. */
struct Ran {
  /** @brief The child's exit. */
  int exit_{-1};
  /** @brief The reply. */
  json::object reply_;
  /** @brief The events. */
  std::vector<json::object> events_;
};

/**
 * @brief The error code of a reply, or empty.
 * @param ran The run.
 * @return The code.
 */
std::string code_of(const Ran& ran) {
  const auto* error = ran.reply_.if_contains("error");
  return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
}

/**
 * @brief The events of a kind.
 * @param ran The run.
 * @param kind The kind.
 * @return Them.
 */
std::vector<json::object> events_of(const Ran& ran, const std::string& kind) {
  std::vector<json::object> out;
  for (const auto& event : ran.events_) {
    if (event.at("event").as_string() == kind) {
      out.push_back(event);
    }
  }
  return out;
}

/**
 * @brief Runs `run_account` in a child on a fresh mock market starting at 09:30 KST of the third day of bars; the child
 *   raises SIGINT once an order rests on the market, so a run that places stops itself.
 * @param fixture The fixture (operator file, state root).
 * @param before Called in the child with the market and the session before the run (an order or a holding).
 * @return What it answered.
 */
Ran run_account_child(
  const PaperFixture& fixture,
  const std::function<void(atl::mock::Market&, atl::BlockingSession<atl::mock::MockClient>&)>& before = {}
) {
  const auto reply_path = fixture.dir() / "account.reply";
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(120);
    const int fd = ::open(reply_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    const int err = ::open((fixture.dir() / "account.stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(err, STDERR_FILENO);
    ::close(err);
    cmdatl::handle_signals();
    const auto series = runtest::bars_of("005930", 10, atl::BarInterval::ONE_DAY);
    atl::Instrument instrument;
    instrument.id_ = series.instrument_;
    instrument.name_ = "005930";
    instrument.type_ = atl::SecurityType::STOCK;
    instrument.currency_ = atl::Currency::krw();
    instrument.lot_size_ = atl::Decimal64::parse("1");
    instrument.price_rule_ = atl::PriceRule::krx_equity();
    atl::mock::MarketSettings settings;
    settings.cash_ = atl::Money{atl::Decimal64(100000000, 0), atl::Currency::krw()};
    settings.start_ = series.bars_.at(2).timestamp_ + std::chrono::minutes(30);
    atl::mock::Market market({series}, settings, {instrument});
    auto client = std::make_shared<atl::mock::MockClient>(market);
    atl::BlockingSession<atl::mock::MockClient> session(client, atl::mock::MockAccount{});
    if (before) {
      before(market, session);
    }
    const auto snapshot = cmdatl::load_operator_config();
    if (snapshot.state_ != cmdatl::ConfigSnapshot::State::OK) {
      ::_exit(90);
    }
    cmdatl::driver::AccountRun run;
    run.base_ = json::object{{"env", "kis-demo"}, {"run", "r1"}};
    run.id_ = "r1";
    run.symbols_ = {"005930"};
    run.ids_ = {series.instrument_};
    run.warmup_ = runtest::script().warmup_;
    run.account_dir_ = *cmdatl::account_dir(*snapshot.config_);
    bool raised = false;
    const auto make_clock = [&](std::function<void()> after_slice) {
      return cmdatl::driver::WallClock(
        std::move(after_slice),
        [&market] {
          return market.now();
        },
        [&market] {
          return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(market.now().time_since_epoch())
          );
        },
        [&market, &raised](std::chrono::nanoseconds duration) {
          market.advance_to(market.now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
          if (!raised && !market.working_orders().orders_.empty()) {
            raised = true;
            ::raise(SIGINT);
          }
        },
        [] {
          return cmdatl::interrupted();
        }
      );
    };
    const int code =
      cmdatl::driver::run_account(run, *snapshot.config_, runtest::Program(run.ids_), session, session, make_clock);
    std::fflush(nullptr);
    ::_exit(code);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  Ran ran;
  ran.exit_ = WEXITSTATUS(status);
  const auto text = slurp(reply_path);
  INFO("stdout: " << text);
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty()) {
      auto object = json::parse(line).as_object();
      if (object.contains("event")) {
        ran.events_.push_back(std::move(object));
      } else {
        ran.reply_ = std::move(object);
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return ran;
}

/**
 * @brief The fixture's account directory for the KIS demo account.
 * @param fixture The fixture.
 * @return The directory.
 */
std::filesystem::path kis_account_dir(const PaperFixture& fixture) {
  return fixture.dir() / "state" / "cmdatl" / "accounts" / "kis-demo-12345678-01";
}

/**
 * @brief Sets the script: one decision, a target of `target` shares at the close, on the second bar.
 * @param target The target.
 */
void one_decision(std::int64_t target = 1) {
  runtest::script() = runtest::Script{};
  runtest::script().decisions_ = {{2, runtest::Decision{target, "1"}}};
}

} // namespace

TEST_CASE("an account run places at its first decision, withdraws on a signal, and ends clean") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran = run_account_child(fixture);
  CHECK(ran.exit_ == 0);
  CHECK(code_of(ran).empty());
  REQUIRE(!ran.events_.empty());
  const auto& start = ran.events_.front();
  CHECK(start.at("event").as_string() == "start");
  CHECK(start.at("account").as_string() == "kis-demo-12345678-01");
  CHECK(start.at("orders_left_today").as_int64() == 3);
  CHECK(events_of(ran, "placed").size() == 1);
  // The heartbeat names what the loop last read, each with its time.
  const auto beats = events_of(ran, "heartbeat");
  REQUIRE(!beats.empty());
  CHECK(beats.front().contains("phase"));
  CHECK(beats.front().at("holdings").is_object());
  CHECK(beats.front().at("holdings").as_object().contains("read_at"));
  CHECK(ran.reply_.at("stopped").as_string() == "signal");
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  CHECK(ran.reply_.at("holdings_at_end").as_object().at("005930").as_string() == "0");
  // The account's journal, kept for the next run.
  CHECK(std::filesystem::exists(kis_account_dir(fixture) / "journal.jsonl"));
}

TEST_CASE("an account run carries today's order allowance across runs through the account's journal") {
  PaperFixture fixture;
  fixture.write_config(kis_config(1));
  fixture.arm();
  one_decision();
  const auto first = run_account_child(fixture);
  REQUIRE(first.exit_ == 0);
  CHECK(events_of(first, "placed").size() == 1);
  one_decision();
  const auto second = run_account_child(fixture);
  CHECK(second.exit_ == 3);
  CHECK(code_of(second) == "refused.daily_order_limit");
  CHECK(events_of(second, "start").empty());
}

TEST_CASE("an account run refuses to start with an order resting anywhere on the account, and sends nothing") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran = run_account_child(fixture, [](atl::mock::Market&, atl::BlockingSession<atl::mock::MockClient>& s) {
    atl::OrderRequest order;
    // Another symbol of the account, far below the market: it rests, and the run's withdraw would cancel it.
    order.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "005930"};
    order.side_ = atl::OrderSide::BUY;
    order.type_ = atl::OrderType::LIMIT;
    order.quantity_ = atl::Decimal64(1, 0);
    order.price_ = atl::Decimal64(60000, 0);
    REQUIRE(s.place(order));
  });
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran) == "refused.orders_resting");
  CHECK(events_of(ran, "start").empty());
  CHECK(!std::filesystem::exists(kis_account_dir(fixture) / "journal.jsonl"));
}

TEST_CASE("an account run refuses to start holding one of its symbols") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran =
    run_account_child(fixture, [](atl::mock::Market& market, atl::BlockingSession<atl::mock::MockClient>& s) {
      atl::OrderRequest order;
      order.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "005930"};
      order.side_ = atl::OrderSide::BUY;
      order.type_ = atl::OrderType::MARKET;
      order.quantity_ = atl::Decimal64(1, 0);
      REQUIRE(s.place(order));
      market.advance_to(market.now() + std::chrono::hours(24));
    });
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran) == "refused.not_flat");
  CHECK(ran.reply_.at("symbol").as_string() == "005930");
}

TEST_CASE("an account run refuses without the armed file, before reading the account") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  one_decision();
  const auto ran = run_account_child(fixture);
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran) == "refused.not_armed");
}

TEST_CASE("a login is the operator file's account only with the same number, product code and environment") {
  using cmdatl::driver::kis_account_mismatch;
  CHECK(!kis_account_mismatch("12345678-01", true, "12345678", "01", true));
  CHECK(kis_account_mismatch("12345678-01", true, "12345678", "01", false));
  CHECK(kis_account_mismatch("12345678-01", false, "12345678", "01", true));
  CHECK(kis_account_mismatch("12345678-01", true, "12345678", "22", true));
  CHECK(kis_account_mismatch("12345678-01", true, "87654321", "01", true));
}

TEST_CASE("the credentials' framing: two documents round trip, and anything else is refused") {
  using cmdatl::sys::frame_credentials;
  using cmdatl::sys::unframe_credentials;
  const auto payload = frame_credentials({"{\"a\":1}", ""});
  REQUIRE(payload);
  const auto back = unframe_credentials(*payload, 2);
  REQUIRE(back);
  CHECK((*back)[0] == "{\"a\":1}");
  CHECK((*back)[1].empty());
  CHECK(!unframe_credentials(*payload, 3));
  CHECK(!unframe_credentials(*payload + "x", 2));
  CHECK(!unframe_credentials(payload->substr(0, payload->size() - 4), 2));
  CHECK(!unframe_credentials(payload->substr(0, 5), 1));
  CHECK(!unframe_credentials("x\n", 1));
  CHECK(!unframe_credentials("", 1));
  CHECK(!frame_credentials({std::string(cmdatl::sys::CREDENTIAL_PAYLOAD_MAX, 'a')}));
}

TEST_CASE(
  "the credentials reach descriptor 3 across an exec boundary, are read to the end, and the descriptor closes"
) {
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    const auto payload = cmdatl::sys::frame_credentials({"{\"k\":\"v\"}", "{}"});
    if (!payload || cmdatl::sys::stage_credentials(*payload) != 0) {
      ::_exit(10);
    }
    const int flags = ::fcntl(cmdatl::sys::CREDENTIAL_FD, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) != 0) {
      ::_exit(11);
    }
    const auto read = cmdatl::sys::read_staged_credentials();
    if (!read || *read != *payload) {
      ::_exit(12);
    }
    if (::fcntl(cmdatl::sys::CREDENTIAL_FD, F_GETFD) != -1) {
      ::_exit(13);
    }
    // Nothing staged: refused, not blocked.
    ::_exit(cmdatl::sys::read_staged_credentials() ? 14 : 0);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("the wall clock waits in slices, returns early once on a signal, and waits whole afterwards") {
  std::chrono::steady_clock::time_point t{};
  bool signalled = false;
  int slices = 0;
  cmdatl::driver::WallClock clock(
    [&slices] {
      ++slices;
    },
    [] {
      return std::chrono::system_clock::time_point{};
    },
    [&t] {
      return t;
    },
    [&t](std::chrono::nanoseconds d) {
      t += std::chrono::duration_cast<std::chrono::steady_clock::duration>(d);
    },
    [&signalled] {
      return signalled;
    }
  );
  clock.wait_for(std::chrono::seconds(10));
  CHECK(slices == 10);
  CHECK(t == std::chrono::steady_clock::time_point{} + std::chrono::seconds(10));
  signalled = true;
  clock.wait_for(std::chrono::seconds(10));
  CHECK(slices == 11);
  CHECK(t == std::chrono::steady_clock::time_point{} + std::chrono::seconds(11));
  clock.wait_for(std::chrono::seconds(5));
  CHECK(slices == 16);
  CHECK(t == std::chrono::steady_clock::time_point{} + std::chrono::seconds(16));
}

TEST_CASE("the operator file takes a KIS account: demo by default, real written out, no [paper], a canonical number") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  auto snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
  CHECK(snapshot.config_->broker_ == "kis");
  CHECK(snapshot.config_->env_ == "demo");
  CHECK(snapshot.config_->scope() == "kis:demo:12345678-01");
  auto text = kis_config();
  text.replace(text.find("number"), 0, "env = \"real\"\n");
  fixture.write_config(text);
  snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
  CHECK(snapshot.config_->env_ == "real");
  fixture.write_config(kis_config() + "\n[paper]\nbars = \"bars.csv\"\ncash = 1\nstart = \"2024-03-25T09:00\"\n");
  snapshot = cmdatl::load_operator_config();
  CHECK(snapshot.state_ != cmdatl::ConfigSnapshot::State::OK);
  CHECK(snapshot.error_.code_ == "config.invalid");
}
#endif
