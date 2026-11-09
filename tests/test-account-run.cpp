// A run on a broker's account in process: `run_account` with the library's mock client standing in for the broker,
// the wall clock replaced by the mock market's (each slice of a wait moves the market), each run in a forked child
// with a fixture operator file naming a KIS demo account, its stdout captured, under a watchdog.

#include "broker-folders.hpp"
#include "paper-support.hpp"
#include "run-strategy.hpp"
#include "support.hpp"

// A broker folder's adapter over the library's mock broker (`tests/fixtures/adapters/mockbroker`), as a run's driver
// includes one before the folder's part of the run.
#include "adapter/adapter.hpp"

#include <cmdatl/account/operator.hpp>
#include <cmdatl/driver/account.hpp>
#include <cmdatl/driver/clock.hpp>
#include <cmdatl/driver/kis_account.hpp>
#include <cmdatl/driver/session.hpp>
#include <cmdatl/driver/user_account.hpp>
#include <cmdatl/paper/krx_grid.hpp>
#include <cmdatl/strategy/runs.hpp>
#include <cmdatl/system/credential_pipe.hpp>
#include <cmdatl/system/process.hpp>

#include <algotradelib/mock>

#include <boost/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace atl = algotradelib;
namespace json = boost::json;

namespace cmdatl {
namespace driver {

/** @brief The tests' way to the configs `KisBroker::identify` built. */
struct KisBrokerTest {
  /**
   * @brief The account's config.
   * @param broker The broker.
   * @return It.
   */
  static const atl::kis::KisConfig& trading(const KisBroker& broker) {
    return broker.trading_config_;
  }

  /**
   * @brief The market-data config, on demo.
   * @param broker The broker.
   * @return It, or nothing.
   */
  static const std::optional<atl::kis::KisConfig>& market(const KisBroker& broker) {
    return broker.market_config_;
  }
};

} // namespace driver
} // namespace cmdatl

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
  /** @brief What the market held after the run, by symbol. */
  json::object held_after_;
  /** @brief The net position the program read on each bar, history included. */
  std::vector<std::string> program_held_;
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
 * @brief Runs `run_account` on 005930 in a child, on a fresh mock market of 005930 and 000660 from 09:30 KST of the
 *   third day of bars.
 *   The child raises SIGINT once an order rests on the market, so a run that places stops itself.
 * @param fixture The fixture (operator file, state root).
 * @param before Called in the child with the market and the session before the run (an order or a holding).
 * @param scrubbed Whether the run trades as a broker folder's run does, through `ScrubbedSession`.
 * @param on_sleep Called in the child with each slice the clock sleeps, before the market moves.
 * @param own_market_data Whether market data is read through a second session on the same client, as on a KIS demo.
 * @return What it answered, and what the market held and the program read.
 */
Ran run_account_child(
  const PaperFixture& fixture,
  const std::function<void(atl::mock::Market&, atl::mock::MockClient&, atl::BlockingSession<atl::mock::MockClient>&)>&
    before = {},
  bool scrubbed = false, const std::function<void(std::chrono::nanoseconds)>& on_sleep = {},
  bool own_market_data = false
) {
  const auto reply_path = fixture.dir() / "account.reply";
  const auto after_path = fixture.dir() / "account.after";
  std::filesystem::remove(after_path);
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
    // Another symbol of the account, outside the run: an order or a holding the run must leave alone.
    const auto other = runtest::bars_of("000660", 10, atl::BarInterval::ONE_DAY);
    std::vector<atl::Instrument> instruments;
    for (const auto* listed : {&series, &other}) {
      atl::Instrument instrument;
      instrument.id_ = listed->instrument_;
      instrument.name_ = listed->instrument_.symbol_;
      instrument.type_ = atl::SecurityType::STOCK;
      instrument.currency_ = atl::Currency::krw();
      instrument.lot_size_ = atl::Decimal64::parse("1");
      instrument.price_rule_ = cmdatl::paper::krx_equity_rule();
      instruments.push_back(instrument);
    }
    atl::mock::MarketSettings settings;
    settings.cash_ = atl::Money{atl::Decimal64(100000000, 0), atl::Currency::krw()};
    settings.start_ = series.bars_.at(2).timestamp_ + std::chrono::minutes(30);
    atl::mock::Market market({series, other}, settings, instruments);
    auto client = std::make_shared<atl::mock::MockClient>(market);
    atl::BlockingSession<atl::mock::MockClient> session(client, atl::mock::MockAccount{});
    if (before) {
      before(market, *client, session);
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
        [&market, &raised, &on_sleep](std::chrono::nanoseconds duration) {
          if (on_sleep) {
            on_sleep(duration);
          }
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
    int code = 0;
    if (scrubbed) {
      // As a broker folder's run trades: the adapter's errors scrubbed, an exception's text not shown.
      run.opaque_errors_ = true;
      cmdatl::driver::ScrubbedSession<atl::BlockingSession<atl::mock::MockClient>> opaque(session);
      code =
        cmdatl::driver::run_account(run, *snapshot.config_, runtest::Program(run.ids_), opaque, opaque, make_clock);
    } else if (own_market_data) {
      atl::BlockingSession<atl::mock::MockClient> market_data(client, atl::mock::MockAccount{});
      code = cmdatl::driver::run_account(
        run, *snapshot.config_, runtest::Program(run.ids_), session, market_data, make_clock
      );
    } else {
      code =
        cmdatl::driver::run_account(run, *snapshot.config_, runtest::Program(run.ids_), session, session, make_clock);
    }
    // What the market holds after the run and what the program read, for the test to read.
    json::object held;
    if (const auto holdings = market.holdings()) {
      for (const auto& position : holdings.value().positions_) {
        held[position.instrument_.symbol_] = position.quantity_.str();
      }
    }
    json::array read;
    for (const auto& net : runtest::script().held_) {
      read.push_back(json::value(net));
    }
    std::FILE* after = std::fopen(after_path.c_str(), "w");
    if (after != nullptr) {
      std::fputs(json::serialize(json::object{{"held", held}, {"read", read}}).c_str(), after);
      std::fclose(after);
    }
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
  if (std::filesystem::exists(after_path)) {
    const auto after = json::parse(slurp(after_path)).as_object();
    ran.held_after_ = after.at("held").as_object();
    for (const auto& net : after.at("read").as_array()) {
      ran.program_held_.emplace_back(net.as_string());
    }
  }
  return ran;
}

/**
 * @brief Parses a reply written on a child's stdout: its last line.
 * @param path The file.
 * @return The reply.
 */
json::object last_reply(const std::filesystem::path& path) {
  const auto text = slurp(path);
  INFO("stdout: " << text);
  auto end = text.find_last_not_of('\n');
  REQUIRE(end != std::string::npos);
  const auto start = text.rfind('\n', end);
  return json::parse(text.substr(start == std::string::npos ? 0 : start + 1, end + 1)).as_object();
}

/**
 * @brief The broker folder's `identify` in a child, on the fixture's operator file and a login, on a fresh mock market.
 * @param fixture The fixture.
 * @param login The login.
 * @param number Set to the account the run would trade, when it is not refused.
 * @return The reply when refused, else an empty object.
 */
json::object identify_child(const PaperFixture& fixture, const std::string& login, std::string& number) {
  const auto out = fixture.dir() / "identify.out";
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(60);
    const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    const auto series = runtest::bars_of("005930", 10, atl::BarInterval::ONE_DAY);
    atl::mock::MarketSettings settings;
    atl::mock::Market market({series}, settings);
    cmdatl_adapter::market() = &market;
    const auto snapshot = cmdatl::load_operator_config();
    if (snapshot.state_ != cmdatl::ConfigSnapshot::State::OK) {
      ::_exit(90);
    }
    auto effective = *snapshot.config_;
    std::string market_data;
    cmdatl::driver::UserAccountBroker broker;
    const auto refused =
      broker.identify(json::object{{"env", "mockbroker"}}, *snapshot.config_, {login}, effective, market_data);
    if (!refused) {
      std::printf("%s\n", json::serialize(json::object{{"number", effective.number_}}).c_str());
    }
    std::fflush(nullptr);
    ::_exit(refused ? *refused : 0);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) != 90);
  auto reply = last_reply(out);
  if (WEXITSTATUS(status) == 0) {
    number = std::string(reply.at("number").as_string());
    return {};
  }
  return reply;
}

/**
 * @brief Runs a built account driver with a login on descriptor 3 and the fixture's operator file.
 * @param fixture The fixture.
 * @param driver The driver.
 * @param login The login.
 * @return The exit and the reply.
 */
std::pair<int, json::object> driver_child(
  const PaperFixture& fixture, const std::string& driver, const std::string& login
) {
  const auto out = fixture.dir() / "driver.out";
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(60);
    const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    const auto payload = cmdatl::sys::frame_credentials({login});
    if (!payload || cmdatl::sys::stage_credentials(*payload) != 0) {
      ::_exit(91);
    }
    ::setenv("CMDATL_CONFIG", fixture.config().c_str(), 1);
    ::execl(driver.c_str(), driver.c_str(), "run.id=r0123456789ab", "replay.market=kr", static_cast<char*>(nullptr));
    ::_exit(92);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) < 90);
  return {WEXITSTATUS(status), last_reply(out)};
}

/**
 * @brief The operator's file for a broker folder's real account.
 * @param broker The folder's name.
 * @param number The account's number, or empty for none.
 * @return The text.
 */
std::string folder_config(const std::string& broker, const std::string& number = "") {
  std::string text = "[account]\nbroker = \"" + broker + "\"\nenv = \"real\"\n";
  if (!number.empty()) {
    text += "number = \"" + number + "\"\n";
  }
  return text +
    "\n[limits]\nsymbols = [\"005930\"]\nposition = { \"005930\" = 100 }\norder_value = 5000000\ndaily_orders = 3\n";
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

/**
 * @brief Sets the script: one decision on the second bar, a target of `target` shares as a sell limit 3 % above the
 *   close, which rests; and a signal on the third bar, for a run whose order is refused and never rests.
 * @param target The target.
 */
void one_sell(std::int64_t target) {
  runtest::script() = runtest::Script{};
  runtest::script().decisions_ = {{2, runtest::Decision{target, "1.03"}}};
  runtest::script().actions_[3] = [] {
    ::raise(SIGINT);
  };
}

/** @brief KIS's refusal of a read for its per-second rate (`EGW00201`), as the client classifies it. */
atl::BrokerError rate_refusal() {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::RATE_LIMIT;
  error.http_status_ = 500;
  error.broker_code_ = "EGW00201";
  error.message_ = "{rt_cd:1, msg_cd:EGW00201}";
  return error;
}

/** @brief KIS's refusal of a second access token within a minute (`EGW00133`), as the client reports it on a read. */
atl::BrokerError token_refusal() {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::AUTHENTICATION;
  error.reached_broker_ = false;
  error.http_status_ = 403;
  error.broker_code_ = "EGW00133";
  error.message_ = "{error_code:EGW00133}";
  return error;
}

/**
 * @brief A sleep hook that adds each slice slept before the run's start line to the fixture's `slept` file.
 * @param fixture The fixture.
 * @return The hook, for `run_account_child`.
 */
std::function<void(std::chrono::nanoseconds)> sleeps_before_start(const PaperFixture& fixture) {
  const auto dir = fixture.dir();
  return [dir](std::chrono::nanoseconds duration) {
    if (slurp(dir / "account.reply").find("\"event\":\"start\"") == std::string::npos) {
      std::ofstream(dir / "slept", std::ios::app) << duration.count() << '\n';
    }
  };
}

/**
 * @brief A `before` that buys shares at the market and lets a day pass, so the account holds them when the run starts.
 * @param symbol The symbol.
 * @param quantity How many.
 * @return The hook.
 */
std::function<void(atl::mock::Market&, atl::mock::MockClient&, atl::BlockingSession<atl::mock::MockClient>&)> holding(
  const std::string& symbol, std::int64_t quantity
) {
  return [symbol, quantity](atl::mock::Market& market, atl::mock::MockClient&, auto& session) {
    atl::OrderRequest order;
    order.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol};
    order.side_ = atl::OrderSide::BUY;
    order.type_ = atl::OrderType::MARKET;
    order.quantity_ = atl::Decimal64(quantity, 0);
    REQUIRE(session.place(order));
    market.advance_to(market.now() + std::chrono::hours(24));
  };
}

/**
 * @brief The operator's file capping 005930 at one share, with an order value.
 * @param order_value The order value, in won.
 * @return The text.
 */
std::string capped_config(const std::string& order_value) {
  auto text = kis_config();
  text.replace(text.find("\"005930\" = 100"), 14, "\"005930\" = 1");
  text.replace(text.find("order_value = 5000000"), 21, "order_value = " + order_value);
  return text;
}

/**
 * @brief The start line's holdings of a symbol.
 * @param ran The run.
 * @param symbol The symbol.
 * @return The quantity, as the start line writes it.
 */
std::string held_at_start(const Ran& ran, const std::string& symbol) {
  return std::string(ran.events_.front().at("holdings_at_start").as_object().at(symbol).as_string());
}

/**
 * @brief The seconds slept before the start line, from the fixture's `slept` file.
 * @param fixture The fixture.
 * @return The seconds.
 */
long long slept_before_start(const PaperFixture& fixture) {
  std::ifstream in(fixture.dir() / "slept");
  long long total = 0;
  long long nanoseconds = 0;
  while (in >> nanoseconds) {
    total += nanoseconds;
  }
  return total / 1000000000LL;
}

/**
 * @brief The `next` of a reply's error.
 * @param ran The run.
 * @return It.
 */
std::string next_of_reply(const Ran& ran) {
  return std::string(ran.reply_.at("error").as_object().at("next").as_string());
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
  CHECK(start.at("holdings_at_start").as_object().size() == 1);
  CHECK(held_at_start(ran, "005930") == "0");
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
  const auto ran = run_account_child(
    fixture, [](atl::mock::Market&, atl::mock::MockClient&, atl::BlockingSession<atl::mock::MockClient>& s) {
      atl::OrderRequest order;
      // Another symbol of the account, far below the market: it rests, and the run's withdraw would cancel it.
      order.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "000660"};
      order.side_ = atl::OrderSide::BUY;
      order.type_ = atl::OrderType::LIMIT;
      order.quantity_ = atl::Decimal64(1, 0);
      order.price_ = atl::Decimal64(60000, 0);
      REQUIRE(s.place(order));
    }
  );
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran) == "refused.orders_resting");
  CHECK(events_of(ran, "start").empty());
  CHECK(!std::filesystem::exists(kis_account_dir(fixture) / "journal.jsonl"));
}

TEST_CASE("an account run starts holding one of its symbols and sends nothing for a target it holds") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_sell(1);
  const auto ran = run_account_child(fixture, holding("005930", 1));
  CHECK(ran.exit_ == 0);
  CHECK(code_of(ran).empty());
  REQUIRE(!ran.events_.empty());
  CHECK(held_at_start(ran, "005930") == "1");
  CHECK(events_of(ran, "placed").empty());
  CHECK(ran.reply_.at("holdings_at_end").as_object().at("005930").as_string() == "1");
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
}

TEST_CASE("an account run sells shares the account held before it started") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_sell(1);
  const auto ran = run_account_child(fixture, holding("005930", 2));
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(held_at_start(ran, "005930") == "2");
  const auto placed = events_of(ran, "placed");
  REQUIRE(placed.size() == 1);
  CHECK(placed.front().at("side").as_string() == "sell");
  CHECK(placed.front().at("quantity").as_string() == "1");
}

TEST_CASE("an account run holding more than its cap sells only by one order that brings it within the cap") {
  PaperFixture fixture;
  fixture.arm();
  const auto placed_sells = [](const auto& ran) {
    std::vector<std::string> quantities;
    for (const auto& event : events_of(ran, "placed")) {
      CHECK(event.at("side").as_string() == "sell");
      quantities.emplace_back(event.at("quantity").as_string());
    }
    return quantities;
  };
  const auto guard_refused = [](const auto& ran) {
    return ran.reply_.at("refused").as_object().at("guard").as_int64();
  };
  // Held 3, capped at 1: a sell to 2 leaves more than the cap, and the guard refuses it.
  fixture.write_config(capped_config("1000000"));
  one_sell(2);
  auto ran = run_account_child(fixture, holding("005930", 3));
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(held_at_start(ran, "005930") == "3");
  CHECK(ran.events_.front().at("cap").as_string() == "1");
  CHECK(placed_sells(ran).empty());
  CHECK(guard_refused(ran) >= 1);
  CHECK(ran.held_after_.at("005930").as_string() == "3");

  // To the cap, with room for 2 shares an order: one sell of 2.
  fixture.write_config(capped_config("200000"));
  one_sell(1);
  ran = run_account_child(fixture, holding("005930", 3));
  CHECK(ran.exit_ == 0);
  CHECK(placed_sells(ran) == std::vector<std::string>{"2"});

  // Flat, with room for 3 shares an order: one sell of 3.
  fixture.write_config(capped_config("300000"));
  one_sell(0);
  ran = run_account_child(fixture, holding("005930", 3));
  CHECK(ran.exit_ == 0);
  CHECK(placed_sells(ran) == std::vector<std::string>{"3"});

  // To the cap, with room for 1 share an order: no order fits, and nothing is placed.
  fixture.write_config(capped_config("100000"));
  one_sell(1);
  ran = run_account_child(fixture, holding("005930", 3));
  CHECK(ran.exit_ == 0);
  CHECK(placed_sells(ran).empty());
  CHECK(guard_refused(ran) >= 1);
  CHECK(ran.held_after_.at("005930").as_string() == "3");
}

TEST_CASE("an account run leaves another symbol's holding alone and does not list it") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran = run_account_child(fixture, holding("000660", 2));
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  const auto& listed = ran.events_.front().at("holdings_at_start").as_object();
  CHECK(listed.size() == 1);
  CHECK(listed.at("005930").as_string() == "0");
  CHECK(events_of(ran, "placed").size() == 1);
  CHECK(ran.reply_.at("stopped").as_string() == "signal");
  CHECK(ran.held_after_.at("000660").as_string() == "2");
}

TEST_CASE("a program reads the account's holding on every bar, the history read at the start included") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  // Three history bars before the first decision; it holds what it has, so nothing is sent.
  runtest::script() = runtest::Script{};
  runtest::script().warmup_ = 3;
  runtest::script().decisions_ = {{3, runtest::Decision{2, "1"}}};
  runtest::script().actions_[4] = [] {
    ::raise(SIGINT);
  };
  const auto ran = run_account_child(fixture, holding("005930", 2));
  CHECK(ran.exit_ == 0);
  CHECK(events_of(ran, "placed").empty());
  REQUIRE(ran.program_held_.size() >= 3);
  for (const auto& net : ran.program_held_) {
    CHECK(net == "2");
  }
}

TEST_CASE("the start's holdings sum every row of an instrument, are 0 without one, and refuse a sum that overflows") {
  using Tag = atl::BlockingSession<atl::mock::MockClient>::BrokerTag;
  const auto id = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "005930"};
  const auto other = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "000660"};
  const auto row = [](const atl::InstrumentId& instrument, const atl::Decimal64& quantity) {
    atl::HoldingPosition position;
    position.instrument_ = instrument;
    position.quantity_ = quantity;
    return position;
  };
  const auto split = cmdatl::driver::net_holdings<Tag>(
    {row(id, atl::Decimal64(60, 0)), row(other, atl::Decimal64(7, 0)), row(id, atl::Decimal64(40, 0))}, {id}
  );
  REQUIRE(split);
  CHECK(split->size() == 1);
  CHECK(split->at("005930").as_string() == "100");
  const auto none = cmdatl::driver::net_holdings<Tag>({row(other, atl::Decimal64(7, 0))}, {id});
  REQUIRE(none);
  CHECK(none->at("005930").as_string() == "0");
  const auto huge = atl::Decimal64(std::numeric_limits<std::int64_t>::max(), 0);
  CHECK(!cmdatl::driver::net_holdings<Tag>({row(id, huge), row(id, huge)}, {id}));
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
  // No number in the operator file: the login decides the account; the environment must still agree.
  CHECK(!kis_account_mismatch("", true, "87654321", "01", true));
  CHECK(kis_account_mismatch("", false, "87654321", "01", true));
}

TEST_CASE("a KIS run reads KRX's tape on every config it builds on demo and on real") {
  using cmdatl::driver::KisBroker;
  using cmdatl::driver::KisBrokerTest;
  // Fake keys: identify only reads the documents, and nothing here connects.
  const std::string demo_login =
    R"({"app_key":"fake-key","app_secret":"fake-secret","account_number":"12345678","demo":true})";
  const std::string real_login =
    R"({"app_key":"fake-key","app_secret":"fake-secret","account_number":"87654321","demo":false})";
  cmdatl::OperatorConfig config;
  config.broker_ = "kis";
  {
    config.env_ = "demo";
    KisBroker broker;
    auto effective = config;
    std::string market_data = "own";
    REQUIRE(!broker.identify(json::object{}, config, {demo_login, real_login}, effective, market_data));
    CHECK(effective.number_ == "12345678-01");
    CHECK(KisBrokerTest::trading(broker).market_tape_ == atl::kis::MarketTape::KRX);
    REQUIRE(KisBrokerTest::market(broker));
    CHECK(!KisBrokerTest::market(broker)->is_demo_);
    CHECK(KisBrokerTest::market(broker)->market_tape_ == atl::kis::MarketTape::KRX);
    CHECK(market_data == "kis-real (read only), KRX");
  }
  {
    config.env_ = "real";
    KisBroker broker;
    auto effective = config;
    std::string market_data = "own";
    REQUIRE(!broker.identify(json::object{}, config, {real_login, ""}, effective, market_data));
    CHECK(effective.number_ == "87654321-01");
    CHECK(!KisBrokerTest::trading(broker).is_demo_);
    CHECK(KisBrokerTest::trading(broker).market_tape_ == atl::kis::MarketTape::KRX);
    // The account's own session reads the market data.
    CHECK(!KisBrokerTest::market(broker));
    CHECK(market_data == "own, KRX");
  }
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

TEST_CASE("the operator file takes a KIS account: demo by default, real written out, no [paper], a number optional") {
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
  auto unnamed = kis_config();
  unnamed.erase(unnamed.find("number = "), std::string("number = \"12345678-01\"\n").size());
  fixture.write_config(unnamed);
  snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
  CHECK(snapshot.config_->number_.empty());
  CHECK(snapshot.config_->account_name() == "kis-demo");
  fixture.write_config(kis_config() + "\n[paper]\nbars = \"bars.csv\"\ncash = 1\nstart = \"2024-03-25T09:00\"\n");
  snapshot = cmdatl::load_operator_config();
  CHECK(snapshot.state_ != cmdatl::ConfigSnapshot::State::OK);
  CHECK(snapshot.error_.code_ == "config.invalid");
}

namespace {

/** @brief A credential store in memory, for the run's login reads. */
class Logins : public cmdatl::auth::CredentialStore {
public:
  std::map<std::string, std::string> items_;
  std::vector<std::string> asked_;
  std::string kind() const override {
    return "memory";
  }
  cmdatl::auth::StoreList list() override {
    cmdatl::auth::StoreList out;
    out.status_ = cmdatl::auth::StoreStatus::OK;
    for (const auto& item : items_) {
      out.names_.push_back(item.first);
    }
    return out;
  }
  cmdatl::auth::StoreAnswer get(const std::string& name) override {
    asked_.push_back(name);
    cmdatl::auth::StoreAnswer answer;
    const auto found = items_.find(name);
    answer.status_ = found == items_.end() ? cmdatl::auth::StoreStatus::NOT_FOUND : cmdatl::auth::StoreStatus::OK;
    if (found != items_.end()) {
      answer.value_ = found->second;
    }
    return answer;
  }
  cmdatl::auth::StoreAnswer put(const std::string&, const std::string&) override {
    return {};
  }
  cmdatl::auth::StoreAnswer remove(const std::string&) override {
    return {};
  }
};

} // namespace

TEST_CASE("a run on a KIS account reads its own login, and on demo the real one for market data, or refuses") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  const auto demo = cmdatl::load_operator_config();
  REQUIRE(demo.state_ == cmdatl::ConfigSnapshot::State::OK);
  Logins store;
  std::vector<std::string> documents;
  auto refused = cmdatl::runs::read_run_logins(*demo.config_, &store, documents);
  REQUIRE(refused);
  CHECK(refused->body_.at("error").as_object().at("code").as_string() == "auth.not_found");
  store.items_["kis/demo"] = "{\"demo\":true}";
  refused = cmdatl::runs::read_run_logins(*demo.config_, &store, documents);
  REQUIRE(refused);
  CHECK(refused->body_.at("error").as_object().at("code").as_string() == "auth.not_found");
  CHECK(
    std::string(refused->body_.at("error").as_object().at("message").as_string()).find("market data") !=
    std::string::npos
  );
  store.items_["kis/real"] = "{\"demo\":false}";
  CHECK(!cmdatl::runs::read_run_logins(*demo.config_, &store, documents));
  REQUIRE(documents.size() == 2);
  CHECK(documents[0] == "{\"demo\":true}");
  CHECK(documents[1] == "{\"demo\":false}");
  CHECK(cmdatl::runs::read_run_logins(*demo.config_, nullptr, documents));
  auto text = kis_config();
  text.replace(text.find("number"), 0, "env = \"real\"\n");
  fixture.write_config(text);
  const auto real = cmdatl::load_operator_config();
  REQUIRE(real.state_ == cmdatl::ConfigSnapshot::State::OK);
  store.asked_.clear();
  CHECK(!cmdatl::runs::read_run_logins(*real.config_, &store, documents));
  CHECK(store.asked_ == std::vector<std::string>{"kis/real"});
  CHECK(documents[1].empty());
}

TEST_CASE("a run on a broker folder's account reads its own login only, and never a data vendor's or a missing demo") {
  const BrokerHome home;
  home.folder("acme", BROKER_TOML);
  home.folder("vendor", VENDOR_TOML);
  PaperFixture fixture;
  Logins store;
  std::vector<std::string> documents;
  const auto code = [&]() {
    const auto snapshot = cmdatl::load_operator_config();
    REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
    const auto refused = cmdatl::runs::read_run_logins(*snapshot.config_, &store, documents);
    return refused ? std::string(refused->body_.at("error").as_object().at("code").as_string()) : std::string();
  };
  fixture.write_config(folder_config("acme"));
  CHECK(code() == "auth.not_found");
  store.items_["acme/real"] = "{\"appKey\":\"k\"}";
  store.asked_.clear();
  CHECK(code().empty());
  CHECK(store.asked_ == std::vector<std::string>{"acme/real"});
  CHECK(documents == std::vector<std::string>{"{\"appKey\":\"k\"}"});

  fixture.write_config(folder_config("vendor"));
  CHECK(code() == "unsupported.broker");
  auto text = folder_config("vendor");
  text.replace(text.find("env = \"real\""), 12, "env = \"demo\"");
  fixture.write_config(text);
  CHECK(code() == "unsupported.broker");
  home.folder("plain", "display = \"P\"\n[[fields]]\nname = \"k\"\nprompt = \"K\"\n");
  text = folder_config("plain");
  text.replace(text.find("env = \"real\""), 12, "env = \"demo\"");
  fixture.write_config(text);
  CHECK(code() == "config.invalid");
  fixture.write_config(folder_config("missing"));
  CHECK(code() == "adapter.not_found");
}

TEST_CASE("the account and order commands refuse a KIS account, and the reply names it kis-demo") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  const auto status = fixture.json({"status"});
  CHECK(status.exit_ == 1);
  const auto reply = json::parse(status.out_).as_object();
  CHECK(reply.at("env").as_string() == "kis-demo");
  CHECK(reply.at("error").as_object().at("code").as_string() == "unsupported.broker");
}

TEST_CASE("once a stop is latched, the boundary refuses every placement before the broker sees it") {
  PaperFixture fixture;
  const auto series = runtest::bars_of("005930", 5, atl::BarInterval::ONE_DAY);
  atl::Instrument instrument;
  instrument.id_ = series.instrument_;
  instrument.currency_ = atl::Currency::krw();
  instrument.lot_size_ = atl::Decimal64::parse("1");
  instrument.price_rule_ = cmdatl::paper::krx_equity_rule();
  atl::mock::MarketSettings settings;
  settings.cash_ = atl::Money{atl::Decimal64(100000000, 0), atl::Currency::krw()};
  settings.start_ = series.bars_.at(1).timestamp_ + std::chrono::minutes(30);
  atl::mock::Market market({series}, settings, {instrument});
  atl::BlockingSession<atl::mock::MockClient> inner(
    std::make_shared<atl::mock::MockClient>(market), atl::mock::MockAccount{}
  );
  std::vector<std::string> reasons;
  cmdatl::driver::GuardedSession<atl::BlockingSession<atl::mock::MockClient>> guarded(
    inner, cmdatl::driver::PlacementPolicy{},
    [&market] {
      return market.now();
    },
    [&reasons](const cmdatl::driver::PlacementEvent& event) {
      reasons.push_back(event.reason_);
    }
  );
  bool stopping = true;
  guarded.set_stopping([&stopping] {
    return stopping;
  });
  atl::OrderRequest order;
  order.instrument_ = series.instrument_;
  order.side_ = atl::OrderSide::BUY;
  order.type_ = atl::OrderType::LIMIT;
  order.quantity_ = atl::Decimal64(1, 0);
  order.price_ = atl::Decimal64(60000, 0);
  CHECK(!guarded.place(order));
  CHECK(!guarded.place(order));
  CHECK(reasons == std::vector<std::string>{"stopping", "stopping"});
  CHECK(market.working_orders().orders_.empty());
}

TEST_CASE("an account run whose calendar cannot be read at start refuses before anything is sent") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran = run_account_child(fixture, [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
    atl::BrokerError error;
    error.category_ = atl::ErrorCategory::TRANSPORT;
    error.reached_broker_ = false;
    error.message_ = "connection refused";
    client.fail_next(atl::mock::Call::CALENDAR, error);
  });
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran) == "run.reference_unreadable");
  CHECK(events_of(ran, "start").empty());
  CHECK(!std::filesystem::exists(kis_account_dir(fixture) / "journal.jsonl"));
}
TEST_CASE("a broker folder's run checks the login the adapter connects with against the operator file") {
  PaperFixture fixture;
  std::string number;
  fixture.write_config(folder_config("mockbroker"));
  CHECK(identify_child(fixture, R"({"account":"acct-1"})", number).empty());
  CHECK(number == "acct-1");

  // A demo login on a real account, an account no journal can be kept under, and a login the adapter cannot use.
  CHECK(
    identify_child(fixture, R"({"account":"acct-1","demo":true})", number).at("error").at("code") ==
    "auth.account_mismatch"
  );
  CHECK(identify_child(fixture, R"({"account":"../x"})", number).at("error").at("code") == "auth.account_mismatch");
  CHECK(identify_child(fixture, R"({"account":""})", number).at("error").at("code") == "auth.account_mismatch");
  const auto failed = identify_child(fixture, R"({"fail":"SECRET-TOKEN-123"})", number);
  CHECK(failed.at("error").at("code") == "auth.account_mismatch");
  CHECK(json::serialize(failed).find("SECRET-TOKEN-123") == std::string::npos);

  // The operator file's number is a pin.
  fixture.write_config(folder_config("mockbroker", "acct-2"));
  CHECK(identify_child(fixture, R"({"account":"acct-1"})", number).at("error").at("code") == "auth.account_mismatch");
  CHECK(identify_child(fixture, R"({"account":"acct-2"})", number).empty());
}

TEST_CASE("a broker folder's run shows none of the adapter's error text") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran = run_account_child(
    fixture,
    [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
      atl::BrokerError error;
      error.category_ = atl::ErrorCategory::HTTP;
      error.http_status_ = 401;
      error.broker_code_ = "E401";
      error.message_ = "GET /api/calendar?api_token=SECRET-TOKEN-123 answered 401";
      client.fail_next(atl::mock::Call::CALENDAR, error);
    },
    true
  );
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran) == "run.reference_unreadable");
  const auto text = json::serialize(ran.reply_);
  CHECK(text.find("SECRET-TOKEN-123") == std::string::npos);
  CHECK(text.find("HTTP 401, code E401") != std::string::npos);
  CHECK(text.find("its text is not shown") != std::string::npos);
  CHECK(slurp(fixture.dir() / "account.stderr").find("SECRET-TOKEN-123") == std::string::npos);
}

TEST_CASE("a broker folder's refused placement shows neither the adapter's text nor a long code") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto ran = run_account_child(
    fixture,
    [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
      atl::BrokerError error;
      error.category_ = atl::ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = true;
      error.broker_code_ = "https://vendor/error?api_token=SECRET-TOKEN-456";
      error.message_ = "refused for token SECRET-TOKEN-456";
      client.fail_next(atl::mock::Call::PLACE, error, 5);
      // Nothing comes to rest, so the run is stopped after its first decision by a signal of its own.
      std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ::kill(::getpid(), SIGINT);
      }).detach();
    },
    true
  );
  std::string text = json::serialize(ran.reply_);
  for (const auto& event : ran.events_) {
    text += json::serialize(event);
  }
  INFO(text);
  // The placement reached the adapter and was refused: its event names a reason, never the long code.
  const auto refused = events_of(ran, "refused");
  REQUIRE(!refused.empty());
  CHECK(refused.front().at("reason").as_string() == "broker");
  CHECK(text.find("SECRET-TOKEN-456") == std::string::npos);
  CHECK(slurp(fixture.dir() / "account.stderr").find("SECRET-TOKEN-456") == std::string::npos);
}

TEST_CASE("a broker folder's driver refuses a client that states none of what a run reads, and another broker's file") {
  PaperFixture fixture;
  fixture.write_config(folder_config("limited"));
  fixture.arm();
  auto [exit, reply] = driver_child(fixture, CMDATL_DRIVER_LIMITED, R"({"account":"a"})");
  CHECK(exit == 1);
  CHECK(reply.at("error").at("code") == "unsupported.capability");
  const auto& missing = reply.at("missing").as_array();
  CHECK(missing.size() == 3);
  CHECK(json::serialize(missing).find("has_calendar") != std::string::npos);
  CHECK(json::serialize(missing).find("has_working_orders") != std::string::npos);
  CHECK(json::serialize(missing).find("has_reference_data") != std::string::npos);

  // A driver built for one broker folder never trades another broker's account.
  fixture.write_config(folder_config("mockbroker"));
  std::tie(exit, reply) = driver_child(fixture, CMDATL_DRIVER_LIMITED, R"({"account":"a"})");
  CHECK(reply.at("error").at("code") == "unsupported.broker");
  fixture.write_config(kis_config());
  std::tie(exit, reply) = driver_child(fixture, CMDATL_DRIVER_LIMITED, R"({"account":"a"})");
  CHECK(reply.at("error").at("code") == "unsupported.broker");

  // Over the mock broker in another process the fixture has no market: connect throws, and its text is not shown.
  fixture.write_config(folder_config("mockbroker"));
  std::tie(exit, reply) = driver_child(fixture, CMDATL_DRIVER_MOCKBROKER, R"({"fail":"SECRET-TOKEN-123"})");
  CHECK(reply.at("error").at("code") == "auth.account_mismatch");
  CHECK(json::serialize(reply).find("SECRET-TOKEN-123") == std::string::npos);
}

TEST_CASE("a read before the start waits as each refusal asks and notes the wait first") {
  using cmdatl::driver::StartRead;
  const auto scripted = [](std::vector<atl::Result<int>> answers, std::size_t& reads) {
    return [answers, &reads]() {
      return answers.at(reads++);
    };
  };
  const auto t0 = std::chrono::system_clock::time_point(std::chrono::hours(1000));

  SECTION("the note comes before the wait and a rate refusal waits 2 s") {
    atl::runner::ManualClock clock(t0);
    std::vector<std::chrono::system_clock::time_point> noted;
    std::size_t reads = 0;
    cmdatl::driver::StartBudget budget;
    const auto answer = cmdatl::driver::read_at_start(
      scripted({atl::Result<int>(rate_refusal()), atl::Result<int>(7)}, reads), clock,
      [&](const std::string& line) {
        noted.push_back(clock.now());
        CHECK(line.find("request rate; reading again in 2 s (2 of 3)") != std::string::npos);
      },
      budget
    );
    REQUIRE(answer.answer_);
    CHECK(answer.answer_.value() == 7);
    CHECK(answer.refused_ == StartRead::NONE);
    CHECK(reads == 2);
    CHECK(noted == std::vector<std::chrono::system_clock::time_point>{t0});
    CHECK(clock.now() == t0 + std::chrono::seconds(2));
  }

  SECTION("rate then token then rate then rate: four reads and the fourth refusal returned as a rate refusal") {
    atl::runner::ManualClock clock(t0);
    std::size_t reads = 0;
    std::vector<std::string> notes;
    cmdatl::driver::StartBudget budget;
    const auto answer = cmdatl::driver::read_at_start(
      scripted(
        {atl::Result<int>(rate_refusal()), atl::Result<int>(token_refusal()), atl::Result<int>(rate_refusal()),
          atl::Result<int>(rate_refusal()), atl::Result<int>(7)},
        reads
      ),
      clock,
      [&](const std::string& line) {
        notes.push_back(line);
      },
      budget
    );
    CHECK(!answer.answer_);
    CHECK(answer.refused_ == StartRead::RATE);
    CHECK(reads == 4);
    CHECK(notes.size() == 3);
    CHECK(budget.token_waited_);
    CHECK(clock.now() == t0 + std::chrono::seconds(2 + 61 + 2));
    // The session's token wait is spent: a later read's token refusal returns at once.
    std::size_t again = 0;
    const auto later = cmdatl::driver::read_at_start(
      scripted({atl::Result<int>(token_refusal())}, again), clock, [](const std::string&) {}, budget
    );
    CHECK(later.refused_ == StartRead::TOKEN);
    CHECK(again == 1);
    CHECK(clock.now() == t0 + std::chrono::seconds(65));
  }

  SECTION("a retry_after of 5 s waits 5 s") {
    atl::runner::ManualClock clock(t0);
    std::size_t reads = 0;
    cmdatl::driver::StartBudget budget;
    auto refused = rate_refusal();
    refused.retry_after_ = std::chrono::seconds(5);
    std::string noted;
    const auto answer = cmdatl::driver::read_at_start(
      scripted({atl::Result<int>(refused), atl::Result<int>(7)}, reads), clock,
      [&](const std::string& line) {
        noted = line;
      },
      budget
    );
    CHECK(answer.answer_);
    CHECK(clock.now() == t0 + std::chrono::seconds(5));
    CHECK(noted.find("reading again in 5 s") != std::string::npos);
  }

  SECTION("a retry_after above the limit or unrepresentable returns at once with no wait and no second read") {
    for (
      const auto asked : {cmdatl::driver::START_RATE_WAIT_LIMIT + std::chrono::seconds(1), std::chrono::seconds::max()}
    ) {
      atl::runner::ManualClock clock(t0);
      std::size_t reads = 0;
      cmdatl::driver::StartBudget budget;
      auto refused = rate_refusal();
      refused.retry_after_ = asked;
      bool noted = false;
      const auto answer = cmdatl::driver::read_at_start(
        scripted({atl::Result<int>(refused), atl::Result<int>(7)}, reads), clock,
        [&](const std::string&) {
          noted = true;
        },
        budget
      );
      CHECK(!answer.answer_);
      CHECK(answer.refused_ == StartRead::LONG_WAIT);
      CHECK(cmdatl::driver::start_read_next(answer.refused_, "") == cmdatl::START_LONG_WAIT_NEXT);
      CHECK(reads == 1);
      CHECK(!noted);
      CHECK(clock.waits() == 0);
      CHECK(clock.now() == t0);
    }
  }

  SECTION("a protocol error and an authentication error with another code are read once") {
    atl::runner::ManualClock clock(t0);
    cmdatl::driver::StartBudget budget;
    atl::BrokerError protocol;
    protocol.category_ = atl::ErrorCategory::PROTOCOL;
    auto other = token_refusal();
    other.broker_code_ = "EGW00123";
    for (const auto& error : {protocol, other}) {
      std::size_t reads = 0;
      bool noted = false;
      const auto answer = cmdatl::driver::read_at_start(
        scripted({atl::Result<int>(error), atl::Result<int>(7)}, reads), clock,
        [&](const std::string&) {
          noted = true;
        },
        budget
      );
      CHECK(!answer.answer_);
      CHECK(answer.refused_ == StartRead::NONE);
      CHECK(reads == 1);
      CHECK(!noted);
    }
    CHECK(clock.now() == t0);
    CHECK(!budget.token_waited_);
  }
}

TEST_CASE("an account run reads the account again after a rate refusal and gives up naming the wait") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  auto ran = run_account_child(fixture, [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
    client.fail_next(atl::mock::Call::HOLDINGS, rate_refusal());
  });
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(ran.events_.front().at("event").as_string() == "start");
  CHECK(events_of(ran, "placed").size() == 1);
  const auto noted = slurp(fixture.dir() / "account.stderr");
  CHECK(
    noted.find("cmdatl: the broker refused a read for its request rate; reading again in 2 s") != std::string::npos
  );

  PaperFixture refused;
  refused.write_config(kis_config());
  refused.arm();
  one_decision();
  ran = run_account_child(
    refused,
    [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
      client.fail_next(atl::mock::Call::HOLDINGS, rate_refusal(), 3);
    },
    false, sleeps_before_start(refused)
  );
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran) == "run.account_unreadable");
  CHECK(next_of_reply(ran) == cmdatl::START_RATE_NEXT);
  CHECK(events_of(ran, "start").empty());
  CHECK(slept_before_start(refused) == 4);
  CHECK(!std::filesystem::exists(kis_account_dir(refused) / "journal.jsonl"));
}

TEST_CASE("an account run waits out KIS's token minute once per session then starts or names the minute") {
  SECTION("one token refusal: the run waits 61 s and starts") {
    PaperFixture fixture;
    fixture.write_config(kis_config());
    fixture.arm();
    one_decision();
    const auto ran = run_account_child(
      fixture,
      [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
        client.fail_next(atl::mock::Call::WORKING_ORDERS, token_refusal());
      },
      false, sleeps_before_start(fixture)
    );
    CHECK(ran.exit_ == 0);
    REQUIRE(!ran.events_.empty());
    CHECK(ran.events_.front().at("event").as_string() == "start");
    CHECK(slept_before_start(fixture) == 61);
    const auto noted = slurp(fixture.dir() / "account.stderr");
    CHECK(
      noted.find("KIS issues one access token a minute and refused this one; reading again in 61 s") !=
      std::string::npos
    );
  }
  SECTION("a second token refusal on the same read or a later one returns at once and names the minute") {
    for (const auto second : {atl::mock::Call::WORKING_ORDERS, atl::mock::Call::HOLDINGS}) {
      PaperFixture fixture;
      fixture.write_config(kis_config());
      fixture.arm();
      one_decision();
      const auto ran = run_account_child(
        fixture,
        [second](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
          client.fail_next(atl::mock::Call::WORKING_ORDERS, token_refusal());
          client.fail_next(second, token_refusal());
        },
        false, sleeps_before_start(fixture)
      );
      CHECK(ran.exit_ == 1);
      CHECK(code_of(ran) == "run.account_unreadable");
      CHECK(next_of_reply(ran) == cmdatl::START_TOKEN_NEXT);
      CHECK(events_of(ran, "start").empty());
      CHECK(slept_before_start(fixture) == 61);
    }
  }
  SECTION("the market-data session has its own wait on a demo account and shares the trading one's otherwise") {
    for (const bool own : {true, false}) {
      PaperFixture fixture;
      fixture.write_config(kis_config());
      fixture.arm();
      one_decision();
      const auto ran = run_account_child(
        fixture,
        [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
          client.fail_next(atl::mock::Call::WORKING_ORDERS, token_refusal());
          client.fail_next(atl::mock::Call::CALENDAR, token_refusal());
        },
        false, sleeps_before_start(fixture), own
      );
      if (own) {
        CHECK(ran.exit_ == 0);
        REQUIRE(!ran.events_.empty());
        CHECK(ran.events_.front().at("event").as_string() == "start");
        CHECK(slept_before_start(fixture) == 122);
      } else {
        CHECK(ran.exit_ == 1);
        CHECK(code_of(ran) == "run.reference_unreadable");
        CHECK(next_of_reply(ran) == cmdatl::START_TOKEN_NEXT);
        CHECK(slept_before_start(fixture) == 61);
      }
    }
  }
}

TEST_CASE("an account run reads reference data again after a rate refusal and notes the wait while it waits") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  one_decision();
  const auto dir = fixture.dir();
  auto ran = run_account_child(
    fixture,
    [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
      client.fail_next(atl::mock::Call::INSTRUMENT, rate_refusal());
    },
    false,
    [dir](std::chrono::nanoseconds) {
      if (!std::filesystem::exists(dir / "first-sleep")) {
        const bool noted = slurp(dir / "account.stderr").find("request rate") != std::string::npos;
        std::ofstream(dir / "first-sleep") << (noted ? "noted" : "silent");
      }
    }
  );
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(ran.events_.front().at("event").as_string() == "start");
  CHECK(slurp(dir / "first-sleep") == "noted");

  PaperFixture refused;
  refused.write_config(kis_config());
  refused.arm();
  one_decision();
  ran = run_account_child(refused, [](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
    client.fail_next(atl::mock::Call::INSTRUMENT, rate_refusal(), 3);
  });
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran) == "run.reference_unreadable");
  CHECK(next_of_reply(ran) == cmdatl::START_RATE_NEXT);
  CHECK(events_of(ran, "start").empty());
}

TEST_CASE("a signal during a wait before the start answers interrupted with nothing started or placed") {
  for (const auto call : {atl::mock::Call::WORKING_ORDERS, atl::mock::Call::INSTRUMENT}) {
    PaperFixture fixture;
    fixture.write_config(kis_config());
    fixture.arm();
    one_decision();
    const auto ran = run_account_child(
      fixture,
      [call](atl::mock::Market&, atl::mock::MockClient& client, auto&) {
        client.fail_next(call, call == atl::mock::Call::INSTRUMENT ? rate_refusal() : token_refusal());
      },
      false,
      [](std::chrono::nanoseconds) {
        ::raise(SIGINT);
      }
    );
    CHECK(ran.exit_ == 130);
    CHECK(code_of(ran) == "interrupted");
    CHECK(events_of(ran, "start").empty());
    CHECK(events_of(ran, "placed").empty());
  }
}
#endif
