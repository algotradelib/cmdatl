// The account and order commands on a broker's account: the command path on a stand-in setup (the library's mock
// session and an in-memory login store, so no case can reach a broker), and KIS's own setup on made-up logins, which
// stops at the logins or at a held lock. Every login here is made up.

#include "./broker-support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>

using namespace support;
using namespace broker_support;
namespace atl = algotradelib;
namespace runner = algotradelib::runner;

TEST_CASE("on a KIS account the clock commands are refused before any login is read") {
  const KisFixture fixture;
  StandIn setup;
  for (const auto& args : std::vector<std::vector<std::string>>{{"clock"}, {"clock", "advance", "--minutes", "1"}}) {
    INFO(cmdatl::join(args));
    const auto ran = run_on(setup, args);
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "unsupported.broker");
    CHECK(reply_of(ran.out_).at("env").as_string() == "kis-demo");
    const auto error = reply_of(ran.out_).at("error").as_object();
    CHECK(error.at("message").as_string().find("are the mock account's") != std::string::npos);
    CHECK(error.at("next").as_string() == "cmdatl status");
  }
  CHECK(setup.identified_ == 0);
  CHECK(setup.logins_.asked_.empty());
  CHECK(!std::filesystem::exists(fixture.account()));
  // Without the command path handed in (every test but these), a KIS account answers as before, opening nothing.
  const auto plain = fixture.paper_.json({"status"});
  CHECK(plain.exit_ == 1);
  CHECK(code_of(plain.out_) == "unsupported.broker");
  CHECK(!std::filesystem::exists(fixture.account()));
}

TEST_CASE("a read on a broker's account answers from its sessions, as of one instant, and sends nothing") {
  const KisFixture fixture;
  StandIn setup;
  auto& session = setup.trading();
  // A working order on the account, and a journal owing it and one the broker does not list.
  const auto resting = session.place(order("005930", true, "1", "70000"));
  REQUIRE(resting);
  const auto working = session.working_orders();
  REQUIRE(working);
  const auto listed = working.value().orders_.front().order_.order_id_;
  fixture.journal({
    place_entry("j1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:01")),
    place_entry("j1", runner::JournalOutcome::ACCEPTED, kst("2024-03-26T09:01"), listed),
    place_entry("j2", runner::JournalOutcome::PENDING, kst("2024-03-26T09:02")),
    place_entry("j2", runner::JournalOutcome::ACCEPTED, kst("2024-03-26T09:02"), "999"),
  });
  // One order-list call serves the owed rows and the order rows: a second would answer the fault.
  setup.client_->repeat_last(atl::mock::Call::WORKING_ORDERS);
  setup.client_->fail_next(atl::mock::Call::WORKING_ORDERS, marker("second-list"));
  setup.client_->fail_next(atl::mock::Call::PLACE, marker("place"));
  setup.client_->fail_next(atl::mock::Call::CANCEL, marker("cancel"));
  const auto history = setup.market_->order_history(atl::OrderHistoryQuery{}).orders_.size();
  const auto ran = run_on(setup, {"status"});
  INFO(ran.out_ << ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto body = reply_of(ran.out_);
  CHECK(body.at("env").as_string() == "kis-demo");
  CHECK(body.at("now").as_string() == "2024-03-26T10:00:00+09:00");
  CHECK(body.at("phase").as_string() == "open");
  CHECK(body.at("cash").is_null());
  CHECK(body.at("data_end").is_null());
  CHECK(!body.at("buying_power").as_string().empty());
  CHECK(body.at("capabilities").as_object().at("clock").as_bool() == false);
  CHECK(body.at("orders_left_today").as_int64() == 1);
  CHECK(body.at("state_dir").as_string() == fixture.account().string());
  CHECK(body.at("orders").as_array().size() == 1);
  const auto& owed = body.at("owed").as_array();
  REQUIRE(owed.size() == 2);
  // Two owed on one symbol are held for the operator, whose order release now reaches this account.
  CHECK(owed[0].as_object().at("broker_status").as_string() == "pending");
  CHECK(owed[0].as_object().at("next").as_string().find("cmdatl order release 005930") != std::string::npos);
  CHECK(owed[1].as_object().at("broker_status").is_null());
  CHECK(owed[1].as_object().at("next").as_string().find("cmdatl order release 005930") != std::string::npos);
  CHECK(body.at("quotes").as_array().size() == 2);
  CHECK(
    StandIn::unconsumed(
      [&] {
        return session.working_orders();
      },
      "second-list"
    )
  );
  CHECK(
    StandIn::unconsumed(
      [&] {
        return session.place(order("005930", true, "1", "70000"));
      },
      "place"
    )
  );
  CHECK(setup.market_->order_history(atl::OrderHistoryQuery{}).orders_.size() == history);
  CHECK(setup.clock_calls_ == 1);
  // The owner record names the command, as on the mock account.
  CHECK(reply_of(slurp(fixture.account() / "owner.json")).at("what").as_string() == "status");
  // The other reads, each with the place and cancel faults still waiting.
  const auto account = run_on(setup, {"account", "--symbol", "005930"});
  INFO(account.out_);
  CHECK(account.exit_ == 0);
  CHECK(reply_of(account.out_).at("owed").as_array().size() == 2);
  const auto quote = run_on(setup, {"quote", "005930", "NOPE"});
  INFO(quote.out_);
  CHECK(quote.exit_ == 0);
  const auto rows = reply_of(quote.out_).at("quotes").as_array();
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].as_object().at("orders_accepted_now").is_bool());
  CHECK(
    words(rows[1].as_object().at("not_accepted")) ==
    std::vector<std::string>{"not_armed", "symbol_not_allowed", "no_price"}
  );
  CHECK(run_on(setup, {"order", "list"}).exit_ == 0);
  atl::AmendInstruction whole;
  whole.all_remaining_ = true;
  CHECK(
    StandIn::unconsumed(
      [&] {
        return session.cancel(resting.value().ref_, whole);
      },
      "cancel"
    )
  );
  CHECK(setup.market_->order_history(atl::OrderHistoryQuery{}).orders_.size() == history);
}

TEST_CASE("a read's phase, allowance and quotes all belong to the one instant taken after the calendar") {
  struct Edge {
    std::string before_;
    std::string after_;
    std::string phase_;
    std::int64_t left_;
  };
  // The journal placed one order on 2024-03-25: the 25th has two left, the 26th three.
  for (
    const auto& edge : std::vector<Edge>{
      {"2024-03-26T08:59", "2024-03-26T09:00", "pre_open", 3},
      {"2024-03-26T15:29", "2024-03-26T15:30", "open", 3},
      {"2024-03-25T23:59", "2024-03-26T00:00", "post_close", 2},
      {"2024-03-26T00:00", "2024-03-25T23:59", "pre_open", 3},
      {"2024-03-28T10:00", "2024-03-27T10:00", "unknown", 3},
    }
  ) {
    INFO(edge.before_);
    const KisFixture fixture;
    fixture.journal({place_entry("j0", runner::JournalOutcome::PENDING, kst("2024-03-25T10:00"))});
    StandIn setup;
    // Any second reading of the clock, as by a slow read after the instant, would give the other side of the edge.
    setup.times_ = {kst(edge.before_), kst(edge.after_)};
    const auto ran = run_on(setup, {"status"});
    INFO(ran.out_);
    REQUIRE(ran.exit_ == 0);
    const auto body = reply_of(ran.out_);
    CHECK(body.at("now").as_string() == cmdatl::kst_iso(kst(edge.before_)));
    CHECK(body.at("phase").as_string() == edge.phase_);
    CHECK(body.at("orders_left_today").as_int64() == edge.left_);
    const auto closed = edge.phase_ == "unknown";
    for (const auto& row : body.at("quotes").as_array()) {
      const auto failing = words(row.as_object().at("not_accepted"));
      CHECK((std::find(failing.begin(), failing.end(), "market_closed") != failing.end()) == closed);
    }
    CHECK(setup.clock_calls_ == 1);
  }
  // The calendar is read before the clock: its fault is taken by then.
  const KisFixture fixture;
  StandIn setup;
  setup.probe_calendar_ = true;
  setup.client_->fail_next(atl::mock::Call::CALENDAR, marker("calendar-first"));
  const auto ran = run_on(setup, {"status"});
  INFO(ran.out_);
  CHECK(ran.exit_ == 0);
  CHECK(reply_of(ran.out_).at("phase").as_string() == "unknown");
  CHECK(setup.calendar_before_clock_);
}

TEST_CASE("a read on a broker's account takes the login's account, and answers busy at once beside a run") {
  const KisFixture fixture("");
  StandIn setup;
  // No number in the operator's file: the lock and journal are the login's account's, the ones a run takes.
  const auto ran = run_on(setup, {"order", "list"});
  INFO(ran.out_);
  REQUIRE(ran.exit_ == 0);
  CHECK(std::filesystem::is_regular_file(fixture.account() / "lock"));
  CHECK(!std::filesystem::exists(fixture.account("kis-demo")));
  // A run holding the account, its owner record naming it: busy at once, and nothing read from the broker.
  cmdatl::OwnerLock run_lock(fixture.account().string());
  REQUIRE(
    run_lock.acquire(
      std::chrono::milliseconds(100),
      [] {
        return false;
      },
      [](const std::string&) {}
    ) == cmdatl::OwnerLock::Outcome::HELD
  );
  spit(
    fixture.account() / "owner.json",
    R"({"role":"agent","pid":4242,"what":"run r1 pid 4242"})"
    "\n"
  );
  setup.client_->fail_next(atl::mock::Call::CALENDAR, marker("calendar"));
  setup.client_->fail_next(atl::mock::Call::WORKING_ORDERS, marker("list"));
  const auto busy = run_on(setup, {"status"});
  CHECK(busy.err_.find("waiting for the account") == std::string::npos);
  CHECK(busy.exit_ == 1);
  CHECK(code_of(busy.out_) == "busy.locked");
  CHECK(reply_of(busy.out_).at("error").as_object().at("message").as_string().find("run r1") != std::string::npos);
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().calendar(atl::Country::KR);
      },
      "calendar"
    )
  );
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().working_orders();
      },
      "list"
    )
  );
}

TEST_CASE("the lock waits for a command's record and not for a run's") {
  const ScratchDir dir;
  cmdatl::OwnerLock held(dir.path().string());
  const auto never = [] {
    return false;
  };
  REQUIRE(held.acquire(std::chrono::milliseconds(0), never, [](const std::string&) {
  }) == cmdatl::OwnerLock::Outcome::HELD);
  for (
    const auto& [what, waits] : std::vector<std::pair<std::string, bool>>{{"status", true}, {"run r1 pid 1", false}}
  ) {
    INFO(what);
    spit(dir.path() / "owner.json", "{\"pid\":1,\"what\":\"" + what + "\"}\n");
    cmdatl::OwnerLock waiter(dir.path().string());
    bool told = false;
    const auto start = std::chrono::steady_clock::now();
    CHECK(waiter.acquire(std::chrono::milliseconds(200), never, [&told](const std::string&) {
      told = true;
    }) == cmdatl::OwnerLock::Outcome::BUSY);
    CHECK(told == waits);
    CHECK((!waits || std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(200)));
  }
}

TEST_CASE("a broker read that fails answers account.unreadable with the broker's code, and a stop stops the reads") {
  const KisFixture fixture;
  StandIn setup;
  setup.client_->fail_next(atl::mock::Call::HOLDINGS, marker("EGW00201"));
  const auto failed = run_on(setup, {"account"});
  CHECK(failed.exit_ == 1);
  CHECK(code_of(failed.out_) == "account.unreadable");
  const auto error = reply_of(failed.out_).at("error").as_object();
  CHECK(error.at("message").as_string() == "the mock broker did not answer holdings (code EGW00201)");
  CHECK(error.at("retry").as_bool());
  // The stop asked for after the calendar and the holdings read: no further read, the interrupted reply.
  setup.client_->fail_next(atl::mock::Call::BUYING_POWER, marker("power"));
  setup.stop_after_ = setup.checks_ + 2;
  const auto stopped = run_on(setup, {"account"});
  CHECK(stopped.exit_ == 130);
  CHECK(code_of(stopped.out_) == "interrupted");
  setup.stop_after_ = -1;
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().buying_power(atl::Currency::krw());
      },
      "power"
    )
  );
}

TEST_CASE("a broker's account reads its journal: absent is empty, a damaged one or another account's is unknown") {
  const KisFixture fixture;
  StandIn setup;
  CHECK(run_on(setup, {"account"}).exit_ == 0);
  CHECK(!std::filesystem::exists(fixture.account() / "journal.jsonl"));
  fixture.journal({}, "kis:demo:99999999-01");
  const auto foreign = run_on(setup, {"account"});
  CHECK(foreign.exit_ == 4);
  CHECK(code_of(foreign.out_) == "unknown.journal_damaged");
  CHECK(reply_of(foreign.out_).contains("positions"));
  std::filesystem::remove(fixture.account() / "journal.jsonl");
  fixture.journal({place_entry("j0", runner::JournalOutcome::PENDING, kst("2024-03-26T09:00"))});
  spit(fixture.account() / "journal.jsonl", slurp(fixture.account() / "journal.jsonl") + "{\"torn\":");
  const auto damaged = run_on(setup, {"status"});
  CHECK(damaged.exit_ == 4);
  CHECK(code_of(damaged.out_) == "unknown.journal_damaged");
  CHECK(reply_of(damaged.out_).at("orders_left_today").is_null());
}

TEST_CASE("KIS's setup refuses a login that is not the operator file's account, and never repeats a login") {
  const KisFixture fixture;
  Logins logins;
  cmdatl::BuiltinSetup<atl::kis::KisBlockingClient> setup(&logins);
  const auto refused = [&](const std::string& demo, const std::string& real, const std::string& words) {
    logins.items_ = {{"kis/demo", demo}, {"kis/real", real}};
    const auto ran = run_on(setup, {"status"});
    INFO(ran.out_);
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "auth.account_mismatch");
    CHECK(reply_of(ran.out_).at("error").as_object().at("message").as_string().find(words) != std::string::npos);
  };
  refused(
    R"({"app_key":"k","app_secret":"MADE-UP-DEMO-SECRET","account_number":"11112222","demo":true})", REAL_LOGIN,
    "is not the operator file's account.number"
  );
  refused(DEMO_LOGIN, DEMO_LOGIN, "is a demo login");
  refused(R"({"app_key":"k","app_secret":"MADE-UP-DEMO-SECRET")", REAL_LOGIN, "could not be read");
  refused(DEMO_LOGIN, R"({"app_key":"MADE-UP-REAL-SECRET"})", "could not be read");
  CHECK(!std::filesystem::exists(fixture.account()));
}

TEST_CASE("KIS's setup builds its sessions without sending, and answers busy beside a run before any request") {
  const KisFixture fixture;
  // The token folder exists and is not private, so the library's store would refuse any request before it is sent.
  const auto tokens = fixture.paper_.dir() / "cache" / "cmdatl" / "tokens";
  std::filesystem::create_directories(tokens);
  REQUIRE(::chmod(tokens.c_str(), 0755) == 0);
  std::filesystem::create_directories(fixture.account());
  cmdatl::OwnerLock run_lock(fixture.account().string());
  REQUIRE(
    run_lock.acquire(
      std::chrono::milliseconds(100),
      [] {
        return false;
      },
      [](const std::string&) {}
    ) == cmdatl::OwnerLock::Outcome::HELD
  );
  spit(
    fixture.account() / "owner.json",
    R"({"role":"agent","pid":4242,"what":"run r1 pid 4242"})"
    "\n"
  );
  Logins logins;
  logins.items_ = {{"kis/demo", DEMO_LOGIN}, {"kis/real", REAL_LOGIN}};
  cmdatl::BuiltinSetup<atl::kis::KisBlockingClient> setup(&logins);
  const auto ran = run_on(setup, {"status"});
  INFO(ran.out_);
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "busy.locked");
  CHECK(logins.asked_ == std::vector<std::string>{"kis/demo", "kis/real"});
  CHECK(std::filesystem::is_empty(tokens));
  CHECK(std::filesystem::is_empty(fixture.paper_.dir() / "cache" / "cmdatl" / "calendars"));
}

TEST_CASE("every login refusal of KIS's setup is declared by the four reads") {
  const KisFixture fixture;
  const cmdatl::Table table(cmdatl::command_list());
  const std::vector<std::vector<std::string>> reads{{"status"}, {"account"}, {"quote", "005930"}, {"order", "list"}};
  struct Refusal {
    std::map<std::string, std::string> items_;
    std::optional<cmdatl::auth::StoreStatus> failing_;
    std::string code_;
  };
  const std::string other =
    R"({"app_key":"k","app_secret":"MADE-UP-DEMO-SECRET","account_number":"11112222","demo":true})";
  for (
    const auto& refusal : std::vector<Refusal>{
      {{}, cmdatl::auth::StoreStatus::UNAVAILABLE, "auth.no_store"},
      {{}, cmdatl::auth::StoreStatus::LOCKED, "auth.store_locked"},
      {{}, std::nullopt, "auth.not_found"},
      {{{"kis/demo", DEMO_LOGIN}}, std::nullopt, "auth.not_found"},
      {{{"kis/demo", other}, {"kis/real", REAL_LOGIN}}, std::nullopt, "auth.account_mismatch"},
    }
  ) {
    for (const auto& args : reads) {
      INFO(refusal.code_ << ": " << cmdatl::join(args));
      Logins logins;
      logins.items_ = refusal.items_;
      logins.failing_ = refusal.failing_;
      cmdatl::BuiltinSetup<atl::kis::KisBlockingClient> setup(&logins);
      const auto ran = run_on(setup, args);
      CHECK(ran.exit_ == 1);
      CHECK(code_of(ran.out_) == refusal.code_);
      const auto errors = cmdatl::errors_of(*table.resolve(args).command_);
      CHECK(std::find(errors.begin(), errors.end(), refusal.code_) != errors.end());
    }
  }
  CHECK(!std::filesystem::exists(fixture.account()));
}

namespace {

/** @brief The mock's capabilities, but no buying power, as Alpaca states. */
struct NoPowerCaps : atl::BrokerCaps<atl::mock::MockClient> {
  /** @brief None. */
  static constexpr bool has_buying_power = false;
};

/** @brief A mock session that states no buying power. */
struct NoPowerSession : atl::BlockingSession<atl::mock::MockClient> {
  using atl::BlockingSession<atl::mock::MockClient>::BlockingSession;
  /** @brief Its capabilities. */
  using Caps = NoPowerCaps;
};

/** @brief The stand-in's logins, clock and market, through a session that states no buying power. */
struct NoPowerSetup {
  /** @brief The stand-in. */
  StandIn& in_;
  /** @brief The session, on its own client over the stand-in's market. */
  NoPowerSession session_;

  /** @brief Builds it. @param in The stand-in. */
  explicit NoPowerSetup(StandIn& in)
  : in_(in), session_(std::make_shared<atl::mock::MockClient>(*in.market_), atl::mock::MockAccount{}) {}

  /** @brief As the stand-in's. @param config The file. @param effective Its number set. @return The refusal. */
  std::optional<cmdatl::Reply> identify(const cmdatl::OperatorConfig& config, cmdatl::OperatorConfig& effective) {
    return in_.identify(config, effective);
  }
  /** @brief The session. @return It. */
  NoPowerSession& trading() {
    return session_;
  }
  /** @brief The session. @return It. */
  NoPowerSession& market() {
    return session_;
  }
  /** @brief The stand-in's clock. @return Its next answer. */
  cmdatl::Clock::time_point now() {
    return in_.now();
  }
  /** @brief The stand-in's stop. @return Whether a stop was asked for. */
  bool stopped() {
    return in_.stopped();
  }
};

/** @brief The stand-in's desk, as the command path builds it: one mock session for trading and market data. */
using StandInDesk = cmdatl::BrokerDesk<HookedSession, HookedSession>;

/** @brief A broker's account opened in process on the stand-in's sessions, with the streams its output writes to. */
struct OpenBroker {
  /** @brief stdout. */
  std::ostringstream out_;
  /** @brief stderr. */
  std::ostringstream err_;
  /** @brief The output. */
  cmdatl::Output output_{out_, err_, cmdatl::Rendering{}, "kis-demo"};
  /** @brief The account. */
  std::unique_ptr<cmdatl::BasicAccount<StandInDesk>> account_;
};

/**
 * @brief Opens the operator file's KIS account on the stand-in, as the command path does: the lock, the desk, the
 *   journal, the calendar and the instant, the view.
 * @param setup The stand-in.
 * @return The account.
 */
std::unique_ptr<OpenBroker> open_broker(StandIn& setup) {
  auto opened = std::make_unique<OpenBroker>();
  const auto snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
  auto result = cmdatl::BasicAccount<StandInDesk>::open_broker(
    snapshot.config_.value(), opened->output_,
    [] {
      return false;
    },
    [&setup](const cmdatl::OperatorConfig& file, const cmdatl::OwnerLock& lock) {
      return std::make_unique<StandInDesk>(
        setup.trading(), setup.market(), file, lock,
        [&setup] {
          return setup.now();
        },
        [&setup] {
          return setup.stopped();
        }
      );
    }
  );
  REQUIRE(result.account_);
  opened->account_ = std::move(result.account_);
  opened->account_->desk().begin();
  opened->account_->view_at();
  return opened;
}

/**
 * @brief Whether the stand-in's market still lists an order.
 * @param setup The stand-in.
 * @param id The order's id.
 * @return Whether it does.
 */
bool still_working(StandIn& setup, const std::string& id) {
  for (const auto& row : setup.market_->working_orders().orders_) {
    if (row.order_.order_id_ == id) {
      return true;
    }
  }
  return false;
}

/** @brief Whether a type has a member call `place`, `cancel`, `amend`, `replace`, `session`, `inner` or `trading`. */
template <typename T, typename = void>
struct has_place : std::false_type {};
template <typename T>
struct has_place<T, std::void_t<decltype(std::declval<T&>().place(std::declval<atl::OrderRequest>()))>>
: std::true_type {};
template <typename T, typename = void>
struct has_cancel : std::false_type {};
template <typename T>
struct has_cancel<T, std::void_t<decltype(&T::cancel)>> : std::true_type {};
template <typename T, typename = void>
struct has_amend : std::false_type {};
template <typename T>
struct has_amend<T, std::void_t<decltype(&T::amend)>> : std::true_type {};
template <typename T, typename = void>
struct has_replace : std::false_type {};
template <typename T>
struct has_replace<T, std::void_t<decltype(&T::replace)>> : std::true_type {};
template <typename T, typename = void>
struct has_session : std::false_type {};
template <typename T>
struct has_session<T, std::void_t<decltype(&T::session)>> : std::true_type {};
template <typename T, typename = void>
struct has_inner : std::false_type {};
template <typename T>
struct has_inner<T, std::void_t<decltype(&T::inner)>> : std::true_type {};

using StandInView = StandInDesk::MarketSession;
static_assert(has_place<atl::BlockingSession<atl::mock::MockClient>>::value, "the detector sees a session's place");
static_assert(has_cancel<atl::BlockingSession<atl::mock::MockClient>>::value, "the detector sees a session's cancel");
static_assert(!has_place<StandInView>::value, "the market view cannot place");
static_assert(!has_cancel<StandInView>::value, "the market view cannot cancel");
static_assert(!has_amend<StandInView>::value && !has_replace<StandInView>::value, "the market view cannot amend");
static_assert(!has_session<StandInView>::value && !has_inner<StandInView>::value, "the market view lends no session");
static_assert(
  std::is_same<StandInDesk::Session, cmdatl::GuardedSession<HookedSession>>::value,
  "a broker's desk lends its trading session only as the placement boundary"
);
static_assert(
  std::is_same<decltype(std::declval<StandInDesk&>().market_session()), StandInView&>::value,
  "a broker's desk lends its market session only as the view"
);

} // namespace

TEST_CASE("the owed rows' next on a broker's account") {
  const auto listed = cmdatl::owed_next("005930", "named", std::string("pending"), false);
  CHECK(listed.find("still working at the broker") != std::string::npos);
  CHECK(listed.find("the operator, who cancels it in the broker's app") != std::string::npos);
  CHECK(listed.find("cmdatl order cancel 005930 withdraws it only when this account placed it") != std::string::npos);
  CHECK(listed.find("refused.unjournaled_order") != std::string::npos);
  const auto unlisted = cmdatl::owed_next("005930", "named", std::nullopt, false);
  CHECK(unlisted.find("not in the broker's order list now") != std::string::npos);
  CHECK(unlisted.find("cmdatl order reconcile 005930") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "unnamed", std::nullopt, false).find("never named it") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "operator", std::nullopt, false).find("order release 005930") != std::string::npos);
}

TEST_CASE("on a broker's account the table lets only order place and order cancel send and never the clock") {
  const auto problems_with = [](const std::string& name, const std::function<void(cmdatl::Command&)>& change) {
    auto list = cmdatl::command_list();
    for (auto& command : list) {
      if (cmdatl::join(command.path_) == name) {
        change(command);
      }
    }
    const cmdatl::Table table(list);
    std::vector<std::string> named;
    for (const auto& problem : cmdatl::check_table(table)) {
      if (problem.rfind(name + ":", 0) == 0) {
        named.push_back(problem);
      }
    }
    return named;
  };
  const cmdatl::Table shipped(cmdatl::command_list());
  CHECK(cmdatl::check_table(shipped).empty());
  // The shipped broker set, by name: a tenth command on a broker's account is an edit here too.
  std::vector<std::string> on_broker;
  std::vector<std::string> sending;
  for (const auto& command : shipped.commands()) {
    if (command.broker_) {
      on_broker.push_back(cmdatl::join(command.path_));
      if (command.may_send_) {
        sending.push_back(cmdatl::join(command.path_));
      }
    }
  }
  std::sort(on_broker.begin(), on_broker.end());
  CHECK(
    on_broker ==
    std::vector<std::string>{
      "account", "order cancel", "order list", "order place", "order plan", "order reconcile", "order release", "quote",
      "status"
    }
  );
  CHECK(sending == std::vector<std::string>{"order place", "order cancel"});
  // The mock clock's two never answer on a broker's account.
  for (const auto* name : {"clock", "clock advance"}) {
    INFO(name);
    CHECK(problems_with(name, [](cmdatl::Command& command) {
      command.broker_ = true;
    }).size() == 1);
  }
  // A command that is not an account command never answers there, sending or not.
  for (const auto* name : {"run", "env"}) {
    INFO(name);
    CHECK(!problems_with(name, [](cmdatl::Command& command) {
      command.broker_ = true;
    }).empty());
  }
  // Every other rule stays: a kind that sends is marked sends(), and only those kinds are.
  for (const auto* name : {"order place", "order cancel"}) {
    INFO(name);
    CHECK(problems_with(name, [](cmdatl::Command& command) {
      command.may_send_ = false;
    }).size() == 1);
  }
  for (const auto* name : {"order plan", "order reconcile"}) {
    INFO(name);
    CHECK(problems_with(name, [](cmdatl::Command& command) {
      command.may_send_ = true;
    }).size() == 2);
  }
  CHECK(problems_with("order release", [](cmdatl::Command& command) {
    command.mutates_ = false;
  }).size() == 1);
}

TEST_CASE("order plan on a broker's account plans on its sessions and starts its journal and sends nothing") {
  const KisFixture fixture;
  StandIn setup;
  // The reply is as of 10:00:00; the plan is issued, and lapses, on the live clock.
  setup.times_ = {kst("2024-03-26T10:00"), kst("2024-03-26T10:00") + std::chrono::seconds(40)};
  setup.client_->fail_next(atl::mock::Call::PLACE, marker("place"));
  setup.client_->fail_next(atl::mock::Call::CANCEL, marker("cancel"));
  const auto history = setup.market_->order_history(atl::OrderHistoryQuery{}).orders_.size();
  REQUIRE(!std::filesystem::exists(fixture.account() / "journal.jsonl"));
  const auto ran = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
  INFO(ran.out_);
  REQUIRE(ran.exit_ == 0);
  const auto body = reply_of(ran.out_);
  CHECK(body.at("env").as_string() == "kis-demo");
  CHECK(body.at("reason").as_string() == "plan");
  CHECK(body.at("would").as_string() == "buy");
  CHECK(body.at("now").as_string() == "2024-03-26T10:00:00+09:00");
  CHECK(body.at("expires_at").as_string() == "2024-03-26T10:01:40+09:00");
  CHECK(body.at("events").is_null());
  CHECK(body.at("owed").as_array().empty());
  // The first command that may write started the journal, under the lock: its header, of this account.
  const auto journal = runner::read_journal_file((fixture.account() / "journal.jsonl").string());
  CHECK(journal.readable());
  CHECK(journal.has_header_);
  CHECK(journal.scope_ == SCOPE);
  CHECK(journal.entries_.empty());
  CHECK(std::filesystem::is_regular_file(fixture.account() / "plans.json"));
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().place(order("005930", true, "1", "78000"));
      },
      "place"
    )
  );
  CHECK(setup.market_->order_history(atl::OrderHistoryQuery{}).orders_.size() == history);

  // On a broker that states no buying power cmdatl refuses a buy itself, before the guard: no plan, nothing sent.
  NoPowerSetup none(setup);
  const auto bought = run_on(none, {"order", "plan", "000660", "--target", "1", "--limit", "181000"});
  INFO(bought.out_);
  CHECK(bought.exit_ == 3);
  CHECK(code_of(bought.out_) == "refused.no_buying_power");
  const auto refusal = reply_of(bought.out_);
  CHECK(!refusal.contains("plan_id"));
  CHECK(refusal.at("error").at("retry") == false);
  CHECK(!refusal.at("error").at("next").as_string().empty());
  CHECK(setup.market_->order_history(atl::OrderHistoryQuery{}).orders_.size() == history);
  // A target already held runs once() end to end with no buying-power read; the account shows none.
  const auto met = run_on(none, {"order", "plan", "000660", "--target", "0", "--limit", "181000"});
  INFO(met.out_);
  CHECK(met.exit_ == 0);
  CHECK(reply_of(met.out_).at("reason") == "met");
  const auto read = run_on(none, {"account"});
  INFO(read.out_);
  REQUIRE(read.exit_ == 0);
  CHECK(reply_of(read.out_).at("buying_power").is_null());
}

TEST_CASE("a broker account's journal is started only when absent: an empty or foreign one is unknown and untouched") {
  for (const std::string& text : {std::string(), std::string("{\"torn\":")}) {
    INFO(text);
    const KisFixture fixture;
    StandIn setup;
    std::filesystem::create_directories(fixture.account());
    spit(fixture.account() / "journal.jsonl", text);
    const auto ran = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.journal_damaged");
    CHECK(slurp(fixture.account() / "journal.jsonl") == text);
    CHECK(!std::filesystem::exists(fixture.account() / "plans.json"));
  }
  const KisFixture fixture;
  StandIn setup;
  fixture.journal({}, "kis:demo:99999999-01");
  const auto before = slurp(fixture.account() / "journal.jsonl");
  const auto foreign = run_on(setup, {"order", "reconcile", "005930"});
  CHECK(foreign.exit_ == 4);
  CHECK(code_of(foreign.out_) == "unknown.journal_damaged");
  CHECK(slurp(fixture.account() / "journal.jsonl") == before);
}

TEST_CASE("a broker account's journal no command started reads empty after an order command") {
  const KisFixture fixture;
  StandIn setup;
  // Nothing owed and no symbol: reconcile opens no journal, and reading the view again finds none, as at open.
  const auto clean = run_on(setup, {"order", "reconcile"});
  INFO(clean.out_);
  CHECK(clean.exit_ == 0);
  CHECK(reply_of(clean.out_).at("reason").as_string() == "clean");
  // A plan refused before the journal is opened.
  const auto refused = run_on(setup, {"order", "plan", "035720", "--target", "1", "--limit", "50000"});
  INFO(refused.out_);
  CHECK(refused.exit_ == 3);
  CHECK(code_of(refused.out_) == "refused.symbol_not_allowed");
  CHECK(reply_of(refused.out_).at("owed").as_array().empty());
  CHECK(!std::filesystem::exists(fixture.account() / "journal.jsonl"));
}

namespace {

/**
 * @brief Breaks a journal as the integrity cases do: `torn` cuts its last line, `removed` moves it aside, `foreign`
 *   writes another account's header over it in place, `replaced` renames a new file holding only its own header over it.
 * @param journal The journal.
 * @param how How.
 */
void break_journal(const std::filesystem::path& journal, const std::string& how) {
  const auto text = slurp(journal);
  const auto aside = journal.string() + ".aside";
  if (how == "torn") {
    spit(journal, text.substr(0, text.size() - 5));
  } else if (how == "removed") {
    std::filesystem::rename(journal, aside);
  } else if (how == "foreign") {
    {
      runner::FileJournal other(aside, "kis:demo:99999999-01");
      REQUIRE(other.open());
    }
    spit(journal, slurp(aside));
  } else {
    spit(aside, text.substr(0, text.find('\n') + 1));
    std::filesystem::rename(aside, journal);
  }
}

} // namespace

TEST_CASE("on a broker's account a journal broken after the result turns a plan into unknown read or not") {
  const std::vector<std::string> plan{"order", "plan", "005930", "--target", "1", "--limit", "78000"};
  // The clock reads a plan makes: its last is the plan's issue, after once() returned and before the reply's end.
  const auto reads = [&plan] {
    const KisFixture fixture;
    StandIn setup;
    REQUIRE(run_on(setup, plan).exit_ == 0);
    return setup.clock_calls_;
  }();
  for (const std::string how : {"torn", "removed", "foreign", "replaced"}) {
    for (const bool listing_fails : {false, true}) {
      INFO(how << (listing_fails ? ", the owed listing stopped and failing" : ""));
      const KisFixture fixture;
      StandIn setup;
      const auto journal = fixture.account() / "journal.jsonl";
      setup.hook_at_ = reads - 1;
      setup.hook_ = [&journal, &how] {
        break_journal(journal, how);
      };
      if (listing_fails) {
        setup.stop_after_ = 1;
        setup.client_->repeat_last(atl::mock::Call::WORKING_ORDERS);
        setup.client_->fail_next(atl::mock::Call::WORKING_ORDERS, marker("owed-list"));
      }
      const auto ran = run_on(setup, plan);
      INFO(ran.out_);
      CHECK(ran.exit_ == 4);
      CHECK(
        code_of(ran.out_) == (how == "torn" || how == "foreign" ? "unknown.journal_damaged" : "unknown.journal_missing")
      );
      const auto body = reply_of(ran.out_);
      CHECK(body.at("owed").is_null());
      CHECK(body.contains("plan_id"));
      CHECK(!body.contains("next"));
    }
  }
}

TEST_CASE("order plan and order reconcile and order release work on a broker's account and send nothing") {
  const KisFixture fixture;
  fixture.paper_.arm();
  StandIn setup;
  setup.market_->advance_to(kst("2024-03-26T09:30"));
  const auto resting = setup.trading().place(order("005930", true, "1", "78000"));
  REQUIRE(resting);
  const auto listed = setup.market_->working_orders().orders_.front().order_.order_id_;
  // An owned order on 005930, and a placement on 000660 the broker never named.
  auto other = place_entry("j2", runner::JournalOutcome::PENDING, kst("2024-03-26T09:40"));
  other.instrument_ = cmdatl::kr("000660");
  other.price_ = atl::Decimal64(181000, 0);
  auto pending = place_entry("j1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:30"));
  auto accepted = place_entry("j1", runner::JournalOutcome::ACCEPTED, kst("2024-03-26T09:30"), listed);
  pending.price_ = accepted.price_ = atl::Decimal64(78000, 0);
  fixture.journal({pending, accepted, other});
  setup.client_->fail_next(atl::mock::Call::PLACE, marker("place"));
  setup.client_->fail_next(atl::mock::Call::CANCEL, marker("cancel"));
  // The resting order meets a target of 1; a target of 0 would withdraw it, which a plan only reports.
  for (const auto* target : {"1", "0"}) {
    const auto planned = run_on(setup, {"order", "plan", "005930", "--target", target, "--limit", "78000"});
    INFO(planned.out_);
    CHECK(planned.exit_ == 0);
  }
  const auto everything = run_on(setup, {"order", "reconcile"});
  INFO(everything.out_);
  CHECK(everything.exit_ == 4);
  const auto reconciled = run_on(setup, {"order", "reconcile", "005930"});
  INFO(reconciled.out_);
  CHECK(reconciled.exit_ == 3);
  CHECK(code_of(reconciled.out_) == "refused.awaiting");
  const auto body = reply_of(reconciled.out_);
  CHECK(body.at("checked").as_array().at(0).as_object().at("state").as_string() == "working");
  CHECK(body.at("events").is_null());
  // A named order is refused whatever its age; the unnamed one on 000660 is released on the approval naming it.
  spit(fixture.paper_.dir() / "paper.release", "005930 j1\n");
  const auto named = run_on(setup, {"order", "release", "005930", "--reason", "read KIS's app"});
  INFO(named.out_);
  CHECK(named.exit_ == 3);
  CHECK(code_of(named.out_) == "refused.named");
  spit(fixture.paper_.dir() / "paper.release", "000660 j2\n");
  const auto released = run_on(setup, {"order", "release", "000660", "--reason", "read KIS's app"});
  INFO(released.out_);
  REQUIRE(released.exit_ == 0);
  CHECK(words(reply_of(released.out_).at("released")) == std::vector<std::string>{"j2"});
  CHECK(still_working(setup, listed));
  // Neither verb was asked of the broker: both faults still wait for their call.
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().place(order("005930", true, "1", "78000"));
      },
      "place"
    )
  );
  atl::AmendInstruction whole;
  whole.all_remaining_ = true;
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().cancel(resting.value().ref_, whole);
      },
      "cancel"
    )
  );
}

TEST_CASE("on a broker's account once() withdraws only what the run's ownership rule proves on both paths") {
  const auto at = kst("2024-03-26T09:30");
  const auto yesterday = at - std::chrono::hours(24);
  // One placement's two lines, against the order resting on 005930: buy 1 at 78000, placed today.
  struct Case {
    std::string name_;
    bool owned_{false};
    cmdatl::Clock::time_point sent_;
    cmdatl::Clock::time_point named_at_;
    runner::JournalOutcome naming_{runner::JournalOutcome::ACCEPTED};
    bool rival_{false};
    atl::OrderSide side_{atl::OrderSide::BUY};
    std::int64_t quantity_{1};
    std::int64_t price_{78000};
    std::string symbol_{"005930"};
  };
  std::vector<Case> cases(9);
  for (auto& c : cases) {
    c.sent_ = c.named_at_ = at;
  }
  cases[0].name_ = "placed and acknowledged today";
  cases[0].owned_ = true;
  cases[1].name_ = "placed and acknowledged yesterday under the number an order of today now carries";
  cases[1].sent_ = cases[1].named_at_ = yesterday;
  cases[2].name_ = "acknowledged today but sent yesterday";
  cases[2].sent_ = yesterday;
  cases[3].name_ = "named only by a recovery sweep";
  cases[3].naming_ = runner::JournalOutcome::RESOLVED;
  cases[4].name_ = "claimed by another placement's line too";
  cases[4].rival_ = true;
  cases[5].name_ = "another side";
  cases[5].side_ = atl::OrderSide::SELL;
  cases[6].name_ = "another quantity";
  cases[6].quantity_ = 2;
  cases[7].name_ = "another price";
  cases[7].price_ = 77900;
  cases[8].name_ = "another instrument";
  cases[8].symbol_ = "000660";
  for (const auto& c : cases) {
    for (const bool withdraw_only : {true, false}) {
      INFO(c.name_ << (withdraw_only ? ", a cancel" : ", a placement's withdrawal"));
      const KisFixture fixture;
      fixture.paper_.arm();
      StandIn setup;
      // The reply's instant a day before the live clock: today, for the rule, is the live clock's.
      setup.times_ = {yesterday, kst("2024-03-26T10:00")};
      setup.market_->advance_to(at);
      REQUIRE(setup.trading().place(order("005930", true, "1", "78000")));
      const auto number = setup.market_->working_orders().orders_.front().order_.order_id_;
      auto pending = place_entry("j1", runner::JournalOutcome::PENDING, c.sent_);
      auto naming = place_entry("j1", c.naming_, c.named_at_, number);
      for (auto* entry : {&pending, &naming}) {
        entry->side_ = c.side_;
        entry->quantity_ = atl::Decimal64(c.quantity_, 0);
        entry->price_ = atl::Decimal64(c.price_, 0);
        entry->instrument_ = cmdatl::kr(c.symbol_);
      }
      std::vector<runner::JournalEntry> lines{pending, naming};
      if (c.rival_) {
        lines.insert(lines.begin(), place_entry("j0", runner::JournalOutcome::REJECTED, at, number));
        lines.insert(lines.begin(), place_entry("j0", runner::JournalOutcome::PENDING, at));
      }
      fixture.journal(lines);
      const auto opened = open_broker(setup);
      auto& account = *opened->account_;
      auto journal = cmdatl::orders_detail::open_journal(account);
      REQUIRE(journal.journal_);
      auto config = cmdatl::once_config(account.config(), cmdatl::risk_limits(account.config(), {"005930"}, 3));
      cmdatl::sending::OnceRequest request;
      request.instrument_ = cmdatl::kr("005930");
      if (withdraw_only) {
        config.withdraw_only_ = true;
        request.target_quantity_ = atl::Decimal64(0, 0);
      } else {
        request.target_quantity_ = atl::Decimal64(5, 0);
        request.limit_ = atl::Decimal64(78000, 0);
      }
      const auto result = cmdatl::sending::once(account.desk(), *journal.journal_, account.lock(), request, config);
      INFO(result.reason_ << ": " << result.detail_);
      if (c.owned_) {
        CHECK(result.reason_ == "withdrawal_sent");
        CHECK(account.desk().sends() == 1);
        CHECK(!still_working(setup, number));
      } else {
        CHECK(result.exit_ != cmdatl::sending::OnceExit::DONE);
        CHECK(account.desk().sends() == 0);
        CHECK(still_working(setup, number));
      }
    }
  }
}

TEST_CASE("on a broker's account a known result is kept when reading owed after it fails or stops") {
  // A named order the broker does not list: the plan waits (exit 3), and its owed rows read the order list again.
  const auto setup_with_owed = [](const KisFixture& fixture) {
    fixture.journal({
      place_entry("j1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:58")),
      place_entry("j1", runner::JournalOutcome::ACCEPTED, kst("2024-03-26T09:58"), "999"),
    });
  };
  const std::vector<std::string> plan{"order", "plan", "005930", "--target", "1", "--limit", "78000"};
  {
    const KisFixture fixture;
    setup_with_owed(fixture);
    StandIn setup;
    // The plan's own listing answers; the owed rows' listing does not.
    setup.client_->repeat_last(atl::mock::Call::WORKING_ORDERS);
    setup.client_->fail_next(atl::mock::Call::WORKING_ORDERS, marker("owed-list"));
    const auto ran = run_on(setup, plan);
    INFO(ran.out_);
    CHECK(ran.exit_ == 3);
    CHECK(code_of(ran.out_) == "refused.awaiting_confirmation");
    CHECK(reply_of(ran.out_).at("owed").is_null());
  }
  {
    const KisFixture fixture;
    setup_with_owed(fixture);
    StandIn setup;
    // A stop asked for after the calendar: the plan's result stands, and owed is not read.
    setup.stop_after_ = 1;
    const auto ran = run_on(setup, plan);
    INFO(ran.out_);
    CHECK(ran.exit_ == 3);
    CHECK(code_of(ran.out_) == "refused.awaiting_confirmation");
    CHECK(reply_of(ran.out_).at("owed").is_null());
  }
}

TEST_CASE("on a broker's account what is thrown inside order release is unknown (4) and inside order plan is not") {
  for (const bool stop : {true, false}) {
    INFO((stop ? "a stop" : "an exception"));
    {
      const KisFixture fixture;
      fixture.paper_.arm();
      fixture.journal({place_entry("j1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:00"))});
      spit(fixture.paper_.dir() / "paper.release", "005930 j1\n");
      StandIn setup;
      setup.throw_after_ = 1;
      setup.throw_stop_ = stop;
      const auto ran = run_on(setup, {"order", "release", "005930", "--reason", "read KIS's app"});
      INFO(ran.out_);
      CHECK(ran.exit_ == 4);
      CHECK(code_of(ran.out_) == "unknown.internal");
      CHECK(
        reply_of(ran.out_).at("error").as_object().at("next").as_string() ==
        "cmdatl account shows what is still owed; read it before releasing again"
      );
    }
    const KisFixture fixture;
    StandIn setup;
    setup.throw_after_ = 1;
    setup.throw_stop_ = stop;
    const auto ran = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
    INFO(ran.out_);
    CHECK(ran.exit_ == (stop ? 130 : 1));
    CHECK(code_of(ran.out_) == (stop ? "interrupted" : "internal.error"));
  }
}

TEST_CASE("on a broker's account a plan lapses on the live clock whatever instant the reply is as of") {
  const KisFixture fixture;
  fixture.paper_.arm();
  StandIn setup;
  setup.times_ = {kst("2024-03-26T10:00")};
  const auto planned = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
  INFO(planned.out_);
  REQUIRE(planned.exit_ == 0);
  const auto id = std::string(reply_of(planned.out_).at("plan_id").as_string());
  // As of 10:00:30, within its 60 s; on the live clock, after the snapshot's reads, 61 s old.
  setup.times_ = {
    kst("2024-03-26T10:00") + std::chrono::seconds(30), kst("2024-03-26T10:01") + std::chrono::seconds(1)
  };
  setup.clock_calls_ = 0;
  const auto ran = run_on(setup, {"order", "place", id});
  INFO(ran.out_);
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran.out_) == "refused.stale_plan");
  CHECK(reply_of(ran.out_).at("now").as_string() == "2024-03-26T10:00:30+09:00");
  CHECK(setup.trading().places_ == 0);
  CHECK(fixture.entries().empty());
  // The lapsed plan was never used: a new plan is the way on.
  CHECK(run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"}).exit_ == 0);
}

TEST_CASE("repeated order commands on a broker's account leave no descriptor open") {
  const KisFixture fixture;
  fixture.paper_.arm();
  StandIn setup;
  const std::vector<std::vector<std::string>> lines{
    {"order", "plan", "005930", "--target", "1", "--limit", "78000"},
    {"order", "reconcile", "005930"},
    {"order", "release", "005930", "--reason", "nothing owed"},
  };
  for (const auto& args : lines) {
    run_on(setup, args);
  }
  const auto before = open_fds();
  for (int i = 0; i < 12; ++i) {
    run_on(setup, lines[static_cast<std::size_t>(i) % lines.size()]);
  }
  CHECK(open_fds() == before);
}

TEST_CASE("on a broker's account a plan's age is taken after the snapshot's reads") {
  const KisFixture fixture;
  fixture.paper_.arm();
  StandIn setup;
  const auto issued = kst("2024-03-26T10:00");
  setup.times_ = {issued};
  const auto planned = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
  INFO(planned.out_);
  REQUIRE(planned.exit_ == 0);
  const auto id = std::string(reply_of(planned.out_).at("plan_id").as_string());
  // Every live reading before the snapshot's buying-power read is 30 s into the plan's 60; that read takes 31 more.
  setup.times_ = {issued + std::chrono::seconds(30)};
  setup.clock_calls_ = 0;
  setup.trading().before_power_ = [&setup, issued] {
    setup.times_ = {issued + std::chrono::seconds(61)};
  };
  const auto ran = run_on(setup, {"order", "place", id});
  INFO(ran.out_);
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran.out_) == "refused.stale_plan");
  CHECK(setup.trading().places_ == 0);
  CHECK(fixture.entries().empty());
}

TEST_CASE("a broker account's first write against a journal that appeared or was replaced after the open") {
  const std::vector<std::string> plan{"order", "plan", "005930", "--target", "1", "--limit", "78000"};
  struct Case {
    std::string name_;
    bool present_at_open_;
    std::string scope_;
  };
  for (
    const auto& c : std::vector<Case>{
      {"appeared, this account's", false, SCOPE},
      {"appeared, another account's", false, "kis:demo:99999999-01"},
      {"replaced by a copy of its header", true, SCOPE},
    }
  ) {
    INFO(c.name_);
    const KisFixture fixture;
    StandIn setup;
    const auto journal = fixture.account() / "journal.jsonl";
    if (c.present_at_open_) {
      fixture.journal({});
    }
    std::string written;
    // The first clock reading is the instant, after the account opened and before the first write.
    setup.hook_at_ = 0;
    setup.hook_ = [&journal, &c, &written] {
      const auto aside = journal.string() + ".new";
      {
        runner::FileJournal other(aside, c.scope_);
        REQUIRE(other.open());
      }
      written = slurp(aside);
      std::filesystem::rename(aside, journal);
    };
    setup.client_->fail_next(atl::mock::Call::PLACE, marker("place"));
    const auto ran = run_on(setup, plan);
    INFO(ran.out_);
    // The file that appeared is never truncated or rewritten: at most this account's own lines follow its header.
    CHECK(slurp(journal).rfind(written, 0) == 0);
    if (c.present_at_open_) {
      // Refused at the first write, before any decision: no plan is issued.
      CHECK(ran.exit_ == 4);
      CHECK(code_of(ran.out_) == "unknown.journal_missing");
      CHECK(slurp(journal) == written);
      CHECK(!reply_of(ran.out_).contains("plan_id"));
      CHECK(!std::filesystem::exists(fixture.account() / "plans.json"));
    } else if (c.scope_ != SCOPE) {
      CHECK(ran.exit_ == 1);
      CHECK(code_of(ran.out_) == "journal.unopened");
      CHECK(slurp(journal) == written);
    } else {
      CHECK(ran.exit_ == 0);
    }
    CHECK(
      StandIn::unconsumed(
        [&] {
          return setup.trading().place(order("005930", true, "1", "78000"));
        },
        "place"
      )
    );
  }
}

namespace {

/**
 * @brief A plan of 1 share of 005930 on the stand-in, armed, its market and the live clock at one instant.
 * @param fixture The account.
 * @param setup The stand-in.
 * @param at The instant.
 * @return The plan id.
 */
std::string ready_plan(
  const KisFixture& fixture, StandIn& setup, cmdatl::Clock::time_point at = kst("2024-03-26T10:00")
) {
  fixture.paper_.arm();
  // The market's own clock at the stand-in's live instant, so its holdings are fresh for the runner.
  setup.market_->advance_to(at);
  setup.times_ = {at};
  const auto planned = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
  INFO(planned.out_);
  REQUIRE(planned.exit_ == 0);
  return std::string(reply_of(planned.out_).at("plan_id").as_string());
}

/**
 * @brief An order this account placed and the broker acknowledged today: resting on 005930, its two journal lines
 *   written; the market is at 09:30 and the session's counts start again from 0.
 * @param fixture The account.
 * @param setup The stand-in.
 * @param prices Each order's limit, one order each, journal ids `j1`, `j2` and on.
 * @return The broker's numbers, in order.
 */
std::vector<std::string> owned_orders(
  const KisFixture& fixture, StandIn& setup, const std::vector<std::int64_t>& prices = {78000}
) {
  const auto at = kst("2024-03-26T09:30");
  setup.market_->advance_to(at);
  std::vector<std::string> numbers;
  std::vector<runner::JournalEntry> lines;
  for (std::size_t k = 0; k < prices.size(); ++k) {
    const auto placed = setup.trading().place(order("005930", true, "1", std::to_string(prices[k])));
    REQUIRE(placed);
    numbers.push_back(placed.value().ref_.primary_);
    const auto id = "j" + std::to_string(k + 1);
    auto pending = place_entry(id, runner::JournalOutcome::PENDING, at);
    auto accepted = place_entry(id, runner::JournalOutcome::ACCEPTED, at, numbers.back());
    pending.price_ = accepted.price_ = atl::Decimal64(prices[k], 0);
    lines.push_back(pending);
    lines.push_back(accepted);
  }
  fixture.journal(lines);
  setup.trading().places_ = 0;
  return numbers;
}

/**
 * @brief The journal's lines of one action, by outcome, in order.
 * @param entries The journal's entries.
 * @param action The action.
 * @return Each line's outcome.
 */
std::vector<runner::JournalOutcome> outcomes(
  const std::vector<runner::JournalEntry>& entries, runner::JournalAction action
) {
  std::vector<runner::JournalOutcome> out;
  for (const auto& entry : entries) {
    if (entry.action_ == action) {
      out.push_back(entry.outcome_);
    }
  }
  return out;
}

/**
 * @brief A reply's error `next`.
 * @param ran The command's output.
 * @return The text.
 */
std::string next_of(const Ran& ran) {
  return std::string(reply_of(ran.out_).at("error").as_object().at("next").as_string());
}

} // namespace

TEST_CASE("order place and order cancel are listed for a broker's account in the schema and the help") {
  for (const auto& name : {std::string("order.place"), std::string("order.cancel")}) {
    INFO(name);
    const auto entry = reply_of(run({"--json", "schema", name}).out_).at("commands").as_array().at(0).as_object();
    const auto errors = words(entry.at("errors"));
    for (const auto* code : {"auth.not_found", "auth.account_mismatch", "account.unreadable"}) {
      CHECK(std::find(errors.begin(), errors.end(), code) != errors.end());
    }
    CHECK(entry.at("may_send").as_bool());
    auto help = cmdatl::split(name, '.');
    help.push_back("--help");
    CHECK(run(help).out_.find("on a KIS account") != std::string::npos);
  }
  const auto clock = reply_of(run({"--json", "schema", "clock"}).out_).at("commands").as_array().at(0).as_object();
  const auto errors = words(clock.at("errors"));
  CHECK(std::find(errors.begin(), errors.end(), "auth.not_found") == errors.end());
}

TEST_CASE("on a broker's account reference data and every market read go to the market session") {
  const KisFixture fixture;
  StandIn setup;
  // As on a demo account: the trading session answers no market data, and is asked none.
  setup.split_ = true;
  setup.client_->fail_next(atl::mock::Call::INSTRUMENT, marker("trading-reference"));
  const auto id = ready_plan(fixture, setup);
  const auto placed = run_on(setup, {"order", "place", id});
  INFO(placed.out_);
  CHECK(placed.exit_ == 0);
  CHECK(reply_of(placed.out_).at("reason").as_string() == "placed");
  const auto cancelled = run_on(setup, {"order", "cancel", "005930"});
  INFO(cancelled.out_);
  CHECK(cancelled.exit_ == 0);
  CHECK(reply_of(cancelled.out_).at("reason").as_string() == "withdrawal_sent");
  CHECK(run_on(setup, {"status"}).exit_ == 0);
  CHECK(setup.trading().places_ == 1);
  CHECK(setup.trading().cancels_ == 1);
  CHECK(setup.trading().market_reads_ == 0);
  CHECK(setup.market_session_->market_reads_ > 0);
  CHECK(setup.market_session_->places_ + setup.market_session_->cancels_ == 0);
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().instrument(cmdatl::kr("005930"));
      },
      "trading-reference"
    )
  );
  // Reference data the market session cannot read refuses the plan; nothing is planned.
  auto unreadable = marker("market-reference");
  unreadable.message_ = "made-up reference failure";
  setup.market_client_->fail_next(atl::mock::Call::INSTRUMENT, unreadable);
  const auto unread = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
  INFO(unread.out_);
  CHECK(unread.exit_ == 3);
  CHECK(code_of(unread.out_) == "refused.reference_unread");
  CHECK(!reply_of(unread.out_).contains("plan_id"));
}

TEST_CASE("on a broker's account a phase refusal and a reconcile still owed give the broker's hints") {
  const KisFixture fixture;
  fixture.paper_.arm();
  StandIn setup;
  setup.times_ = {kst("2024-03-26T16:00")};
  const auto closed = run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"});
  INFO(closed.out_);
  CHECK(closed.exit_ == 3);
  CHECK(code_of(closed.out_) == "refused.phase");
  CHECK(next_of(closed) == cmdatl::broker_next("refused.phase"));
  CHECK(cmdatl::catalog_next("refused.phase").find("cmdatl clock advance") != std::string::npos);
  setup.times_ = {kst("2024-03-26T10:00")};
  owned_orders(fixture, setup);
  const auto owed = run_on(setup, {"order", "reconcile", "005930"});
  INFO(owed.out_);
  CHECK(owed.exit_ == 3);
  CHECK(code_of(owed.out_) == "refused.awaiting");
  CHECK(next_of(owed) == cmdatl::broker_next("refused.awaiting"));
  const auto row = reply_of(owed.out_).at("owed").as_array().at(0).as_object();
  CHECK(row.at("next").as_string().find("cmdatl order cancel 005930 withdraws it only when") != std::string::npos);
}

TEST_CASE("on a broker's account an accepted placement keeps its result through a signal and a failed owed read") {
  // The clock readings a placement makes: its last follows the broker's answer.
  const auto reads = [] {
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    const auto before = setup.clock_calls_;
    const auto first = run_on(setup, {"order", "place", id});
    INFO(first.out_);
    REQUIRE(first.exit_ == 0);
    return setup.clock_calls_ - before;
  }();
  const std::vector<std::string> cases{
    "a stop after the boundary's last check, while the request is in flight",
    "a stop just after the broker's answer",
    "the owed rows' order list failing",
  };
  for (std::size_t which = 0; which < cases.size(); ++which) {
    INFO(cases[which]);
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    if (which == 0) {
      // The account's calendar check, then the boundary's: a stop from the next one on.
      setup.stop_after_ = setup.checks_ + 2;
    } else if (which == 1) {
      setup.hook_at_ = setup.clock_calls_ + reads - 1;
      setup.hook_ = [&setup] {
        setup.stop_after_ = setup.checks_;
      };
    } else {
      // The snapshot's and once()'s listings answer; the owed rows' does not.
      setup.client_->repeat_last(atl::mock::Call::WORKING_ORDERS, 2);
      setup.client_->fail_next(atl::mock::Call::WORKING_ORDERS, marker("owed-list"));
    }
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 0);
    const auto body = reply_of(ran.out_);
    CHECK(body.at("reason").as_string() == "placed");
    CHECK(body.at("order").as_object().at("side").as_string() == "buy");
    CHECK(body.at("owed").is_null());
    CHECK(setup.trading().places_ == 1);
    const auto lines = fixture.entries();
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].outcome_ == runner::JournalOutcome::ACCEPTED);
    CHECK(lines[1].order_id_ == std::string(body.at("order").as_object().at("order_id").as_string()));
    // The order rests: reconcile holds it owed and sends nothing.
    setup.stop_after_ = -1;
    setup.hook_ = nullptr;
    const auto next = run_on(setup, {"order", "reconcile", "005930"});
    INFO(next.out_);
    CHECK(next.exit_ == 3);
    CHECK(code_of(next.out_) == "refused.awaiting");
    CHECK(setup.trading().places_ + setup.trading().cancels_ == 1);
  }
  {
    // An unknown result stays the unknown one, through a stop after it.
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    setup.client_->lose_next_reply(atl::mock::Call::PLACE);
    setup.stop_after_ = setup.checks_ + 2;
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.indeterminate");
    CHECK(setup.trading().places_ == 1);
    CHECK(outcomes(fixture.entries(), runner::JournalAction::PLACE).front() == runner::JournalOutcome::PENDING);
  }
}

TEST_CASE("on a broker's account an accepted cancellation keeps its result through a signal") {
  // The clock readings a cancellation makes: its last follows the broker's answer.
  const auto reads = [] {
    const KisFixture fixture;
    StandIn setup;
    owned_orders(fixture, setup);
    const auto before = setup.clock_calls_;
    REQUIRE(run_on(setup, {"order", "cancel", "005930"}).exit_ == 0);
    return setup.clock_calls_ - before;
  }();
  for (const bool after_answer : {false, true}) {
    INFO((after_answer ? "a stop just after the broker's answer" : "a stop while the request is in flight"));
    const KisFixture fixture;
    StandIn setup;
    const auto number = owned_orders(fixture, setup).front();
    if (after_answer) {
      setup.hook_at_ = setup.clock_calls_ + reads - 1;
      setup.hook_ = [&setup] {
        setup.stop_after_ = setup.checks_;
      };
    } else {
      // The account's calendar check passes; a cancellation is not checked for a stop; the next check sees it.
      setup.stop_after_ = setup.checks_ + 1;
    }
    const auto ran = run_on(setup, {"order", "cancel", "005930"});
    INFO(ran.out_);
    CHECK(ran.exit_ == 0);
    const auto body = reply_of(ran.out_);
    CHECK(body.at("reason").as_string() == "withdrawal_sent");
    CHECK(words(body.at("withdrawals")) == std::vector<std::string>{number});
    CHECK(body.at("owed").is_null());
    CHECK(setup.trading().cancels_ == 1);
    CHECK(!still_working(setup, number));
    CHECK(
      outcomes(fixture.entries(), runner::JournalAction::CANCEL) ==
      std::vector<runner::JournalOutcome>{runner::JournalOutcome::PENDING, runner::JournalOutcome::ACCEPTED}
    );
  }
}

TEST_CASE("on a broker's account what is thrown inside order place or order cancel is unknown with reconcile next") {
  for (const bool stop : {true, false}) {
    INFO((stop ? "a stop" : "an exception"));
    {
      // The handler's first live reading throws, before anything is sent.
      const KisFixture fixture;
      StandIn setup;
      const auto id = ready_plan(fixture, setup);
      setup.throw_after_ = setup.clock_calls_ + 1;
      setup.throw_stop_ = stop;
      const auto ran = run_on(setup, {"order", "place", id});
      INFO(ran.out_);
      CHECK(ran.exit_ == 4);
      CHECK(code_of(ran.out_) == "unknown.internal");
      CHECK(next_of(ran) == "cmdatl order reconcile");
      CHECK(setup.trading().places_ == 0);
      CHECK(fixture.entries().empty());
      setup.throw_after_ = -1;
      const auto next = run_on(setup, {"order", "reconcile", "005930"});
      INFO(next.out_);
      CHECK(next.exit_ == 0);
      CHECK(reply_of(next.out_).at("reason").as_string() == "clean");
    }
    const KisFixture fixture;
    StandIn setup;
    const auto number = owned_orders(fixture, setup).front();
    setup.throw_after_ = setup.clock_calls_ + 1;
    setup.throw_stop_ = stop;
    const auto ran = run_on(setup, {"order", "cancel", "005930"});
    INFO(ran.out_);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.internal");
    CHECK(next_of(ran) == "cmdatl order reconcile");
    CHECK(setup.trading().cancels_ == 0);
    CHECK(still_working(setup, number));
    CHECK(outcomes(fixture.entries(), runner::JournalAction::CANCEL).empty());
  }
  {
    // An owed read that throws after the broker accepted: not a result, so the CLI answers 4.
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    int lists = 0;
    setup.trading().before_list_ = [&lists] {
      if (++lists == 3) {
        throw std::runtime_error("the owed rows' read threw");
      }
    };
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.internal");
    CHECK(next_of(ran) == "cmdatl order reconcile");
    CHECK(setup.trading().places_ == 1);
    CHECK(outcomes(fixture.entries(), runner::JournalAction::PLACE).back() == runner::JournalOutcome::ACCEPTED);
  }
  {
    // A throw as the request leaves, after its pending line: once() answers unknown (4), and the plan is spent.
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    setup.trading().before_place_ = [] {
      throw std::runtime_error("the request's send threw");
    };
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.internal");
    CHECK(next_of(ran).find("reconcile") != std::string::npos);
    CHECK(setup.trading().places_ == 0);
    CHECK(
      outcomes(fixture.entries(), runner::JournalAction::PLACE) ==
      std::vector<runner::JournalOutcome>{runner::JournalOutcome::PENDING}
    );
    setup.trading().before_place_ = nullptr;
    const auto again = run_on(setup, {"order", "place", id});
    INFO(again.out_);
    CHECK(again.exit_ == 3);
    CHECK(code_of(again.out_) == "refused.plan_used");
    const auto next = run_on(setup, {"order", "reconcile", "005930"});
    INFO(next.out_);
    CHECK(next.exit_ == 4);
    CHECK(setup.trading().places_ == 0);
  }
}

TEST_CASE("a lost reply on a broker's account is exit 1 before the account dispatch and 4 inside it or after a send") {
  const KisFixture fixture;
  StandIn setup;
  // Refused before the operator's file is read: nothing could have been sent.
  const auto usage = run_on(setup, {"order", "place", "pNOTAPLAN"}, true);
  CHECK(usage.exit_ == 1);
  CHECK(usage.err_.find("its reply could not be written") != std::string::npos);
  CHECK(usage.err_.find("unknown") == std::string::npos);
  CHECK(setup.identified_ == 0);
  // A login refusal inside the account dispatch, its reply lost: 4 for a command that may send though nothing was,
  // 1 for a read.
  setup.logins_.failing_ = cmdatl::auth::StoreStatus::LOCKED;
  for (
    const auto& args :
    std::vector<std::vector<std::string>>{{"order", "place", "p0123456789"}, {"order", "cancel", "005930"}}
  ) {
    INFO(cmdatl::join(args));
    const auto lost = run_on(setup, args, true);
    CHECK(lost.exit_ == 4);
    CHECK(lost.err_.find("its outcome is unknown") != std::string::npos);
  }
  CHECK(run_on(setup, {"status"}, true).exit_ == 1);
  CHECK(setup.identified_ == 3);
  CHECK(!std::filesystem::exists(fixture.account()));
  setup.logins_.failing_.reset();
  // After a send: the minimal line names reconcile, and the order is in the journal.
  const auto id = ready_plan(fixture, setup);
  const auto sent = run_on(setup, {"order", "place", id}, true);
  CHECK(sent.exit_ == 4);
  CHECK(sent.err_.find("run cmdatl order reconcile before anything else") != std::string::npos);
  CHECK(setup.trading().places_ == 1);
  CHECK(outcomes(fixture.entries(), runner::JournalAction::PLACE).back() == runner::JournalOutcome::ACCEPTED);
  const auto next = run_on(setup, {"order", "reconcile", "005930"});
  INFO(next.out_);
  CHECK(next.exit_ == 3);
  CHECK(code_of(next.out_) == "refused.awaiting");
}

TEST_CASE("on a broker's account two orders owed on one symbol are the operator's and no withdrawal is sent") {
  // Each placement owns at most one working order, and two owed on one symbol are held for the operator: a broker's
  // account never sends several withdrawals in one call.
  for (const bool cancel : {true, false}) {
    INFO((cancel ? "order cancel" : "order plan, which would withdraw first"));
    const KisFixture fixture;
    fixture.paper_.arm();
    StandIn setup;
    const auto numbers = owned_orders(fixture, setup, {78000, 77900});
    const auto ran = run_on(
      setup,
      cancel ?
        std::vector<std::string>{"order", "cancel", "005930"} :
        std::vector<std::string>{"order", "plan", "005930", "--target", "5", "--limit", "78000"}
    );
    INFO(ran.out_);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.unaccounted");
    CHECK(next_of(ran).find("report to the operator") != std::string::npos);
    CHECK(setup.trading().cancels_ == 0);
    CHECK(still_working(setup, numbers[0]));
    CHECK(still_working(setup, numbers[1]));
    CHECK(outcomes(fixture.entries(), runner::JournalAction::CANCEL).empty());
  }
}

TEST_CASE("on a broker's account a read error at the boundary is refused before sending") {
  const KisFixture fixture;
  StandIn setup;
  const auto id = ready_plan(fixture, setup);
  // Once()'s quote, then the boundary's own: only the boundary's fails.
  const auto quotes = setup.trading().quotes_;
  setup.trading().before_quote_ = [&setup, quotes](int n) {
    if (n == quotes + 2) {
      setup.client_->fail_next(atl::mock::Call::QUOTE, marker("boundary-quote"));
    }
  };
  const auto ran = run_on(setup, {"order", "place", id});
  INFO(ran.out_);
  CHECK(ran.exit_ == 3);
  CHECK(code_of(ran.out_) == "refused.not_reached");
  CHECK(reply_of(ran.out_).at("error").as_object().at("message").as_string().find("no_price") != std::string::npos);
  CHECK(setup.trading().places_ == 0);
  CHECK(
    outcomes(fixture.entries(), runner::JournalAction::PLACE) ==
    std::vector<runner::JournalOutcome>{runner::JournalOutcome::PENDING, runner::JournalOutcome::REJECTED}
  );
  // Nothing is owed: a new plan is the way on.
  setup.trading().before_quote_ = nullptr;
  CHECK(run_on(setup, {"order", "plan", "005930", "--target", "1", "--limit", "78000"}).exit_ == 0);
}

TEST_CASE("on a broker's account a used or unarmed or moved plan sends nothing and a cancel needs no armed file") {
  {
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    REQUIRE(run_on(setup, {"order", "place", id}).exit_ == 0);
    const auto again = run_on(setup, {"order", "place", id});
    INFO(again.out_);
    CHECK(again.exit_ == 3);
    CHECK(code_of(again.out_) == "refused.plan_used");
    CHECK(setup.trading().places_ == 1);
    CHECK(outcomes(fixture.entries(), runner::JournalAction::PLACE).size() == 2);
  }
  {
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    std::filesystem::remove(fixture.paper_.armed());
    const auto unarmed = run_on(setup, {"order", "place", id});
    INFO(unarmed.out_);
    CHECK(unarmed.exit_ == 3);
    CHECK(code_of(unarmed.out_) == "refused.not_armed");
    CHECK(setup.trading().places_ == 0);
    CHECK(fixture.entries().empty());
  }
  {
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    // Another order on the account since the plan: its snapshot no longer holds.
    REQUIRE(setup.trading().place(order("000660", true, "1", "181000")));
    setup.trading().places_ = 0;
    const auto moved = run_on(setup, {"order", "place", id});
    INFO(moved.out_);
    CHECK(moved.exit_ == 3);
    CHECK(code_of(moved.out_) == "refused.stale_plan");
    CHECK(setup.trading().places_ == 0);
    CHECK(fixture.entries().empty());
  }
  // A cancel needs neither the armed file nor a permitted phase.
  const KisFixture fixture;
  StandIn setup;
  const auto number = owned_orders(fixture, setup).front();
  setup.times_ = {kst("2024-03-26T16:00")};
  const auto ran = run_on(setup, {"order", "cancel", "005930"});
  INFO(ran.out_);
  CHECK(ran.exit_ == 0);
  CHECK(reply_of(ran.out_).at("reason").as_string() == "withdrawal_sent");
  CHECK(reply_of(ran.out_).at("phase").as_string() == "post_close");
  CHECK(!std::filesystem::exists(fixture.paper_.armed()));
  CHECK(setup.trading().cancels_ == 1);
  CHECK(!still_working(setup, number));
}

TEST_CASE("on a broker's account the phase is checked at the boundary's sample and not at the broker's receipt") {
  const auto before_close = kst("2024-03-26T15:29") + std::chrono::seconds(50);
  const auto after_close = kst("2024-03-26T15:45");
  {
    // The phase closes between once()'s check and the boundary's sample: refused there, nothing sent.
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup, before_close);
    const auto quotes = setup.trading().quotes_;
    setup.trading().before_quote_ = [&setup, quotes, after_close](int n) {
      if (n == quotes + 2) {
        setup.times_ = {after_close};
      }
    };
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 3);
    CHECK(code_of(ran.out_) == "refused.not_reached");
    CHECK(reply_of(ran.out_).at("error").as_object().at("message").as_string().find("phase") != std::string::npos);
    CHECK(setup.trading().places_ == 0);
  }
  // A slow calendar read after the sample, or a slow send (a token request, the broker's pace), carries the order past
  // the phase it was checked in: the stated limit, an observation and not a guarantee at receipt.
  for (const bool calendar : {true, false}) {
    INFO((calendar ? "the boundary's calendar read" : "the send"));
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup, before_close);
    const auto quotes = setup.trading().quotes_;
    bool sampled = false;
    setup.trading().before_quote_ = [&sampled, quotes](int n) {
      sampled = sampled || n == quotes + 2;
    };
    if (calendar) {
      setup.trading().before_calendar_ = [&setup, &sampled, after_close](int) {
        if (sampled) {
          setup.times_ = {after_close};
        }
      };
    } else {
      setup.trading().before_place_ = [&setup, after_close] {
        setup.times_ = {after_close};
      };
    }
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("reason").as_string() == "placed");
    CHECK(setup.trading().places_ == 1);
    const auto lines = fixture.entries();
    REQUIRE(!lines.empty());
    CHECK(lines.back().outcome_ == runner::JournalOutcome::ACCEPTED);
    CHECK(lines.back().recorded_at_ >= after_close);
  }
  {
    // Midnight between the plan and its placement, in phases the operator allows on both sides.
    const KisFixture fixture("12345678-01", "phases = [\"pre_open\", \"open\", \"post_close\"]\n");
    StandIn setup;
    const auto id = ready_plan(fixture, setup, kst("2024-03-26T23:59") + std::chrono::seconds(40));
    const auto after = kst("2024-03-27T00:00") + std::chrono::seconds(10);
    setup.market_->advance_to(after);
    setup.times_ = {after};
    const auto ran = run_on(setup, {"order", "place", id});
    INFO(ran.out_);
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("phase").as_string() == "pre_open");
    CHECK(setup.trading().places_ == 1);
    // It counts on the new day.
    const auto status = run_on(setup, {"status"});
    INFO(status.out_);
    CHECK(reply_of(status.out_).at("orders_left_today").as_int64() == 2);
  }
}

TEST_CASE("on a broker's account an order command beside a run is busy and both count the journal's placements") {
  {
    const KisFixture fixture;
    StandIn setup;
    const auto id = ready_plan(fixture, setup);
    const auto plans = slurp(fixture.account() / "plans.json");
    cmdatl::OwnerLock run_lock(fixture.account().string());
    REQUIRE(
      run_lock.acquire(
        std::chrono::milliseconds(100),
        [] {
          return false;
        },
        [](const std::string&) {}
      ) == cmdatl::OwnerLock::Outcome::HELD
    );
    spit(
      fixture.account() / "owner.json",
      R"({"role":"agent","pid":4242,"what":"run r1 pid 4242"})"
      "\n"
    );
    for (
      const auto& args : std::vector<std::vector<std::string>>{{"order", "place", id}, {"order", "cancel", "005930"}}
    ) {
      INFO(cmdatl::join(args));
      const auto busy = run_on(setup, args);
      CHECK(busy.exit_ == 1);
      CHECK(code_of(busy.out_) == "busy.locked");
      // Its reply lost inside the account dispatch: 4, though nothing was sent.
      CHECK(run_on(setup, args, true).exit_ == 4);
    }
    CHECK(setup.trading().places_ + setup.trading().cancels_ == 0);
    CHECK(slurp(fixture.account() / "plans.json") == plans);
  }
  const KisFixture fixture;
  // Two placements a run made today, both refused by the broker: nothing owed, two of the three counted.
  fixture.journal({
    place_entry("r1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:10")),
    place_entry("r1", runner::JournalOutcome::REJECTED, kst("2024-03-26T09:10")),
    place_entry("r2", runner::JournalOutcome::PENDING, kst("2024-03-26T09:20")),
    place_entry("r2", runner::JournalOutcome::REJECTED, kst("2024-03-26T09:20")),
  });
  StandIn setup;
  const auto id = ready_plan(fixture, setup);
  const auto placed = run_on(setup, {"order", "place", id});
  INFO(placed.out_);
  CHECK(placed.exit_ == 0);
  const auto over = run_on(setup, {"order", "plan", "005930", "--target", "2", "--limit", "78000"});
  INFO(over.out_);
  CHECK(over.exit_ == 3);
  CHECK(code_of(over.out_) == "refused.daily_order_limit");
  // A run's start counts the command's placement with the run's, from the same journal and the same rule.
  const auto snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.config_);
  const auto view = cmdatl::journal_view(
    setup.trading(), runner::read_journal_file(fixture.journal_file().string()), SCOPE, cmdatl::market_of_word("kr"),
    cmdatl::kst_day(kst("2024-03-26T10:00"))
  );
  CHECK(view.placed_today_ == 3);
  CHECK(cmdatl::orders_left(*snapshot.config_, view) == std::optional<std::int64_t>(0));
}

#endif
