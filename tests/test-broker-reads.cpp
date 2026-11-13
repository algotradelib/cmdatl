// The account and order commands on a broker's account: the command path on a stand-in setup (the library's mock
// session and an in-memory login store, so no case can reach a broker), and KIS's own setup on made-up logins, which
// stops at the logins or at a held lock. Every login here is made up.

#include "./paper-support.hpp"

#include <cmdatl/cli/kis.hpp>

#include <algotradelib/mock>

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
namespace atl = algotradelib;
namespace runner = algotradelib::runner;

namespace {

/** @brief The account's login: a demo login of account 12345678-01. */
const std::string DEMO_LOGIN =
  R"({"app_key":"made-up-demo-key","app_secret":"MADE-UP-DEMO-SECRET","account_number":"12345678","demo":true})";
/** @brief The real login a demo account reads market data with. */
const std::string REAL_LOGIN =
  R"({"app_key":"made-up-real-key","app_secret":"MADE-UP-REAL-SECRET","account_number":"87654321","demo":false})";

/** @brief The account directory a KIS demo command takes: the login's account. */
const std::string ACCOUNT = "kis-demo-12345678-01";
/** @brief Its journal's scope. */
const std::string SCOPE = "kis:demo:12345678-01";

/**
 * @brief The operator's file for a KIS demo account.
 * @param number The `[account] number` line's value, or empty for none.
 * @return The text.
 */
std::string kis_config(const std::string& number = "12345678-01") {
  return "[account]\nbroker = \"kis\"\n" + (number.empty() ? std::string() : "number = \"" + number + "\"\n") +
    "\n[limits]\nsymbols = [\"005930\", \"000660\"]\nposition = { \"005930\" = 100, \"000660\" = 20 }\n"
    "order_value = 5000000\ndaily_orders = 3\n";
}

/** @brief A credential store in memory, recording what was asked. */
class Logins : public cmdatl::auth::CredentialStore {
public:
  /** @brief The logins by name. */
  std::map<std::string, std::string> items_;
  /** @brief Every name asked for. */
  std::vector<std::string> asked_;
  /** @brief How every read answers, when set: a locked or unusable store. */
  std::optional<cmdatl::auth::StoreStatus> failing_;

  /** @brief The store's kind. */
  std::string kind() const override {
    return "memory";
  }

  /** @brief Lists nothing: never called here. */
  cmdatl::auth::StoreList list() override {
    return {};
  }

  /**
   * @brief A login.
   * @param name Its name.
   * @return It, or not found.
   */
  cmdatl::auth::StoreAnswer get(const std::string& name) override {
    asked_.push_back(name);
    cmdatl::auth::StoreAnswer answer;
    if (failing_) {
      answer.status_ = *failing_;
      return answer;
    }
    const auto found = items_.find(name);
    answer.status_ = found == items_.end() ? cmdatl::auth::StoreStatus::NOT_FOUND : cmdatl::auth::StoreStatus::OK;
    if (found != items_.end()) {
      answer.value_ = found->second;
    }
    return answer;
  }

  /** @brief Never called here. */
  cmdatl::auth::StoreAnswer put(const std::string&, const std::string&) override {
    return {};
  }

  /** @brief Never called here. */
  cmdatl::auth::StoreAnswer remove(const std::string&) override {
    return {};
  }
};

/**
 * @brief A broker error a test scripts, to see whether a call consumed it.
 * @param code Its code.
 * @return The error, whose message stands for a broker's body.
 */
atl::BrokerError marker(const std::string& code) {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::TRANSPORT;
  error.broker_code_ = code;
  error.message_ = "MADE-UP-BODY-TEXT";
  return error;
}

/** @brief A stand-in setup: the real login checks on an in-memory store, and the library's mock session. */
struct StandIn {
  /** @brief The logins. */
  Logins logins_;
  /** @brief The market, from the fixture's bars. */
  std::unique_ptr<atl::mock::Market> market_;
  /** @brief Its client, for scripting faults. */
  std::shared_ptr<atl::mock::MockClient> client_;
  /** @brief The one session, trading and market data. */
  std::unique_ptr<atl::BlockingSession<atl::mock::MockClient>> session_;
  /** @brief The clock's answers, in order; the last repeats. */
  std::vector<cmdatl::Clock::time_point> times_{kst("2024-03-26T10:00")};
  /** @brief How many times the clock was read. */
  int clock_calls_{0};
  /** @brief How many times the stop was checked. */
  int checks_{0};
  /** @brief Stop from the check after this many, or never. */
  int stop_after_{-1};
  /** @brief How many times `identify` ran. */
  int identified_{0};
  /** @brief Whether the clock checks that the calendar was read before it, by a scripted calendar fault. */
  bool probe_calendar_{false};
  /** @brief Whether the calendar's fault was taken before the clock was read. */
  bool calendar_before_clock_{false};
  /** @brief Throw from the clock once it was read this many times, or never. */
  int throw_after_{-1};
  /** @brief Whether that throw is a stop (`cmdatl::Stopped`) rather than another exception. */
  bool throw_stop_{false};

  /** @brief The market of the fixture's bars, with both logins stored. */
  StandIn() {
    cmdatl::MarketData data;
    REQUIRE(!cmdatl::parse_bars(BARS, data));
    std::vector<atl::BarSeries> series;
    for (const auto& entry : data.bars_) {
      series.push_back(cmdatl::mock_series(entry.first, entry.second));
    }
    atl::mock::MarketSettings settings;
    settings.cash_ = atl::Money{atl::Decimal64(100000000, 0), atl::Currency::krw()};
    settings.start_ = kst("2024-03-25T16:00");
    market_ = std::make_unique<atl::mock::Market>(std::move(series), std::move(settings));
    client_ = std::make_shared<atl::mock::MockClient>(*market_);
    session_ = std::make_unique<atl::BlockingSession<atl::mock::MockClient>>(client_, atl::mock::MockAccount{});
    logins_.items_["kis/demo"] = DEMO_LOGIN;
    logins_.items_["kis/real"] = REAL_LOGIN;
  }

  /**
   * @brief Reads and checks the logins as KIS's setup does; the sessions are the mock's.
   * @param config The operator's file.
   * @param effective Its number set to the login's account.
   * @return The refusal, or nothing.
   */
  std::optional<cmdatl::Reply> identify(const cmdatl::OperatorConfig& config, cmdatl::OperatorConfig& effective) {
    ++identified_;
    std::vector<std::string> documents;
    if (
      auto refused = cmdatl::runs::read_run_logins(config, cmdatl::auth::lookup_broker("kis", ""), &logins_, documents)
    ) {
      return refused;
    }
    atl::kis::KisConfig trading;
    atl::kis::KisAccount account;
    std::optional<atl::kis::KisConfig> market;
    if (const auto mismatch = cmdatl::read_kis_logins(config, documents, trading, account, market, effective)) {
      return cmdatl::error_reply(cmdatl::Exit::FAILED, "auth.account_mismatch", *mismatch);
    }
    return std::nullopt;
  }

  /** @brief The trading session. */
  atl::BlockingSession<atl::mock::MockClient>& trading() {
    return *session_;
  }

  /** @brief The market-data session: the same. */
  atl::BlockingSession<atl::mock::MockClient>& market() {
    return *session_;
  }

  /** @brief The scripted clock. */
  cmdatl::Clock::time_point now() {
    if (throw_after_ >= 0 && clock_calls_ >= throw_after_) {
      if (throw_stop_) {
        throw cmdatl::Stopped();
      }
      throw std::runtime_error("the stand-in's clock failed");
    }
    if (probe_calendar_) {
      // The fault is still there only when the calendar was not read yet.
      calendar_before_clock_ = !unconsumed(
        [this] {
          return session_->calendar(atl::Country::KR);
        },
        "calendar-first"
      );
    }
    const auto at = times_[std::min<std::size_t>(static_cast<std::size_t>(clock_calls_), times_.size() - 1)];
    ++clock_calls_;
    return at;
  }

  /** @brief The scripted stop. */
  bool stopped() {
    ++checks_;
    return stop_after_ >= 0 && checks_ > stop_after_;
  }

  /**
   * @brief Whether a scripted fault is still waiting for its call, which therefore was not made.
   * @param read Makes the call.
   * @param code The fault's code.
   * @return Whether the call answers the fault.
   */
  template <typename Read>
  static bool unconsumed(Read read, const std::string& code) {
    const auto answer = read();
    return !answer && answer.error().broker_code_ == code;
  }
};

/** @brief A KIS demo account in a scratch directory, its token folder there too. */
struct KisFixture {
  /** @brief The operator's file and state root. */
  PaperFixture paper_;
  /** @brief The cache root. */
  ScopedVariable cache_{"XDG_CACHE_HOME", (paper_.dir() / "cache").string()};

  /**
   * @brief Writes the operator's file.
   * @param number Its account number, or empty for none.
   */
  explicit KisFixture(const std::string& number = "12345678-01") {
    paper_.write_config(kis_config(number));
  }

  /**
   * @brief An account's directory.
   * @param name Its name.
   * @return The path.
   */
  std::filesystem::path account(const std::string& name = ACCOUNT) const {
    return paper_.dir() / "state" / "cmdatl" / "accounts" / name;
  }

  /**
   * @brief Writes the account's journal through the library's writer.
   * @param entries The entries.
   * @param scope Its scope.
   */
  void journal(const std::vector<runner::JournalEntry>& entries, const std::string& scope = SCOPE) const {
    std::filesystem::create_directories(account());
    runner::FileJournal file((account() / "journal.jsonl").string(), scope);
    REQUIRE(file.open());
    for (const auto& entry : entries) {
      REQUIRE(file.append(entry));
    }
  }
};

/**
 * @brief A placement's journal line.
 * @param id The journal id.
 * @param outcome What the line says.
 * @param at When it was written.
 * @param order_id The broker's id, or empty.
 * @return The entry.
 */
runner::JournalEntry place_entry(
  const std::string& id, runner::JournalOutcome outcome, cmdatl::Clock::time_point at, const std::string& order_id = ""
) {
  runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = outcome;
  entry.recorded_at_ = at;
  entry.instrument_ = cmdatl::kr("005930");
  entry.side_ = atl::OrderSide::BUY;
  entry.type_ = atl::OrderType::LIMIT;
  entry.time_in_force_ = atl::TimeInForce::DAY;
  entry.session_ = atl::OrderSession::REGULAR;
  entry.venue_ = atl::Venue::UNSPECIFIED;
  entry.quantity_ = atl::Decimal64(1, 0);
  entry.price_ = atl::Decimal64(70000, 0);
  entry.baseline_ = atl::Decimal64(0, 0);
  entry.order_id_ = order_id;
  return entry;
}

/**
 * @brief Runs a command line through `cmdatl::run`, with the command path on a KIS account going to a setup.
 * @tparam Setup The setup.
 * @param setup The setup.
 * @param args The command line, without `--json`.
 * @return What it wrote and returned.
 */
template <typename Setup>
Ran run_on(Setup& setup, std::vector<std::string> args) {
  args.insert(args.begin(), "--json");
  std::ostringstream out;
  std::ostringstream err;
  const auto exit = cmdatl::run(
    args, out, err, false,
    [&setup](
      const cmdatl::Command& command, const cmdatl::Invocation& invocation, const cmdatl::ConfigSnapshot& snapshot,
      cmdatl::Output& output
    ) {
      return cmdatl::broker_command_reply(command, invocation, *snapshot.config_, output, setup);
    }
  );
  const Ran ran{exit, out.str(), err.str()};
  for (const auto* secret : {"MADE-UP-DEMO-SECRET", "MADE-UP-REAL-SECRET", "MADE-UP-BODY-TEXT"}) {
    CHECK(ran.out_.find(secret) == std::string::npos);
    CHECK(ran.err_.find(secret) == std::string::npos);
  }
  return ran;
}

/**
 * @brief A reply's words.
 * @param value An array of strings.
 * @return The words.
 */
std::vector<std::string> words(const boost::json::value& value) {
  std::vector<std::string> out;
  for (const auto& word : value.as_array()) {
    out.emplace_back(word.as_string());
  }
  return out;
}

} // namespace

TEST_CASE("on a KIS account order place and order cancel and the clock commands are refused before any login is read") {
  const KisFixture fixture;
  StandIn setup;
  for (
    const auto& args : std::vector<std::vector<std::string>>{
      {"order", "place", "p9cc4b94da4"},
      {"order", "cancel", "005930"},
      {"clock"},
      {"clock", "advance", "--minutes", "1"},
    }
  ) {
    INFO(cmdatl::join(args));
    const auto ran = run_on(setup, args);
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "unsupported.broker");
    CHECK(reply_of(ran.out_).at("env").as_string() == "kis-demo");
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
  const auto start = std::chrono::steady_clock::now();
  const auto busy = run_on(setup, {"status"});
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
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
    CHECK((std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(200)) == waits);
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

TEST_CASE("repeated reads on a broker's account leave no descriptor open") {
  const KisFixture fixture;
  StandIn setup;
  REQUIRE(run_on(setup, {"status"}).exit_ == 0);
  const auto before = open_fds();
  for (int i = 0; i < 20; ++i) {
    REQUIRE(run_on(setup, {i % 2 == 0 ? "status" : "account"}).exit_ == 0);
  }
  CHECK(open_fds() == before);
}

TEST_CASE("KIS's setup refuses a login that is not the operator file's account, and never repeats a login") {
  const KisFixture fixture;
  Logins logins;
  cmdatl::KisSetup setup(&logins);
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
  cmdatl::KisSetup setup(&logins);
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
      cmdatl::KisSetup setup(&logins);
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

/** @brief The stand-in's desk, as the command path builds it: one mock session for trading and market data. */
using StandInDesk =
  cmdatl::BrokerDesk<atl::BlockingSession<atl::mock::MockClient>, atl::BlockingSession<atl::mock::MockClient>>;

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
  std::is_same<StandInDesk::Session, cmdatl::GuardedSession<atl::BlockingSession<atl::mock::MockClient>>>::value,
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
  const auto unlisted = cmdatl::owed_next("005930", "named", std::nullopt, false);
  CHECK(unlisted.find("not in the broker's order list now") != std::string::npos);
  CHECK(unlisted.find("cmdatl order reconcile 005930") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "unnamed", std::nullopt, false).find("never named it") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "operator", std::nullopt, false).find("order release 005930") != std::string::npos);
}

TEST_CASE("the table cannot mark a command that sends as working on a broker's account") {
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
  const auto none = [](cmdatl::Command&) {};
  CHECK(cmdatl::check_table(cmdatl::Table(cmdatl::command_list())).empty());
  CHECK(problems_with("order place", none).empty());
  for (const auto* name : {"order place", "order cancel"}) {
    INFO(name);
    CHECK(problems_with(name, [](cmdatl::Command& command) {
      command.broker_ = true;
    }).size() == 1);
    // A kind that sends without the mark would run on a broker's account unchecked.
    CHECK(problems_with(name, [](cmdatl::Command& command) {
      command.may_send_ = false;
    }).size() == 1);
  }
  CHECK(problems_with("order release", [](cmdatl::Command& command) {
    command.mutates_ = false;
  }).size() == 1);
  CHECK(problems_with("order plan", [](cmdatl::Command& command) {
    command.may_send_ = true;
  }).size() >= 1);
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

TEST_CASE("order reconcile and order release work on a broker's account and send nothing") {
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
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().place(order("005930", true, "1", "78000"));
      },
      "place"
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
  setup.client_->fail_next(atl::mock::Call::PLACE, marker("place"));
  // As of 10:00:30, within its 60 s; on the live clock, after the snapshot's reads, 61 s old.
  setup.times_ = {
    kst("2024-03-26T10:00") + std::chrono::seconds(30), kst("2024-03-26T10:01") + std::chrono::seconds(1)
  };
  setup.clock_calls_ = 0;
  const auto opened = open_broker(setup);
  cmdatl::Invocation invocation;
  invocation.values_["PLAN_ID"] = {id};
  const auto reply = cmdatl::place_reply(invocation, *opened->account_, opened->output_);
  CHECK(code_of_reply(reply) == "refused.stale_plan");
  CHECK(reply.body_.at("now").as_string() == "2024-03-26T10:00:30+09:00");
  CHECK(opened->account_->desk().sends() == 0);
  CHECK(
    StandIn::unconsumed(
      [&] {
        return setup.trading().place(order("005930", true, "1", "78000"));
      },
      "place"
    )
  );
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

#endif
