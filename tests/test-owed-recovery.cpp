// A run on a broker's account that starts after an earlier run died with orders owed: `run_account` on the library's
// mock client, each run in a forked child (as `test-account-run.cpp` runs them), the earlier run's orders placed on the
// child's market and written to the account's journal by the library's own journaling executor before the run starts.
// Each case asserts the journal on disk and starts the account again, so nothing settled only in memory passes.

#include "paper-support.hpp"
#include "run-strategy.hpp"
#include "support.hpp"

#include <cmdatl/account/account.hpp>
#include <cmdatl/account/operator.hpp>
#include <cmdatl/driver/account.hpp>
#include <cmdatl/driver/clock.hpp>
#include <cmdatl/paper/krx_grid.hpp>
#include <cmdatl/system/process.hpp>

#include <algotradelib/live>
#include <algotradelib/mock>
#include <algotradelib/runner>

#include <boost/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace atl = algotradelib;
namespace json = boost::json;

namespace {

using Session = atl::BlockingSession<atl::mock::MockClient>;

/** @brief The operator's file for a KIS demo account trading 005930 and 000660. */
std::string kis_config(int daily_orders = 3) {
  std::string text = "[account]\nbroker = \"kis\"\nnumber = \"12345678-01\"\n\n[limits]\n";
  text += "symbols = [\"005930\", \"000660\"]\n";
  text += "position = { \"005930\" = 100, \"000660\" = 20 }\n";
  text += "order_value = 5000000\n";
  text += "daily_orders = " + std::to_string(daily_orders) + "\n";
  return text;
}

/** @brief An instrument of the account, as the mock market lists it. */
atl::InstrumentId kr(const std::string& symbol) {
  return atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol};
}

/** @brief The market's clock, for the earlier run's journal lines. */
struct MarketTime {
  /** @brief The market. */
  atl::mock::Market* market_;
  /** @brief Now. @return The market's time. */
  std::chrono::system_clock::time_point now() const {
    return market_->now();
  }
};

/**
 * @brief The earlier run, before it died: its orders go to the child's market and, when it is seeding, to the
 *   account's journal through the library's journaling executor, as a run's binding writes them (`PENDING` with the
 *   baseline, then the broker's answer). On a restart the journal is already on disk: only the market is rebuilt, by
 *   the same placements in the same order, so the market's numbers come out the same.
 */
class Dead {
public:
  using Executor =
    atl::runner::JournalingExecutor<atl::live::SessionExecutor<Session>, atl::runner::FileJournal, MarketTime>;

  /**
   * @brief The earlier run.
   * @param market The market.
   * @param client The client.
   * @param session The session.
   * @param journal_path The account's journal.
   * @param scope The account's scope.
   * @param seed Whether to write the journal (the first start), or only rebuild the market (a restart).
   */
  Dead(
    atl::mock::Market& market, atl::mock::MockClient& client, Session& session, const std::string& journal_path,
    const std::string& scope, bool seed
  )
  : market_(&market), client_(&client), session_(&session), seed_(seed) {
    if (seed_) {
      journal_ = std::make_unique<atl::runner::FileJournal>(journal_path, scope);
      REQUIRE(journal_->open());
      executor_ =
        std::make_unique<Executor>(atl::live::SessionExecutor<Session>(session), *journal_, MarketTime{&market});
      executor_->set_id_prefix("dead");
    }
  }

  /** @brief The market. @return It. */
  atl::mock::Market& market() {
    return *market_;
  }
  /** @brief The client. @return It. */
  atl::mock::MockClient& client() {
    return *client_;
  }
  /** @brief Whether this start writes the journal. @return Whether. */
  bool seeding() const {
    return seed_;
  }

  /**
   * @brief Places a limit order as the earlier run did.
   * @param symbol The symbol.
   * @param side The side.
   * @param quantity The quantity.
   * @param price The limit.
   * @param lost Whether the broker's reply is lost (the order is carried out, the journal says `INDETERMINATE`).
   */
  void place(
    const std::string& symbol, atl::OrderSide side, std::int64_t quantity, std::int64_t price, bool lost = false
  ) {
    atl::strategy::PlannedOrder order;
    order.instrument_ = kr(symbol);
    order.side_ = side;
    order.type_ = atl::OrderType::LIMIT;
    order.time_in_force_ = atl::TimeInForce::DAY;
    order.quantity_ = atl::Decimal64(quantity, 0);
    order.price_ = atl::Decimal64(price, 0);
    if (seed_) {
      if (lost) {
        client_->lose_next_reply(atl::mock::Call::PLACE);
      }
      const auto held = market_->holdings();
      REQUIRE(held);
      atl::Decimal64 baseline(0, 0);
      for (const auto& position : held.value().positions_) {
        if (position.instrument_ == order.instrument_) {
          baseline = position.quantity_;
        }
      }
      (void)executor_->place(order, baseline);
      REQUIRE(executor_->last_entry());
      last_ = *executor_->last_entry();
      return;
    }
    atl::OrderRequest request;
    request.instrument_ = order.instrument_;
    request.side_ = side;
    request.type_ = order.type_;
    request.time_in_force_ = order.time_in_force_;
    request.quantity_ = order.quantity_;
    request.price_ = order.price_;
    REQUIRE(session_->place(request));
  }

  /**
   * @brief Places a limit order that is no earlier run's: straight to the market, never in the journal.
   * @param symbol The symbol.
   * @param side The side.
   * @param quantity The quantity.
   * @param price The limit.
   */
  void place_elsewhere(const std::string& symbol, atl::OrderSide side, std::int64_t quantity, std::int64_t price) {
    atl::OrderRequest request;
    request.instrument_ = kr(symbol);
    request.side_ = side;
    request.type_ = atl::OrderType::LIMIT;
    request.time_in_force_ = atl::TimeInForce::DAY;
    request.quantity_ = atl::Decimal64(quantity, 0);
    request.price_ = atl::Decimal64(price, 0);
    REQUIRE(session_->place(request));
  }

  /**
   * @brief Writes a line the earlier run's journal holds, when seeding; stamped as given.
   * @param entry The line.
   */
  void append(const atl::runner::JournalEntry& entry) {
    if (seed_) {
      REQUIRE(journal_->append(entry));
    }
  }

  /** @brief The answering line of the last placement through the journal. @return It. */
  const atl::runner::JournalEntry& last() const {
    return last_;
  }

  /** @brief Moves the market. @param by How far. */
  void advance(std::chrono::system_clock::duration by) {
    market_->advance_to(market_->now() + by);
  }

private:
  atl::mock::Market* market_;
  atl::mock::MockClient* client_;
  Session* session_;
  bool seed_;
  std::unique_ptr<atl::runner::FileJournal> journal_;
  std::unique_ptr<Executor> executor_;
  atl::runner::JournalEntry last_;
};

/**
 * @brief A session whose working-order rows carry no order time, as KIS real-account rows do; everything else forwarded.
 * @tparam Inner The session it wraps.
 */
template <typename Inner>
class UndatedSession {
public:
  /** @brief The broker's capabilities. */
  using Caps = typename Inner::Caps;
  /** @brief The broker's tag. */
  using BrokerTag = typename Inner::BrokerTag;
  /** @brief The broker's traits. */
  using Traits = typename Inner::Traits;
  /** @brief An order reference. */
  using Ref = typename Inner::Ref;

  /** @brief Wraps a session. @param inner The session; borrowed. */
  explicit UndatedSession(Inner& inner) : inner_(&inner) {}

  /** @brief Forwarded. @param order The order. @return The answer. */
  auto place(atl::OrderRequest order) {
    return inner_->place(std::move(order));
  }
  /** @brief Forwarded. @param ref The order. @param instruction What to cancel. @return The answer. */
  auto cancel(const Ref& ref, const atl::AmendInstruction& instruction) {
    return inner_->cancel(ref, instruction);
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto bars(atl::BarQuery query) {
    return inner_->bars(std::move(query));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto quote(atl::InstrumentId instrument) {
    return inner_->quote(std::move(instrument));
  }
  /** @brief Forwarded. @param subject The instrument. @return The answer. */
  auto instrument(atl::InstrumentId subject) {
    return inner_->instrument(std::move(subject));
  }
  /** @brief Forwarded. @param country The country. @param from The first date. @return The answer. */
  auto calendar(atl::Country country, std::optional<atl::Date> from = std::nullopt) {
    return inner_->calendar(country, std::move(from));
  }
  /** @brief Forwarded. @return The answer. */
  auto holdings() {
    return inner_->holdings();
  }
  /** @brief Forwarded. @param currency The currency. @return The answer. */
  auto buying_power(atl::Currency currency) {
    return inner_->buying_power(std::move(currency));
  }
  /** @brief Forwarded, each row's order time removed. @return The answer. */
  auto working_orders() {
    auto answer = inner_->working_orders();
    if (answer) {
      for (auto& row : answer.value().orders_) {
        row.order_.ordered_at_.reset();
      }
    }
    return answer;
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto order_history(const atl::OrderHistoryQuery& query) {
    return inner_->order_history(query);
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto sellable_quantity(atl::InstrumentId instrument) {
    return inner_->sellable_quantity(std::move(instrument));
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto price_limits(atl::InstrumentId instrument) {
    return inner_->price_limits(std::move(instrument));
  }

private:
  Inner* inner_;
};

/** @brief What a run in a child answered, and what its market and journal showed. */
struct Ran {
  /** @brief The child's exit. */
  int exit_{-1};
  /** @brief The reply. */
  json::object reply_;
  /** @brief The events. */
  std::vector<json::object> events_;
  /** @brief What the market held after the run, by symbol. */
  json::object held_after_;
  /** @brief Every order the market took, oldest first, after the run: id, symbol, side, status. */
  json::array orders_after_;
  /** @brief The journal just before the run started. */
  std::string journal_before_;
  /** @brief The journal after the run. */
  std::string journal_after_;
};

/** @brief The error code of a reply, or empty. */
std::string code_of(const Ran& ran) {
  const auto* error = ran.reply_.if_contains("error");
  return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
}

/** @brief The events of a kind. */
std::vector<json::object> events_of(const Ran& ran, const std::string& kind) {
  std::vector<json::object> out;
  for (const auto& event : ran.events_) {
    if (event.at("event").as_string() == kind) {
      out.push_back(event);
    }
  }
  return out;
}

/** @brief An order's status as the market's history reports it. */
std::string status_name(atl::OrderStatus status) {
  switch (status) {
    case atl::OrderStatus::PENDING:
      return "pending";
    case atl::OrderStatus::FILLED:
      return "filled";
    case atl::OrderStatus::CANCELED:
      return "canceled";
    default:
      return "other";
  }
}

/**
 * @brief Runs `run_account` on 005930 in a child, on a fresh mock market of 005930 and 000660 from 09:30 KST of the
 *   third day of bars, after `before` played the earlier run on it.
 *   The child raises SIGINT once the market's clock is `stop_after` past the run's start, so a run that starts stops.
 * @param fixture The fixture (operator file, state root).
 * @param before The earlier run, given the market, the client and whether it writes the journal; may be empty.
 * @param seed Whether `before` writes the journal (the first start) or only rebuilds the market (a restart).
 * @param stop_after How long the run trades before the signal, on the market's clock.
 * @param undated Whether the run reads working-order rows without their order time (`UndatedSession`).
 * @return What it answered, and what the market and the journal showed.
 */
Ran owed_run(
  const PaperFixture& fixture, const std::function<void(Dead&)>& before, bool seed,
  std::chrono::minutes stop_after = std::chrono::minutes(20), bool undated = false
) {
  const auto reply_path = fixture.dir() / "owed.reply";
  const auto after_path = fixture.dir() / "owed.after";
  const auto journal_before = fixture.dir() / "journal.before";
  std::filesystem::remove(after_path);
  std::filesystem::remove(journal_before);
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(120);
    const int fd = ::open(reply_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    const int err = ::open((fixture.dir() / "owed.stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(err, STDERR_FILENO);
    ::close(err);
    cmdatl::handle_signals();
    const auto series = runtest::bars_of("005930", 10, atl::BarInterval::ONE_DAY);
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
    Session session(client, atl::mock::MockAccount{});
    const auto snapshot = cmdatl::load_operator_config();
    if (snapshot.state_ != cmdatl::ConfigSnapshot::State::OK) {
      ::_exit(90);
    }
    const auto account_dir = *cmdatl::account_dir(*snapshot.config_);
    if (cmdatl::make_private_dirs(account_dir) != 0) {
      ::_exit(91);
    }
    const auto journal_path = account_dir + "/journal.jsonl";
    if (before) {
      Dead dead(market, *client, session, journal_path, snapshot.config_->scope(), seed);
      before(dead);
    }
    if (std::filesystem::exists(journal_path)) {
      std::filesystem::copy_file(journal_path, journal_before);
    }
    cmdatl::driver::AccountRun run;
    run.base_ = json::object{{"env", "kis-demo"}, {"run", "r1"}};
    run.id_ = "r1";
    run.symbols_ = {"005930"};
    run.ids_ = {series.instrument_};
    run.warmup_ = runtest::script().warmup_;
    run.account_dir_ = account_dir;
    const auto stop_at = market.now() + stop_after;
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
        [&market, &raised, stop_at](std::chrono::nanoseconds duration) {
          market.advance_to(market.now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
          if (!raised && !(market.now() < stop_at)) {
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
    if (undated) {
      UndatedSession<Session> without_times(session);
      code = cmdatl::driver::run_account(
        run, *snapshot.config_, runtest::Program(run.ids_), without_times, without_times, make_clock
      );
    } else {
      code =
        cmdatl::driver::run_account(run, *snapshot.config_, runtest::Program(run.ids_), session, session, make_clock);
    }
    json::object held;
    if (const auto holdings = market.holdings()) {
      for (const auto& position : holdings.value().positions_) {
        held[position.instrument_.symbol_] = position.quantity_.str();
      }
    }
    json::array orders;
    for (const auto& order : market.order_history(atl::OrderHistoryQuery{}).orders_) {
      orders.push_back(
        json::object{
          {"id", order.order_id_},
          {"symbol", order.instrument_.symbol_},
          {"side", order.side_ == atl::OrderSide::BUY ? "buy" : "sell"},
          {"status", status_name(order.status_)},
        }
      );
    }
    std::FILE* after = std::fopen(after_path.c_str(), "w");
    if (after != nullptr) {
      std::fputs(json::serialize(json::object{{"held", held}, {"orders", orders}}).c_str(), after);
      std::fclose(after);
    }
    std::fflush(nullptr);
    ::_exit(code);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) < 90);
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
    ran.orders_after_ = after.at("orders").as_array();
  }
  if (std::filesystem::exists(journal_before)) {
    ran.journal_before_ = slurp(journal_before);
  }
  const auto journal = fixture.dir() / "state" / "cmdatl" / "accounts" / "kis-demo-12345678-01" / "journal.jsonl";
  if (std::filesystem::exists(journal)) {
    ran.journal_after_ = slurp(journal);
  }
  return ran;
}

/**
 * @brief The account's journal on disk, as the library reads it.
 * @param fixture The fixture.
 * @return Its lines.
 */
atl::runner::JournalReplay journal_on_disk(const PaperFixture& fixture) {
  return atl::runner::read_journal_file(
    (fixture.dir() / "state" / "cmdatl" / "accounts" / "kis-demo-12345678-01" / "journal.jsonl").string()
  );
}

/**
 * @brief The outcomes the journal on disk wrote for a journal id, in order.
 * @param fixture The fixture.
 * @param id The id.
 * @return The outcomes' names.
 */
std::vector<std::string> outcomes_of(const PaperFixture& fixture, const std::string& id) {
  std::vector<std::string> out;
  for (const auto& entry : journal_on_disk(fixture).entries_) {
    if (entry.id_ == id) {
      out.emplace_back(atl::runner::journal_outcome_name(entry.outcome_));
    }
  }
  return out;
}

/**
 * @brief Whether a list of names holds one.
 * @param names The names.
 * @param name The name.
 * @return Whether.
 */
bool has(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

/**
 * @brief The journal ids a reply's or start line's owed list names.
 * @param rows The list.
 * @return The ids.
 */
std::vector<std::string> owed_ids(const json::value& rows) {
  std::vector<std::string> ids;
  for (const auto& row : rows.as_array()) {
    ids.emplace_back(row.as_object().at("journal_id").as_string());
  }
  return ids;
}

/**
 * @brief The status of the market's order with an id after the run, or empty.
 * @param ran The run.
 * @param id The market's id.
 * @return The status.
 */
std::string status_of(const Ran& ran, const std::string& id) {
  for (const auto& order : ran.orders_after_) {
    if (order.as_object().at("id").as_string() == id) {
      return std::string(order.as_object().at("status").as_string());
    }
  }
  return {};
}

/**
 * @brief Sets the script: nothing decided, or one decision on the second bar, a target of `target` shares at the close.
 * @param target The target, or nothing for no decision.
 */
void script_target(std::optional<std::int64_t> target) {
  runtest::script() = runtest::Script{};
  if (target) {
    runtest::script().decisions_ = {{2, runtest::Decision{*target, "1"}}};
  }
}

/** @brief A broker read's failure, as a transport error. */
atl::BrokerError read_failure() {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::TRANSPORT;
  error.message_ = "the read failed";
  return error;
}

/**
 * @brief Checks a refusal before anything is sent: no start line, nothing placed or cancelled on the market, and the
 *   journal byte for byte what it was.
 * @param ran The run.
 * @param orders How many orders the market took before the run.
 */
void sent_nothing(const Ran& ran, std::size_t orders) {
  CHECK(events_of(ran, "start").empty());
  CHECK(events_of(ran, "placed").empty());
  CHECK(ran.orders_after_.size() == orders);
  for (const auto& order : ran.orders_after_) {
    CHECK(order.as_object().at("status").as_string() == "pending");
  }
  CHECK(!ran.journal_before_.empty());
  CHECK(ran.journal_after_ == ran.journal_before_);
}

/** @brief A limit far below the market: it rests all day. */
constexpr std::int64_t RESTS = 60000;
/** @brief A buy limit above the next bar's range: it fills on that bar. */
constexpr std::int64_t FILLS = 90000;

} // namespace

TEST_CASE("a run settles an earlier run's working order on its symbol: withdrawn at the stop, confirmed, clean") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  // The earlier run's buy rests on the run's symbol, and the program wants the same share: nothing more is placed.
  script_target(1);
  const auto earlier = [](Dead& dead) {
    dead.place("005930", atl::OrderSide::BUY, 1, RESTS);
    dead.advance(std::chrono::minutes(5));
  };
  const auto ran = owed_run(fixture, earlier, true);
  CHECK(ran.exit_ == 0);
  CHECK(code_of(ran).empty());
  REQUIRE(!ran.events_.empty());
  const auto& start = ran.events_.front();
  REQUIRE(start.at("event").as_string() == "start");
  const auto& owed = start.at("owed_at_start").as_array();
  REQUIRE(owed.size() == 1);
  const auto& row = owed.front().as_object();
  CHECK(row.at("symbol").as_string() == "005930");
  CHECK(row.at("side").as_string() == "buy");
  CHECK(row.at("quantity").as_string() == "1");
  CHECK(row.at("order_id").as_string() == "1");
  CHECK(row.at("journal_id").as_string() == "dead-1");
  CHECK(row.at("state").as_string() == "named");
  // Today's allowance counts the earlier run's placement once.
  CHECK(start.at("orders_left_today").as_int64() == 2);
  CHECK(events_of(ran, "placed").empty());
  CHECK(ran.reply_.at("placed").as_int64() == 0);
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  CHECK(ran.orders_after_.size() == 1);
  CHECK(status_of(ran, "1") == "canceled");
  CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
  // Started again, the account owes nothing.
  script_target(std::nullopt);
  const auto again = owed_run(fixture, {}, false);
  CHECK(again.exit_ == 0);
  REQUIRE(!again.events_.empty());
  CHECK(again.events_.front().at("owed_at_start").as_array().empty());
}

TEST_CASE("a run settles an earlier run's order that filled from the holdings, and one cancelled from the history") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(std::nullopt);
  SECTION("filled the day before") {
    const auto ran = owed_run(
      fixture,
      [](Dead& dead) {
        dead.place("005930", atl::OrderSide::BUY, 1, FILLS);
        dead.advance(std::chrono::hours(24));
      },
      true
    );
    CHECK(ran.exit_ == 0);
    REQUIRE(!ran.events_.empty());
    CHECK(owed_ids(ran.events_.front().at("owed_at_start")) == std::vector<std::string>{"dead-1"});
    // An earlier day's placement leaves today's allowance whole.
    CHECK(ran.events_.front().at("orders_left_today").as_int64() == 3);
    CHECK(ran.reply_.at("outcome").as_string() == "clean");
    CHECK(ran.held_after_.at("005930").as_string() == "1");
    CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
  }
  SECTION("cancelled in the broker's app") {
    const auto ran = owed_run(
      fixture,
      [](Dead& dead) {
        dead.place("005930", atl::OrderSide::BUY, 1, RESTS);
        dead.advance(std::chrono::minutes(1));
        REQUIRE(dead.market().cancel("1"));
        dead.advance(std::chrono::minutes(4));
      },
      true
    );
    CHECK(ran.exit_ == 0);
    REQUIRE(!ran.events_.empty());
    CHECK(owed_ids(ran.events_.front().at("owed_at_start")) == std::vector<std::string>{"dead-1"});
    CHECK(ran.reply_.at("outcome").as_string() == "clean");
    CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
  }
  const auto again = owed_run(fixture, {}, false);
  CHECK(again.exit_ == 0);
  REQUIRE(!again.events_.empty());
  CHECK(again.events_.front().at("owed_at_start").as_array().empty());
}

TEST_CASE("a run waits out an earlier run's delayed history and holdings, then settles its order") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(std::nullopt);
  const auto ran = owed_run(
    fixture,
    [](Dead& dead) {
      dead.place("005930", atl::OrderSide::BUY, 1, FILLS);
      // What the broker answered before the fill, answered again for a while after it.
      REQUIRE(dead.client().holdings());
      REQUIRE(dead.client().order_history(atl::OrderHistoryQuery{}));
      dead.advance(std::chrono::hours(24));
      dead.client().repeat_last(atl::mock::Call::HOLDINGS, 6);
      dead.client().repeat_last(atl::mock::Call::ORDER_HISTORY, 6);
    },
    true
  );
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(owed_ids(ran.events_.front().at("owed_at_start")) == std::vector<std::string>{"dead-1"});
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(ran.held_after_.at("005930").as_string() == "1");
  CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
  // Started again, the account owes nothing.
  const auto again = owed_run(fixture, {}, false);
  CHECK(again.exit_ == 0);
  REQUIRE(!again.events_.empty());
  CHECK(again.events_.front().at("owed_at_start").as_array().empty());
  CHECK(again.reply_.at("outcome").as_string() == "clean");
}

TEST_CASE("a withheld target is released against the holdings an earlier run's fill left, and buys nothing more") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  // The program wants one share; the earlier run's buy of one filled, and the holdings show it late.
  script_target(1);
  const auto earlier = [](Dead& dead) {
    dead.place("005930", atl::OrderSide::BUY, 1, FILLS);
    REQUIRE(dead.client().holdings());
    dead.advance(std::chrono::hours(24));
    dead.client().repeat_last(atl::mock::Call::HOLDINGS, 6);
  };
  const auto ran = owed_run(fixture, earlier, true);
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  // The start read the stale holdings.
  CHECK(ran.events_.front().at("holdings_at_start").as_object().at("005930").as_string() == "0");
  CHECK(events_of(ran, "placed").empty());
  CHECK(ran.reply_.at("placed").as_int64() == 0);
  CHECK(ran.orders_after_.size() == 1);
  CHECK(ran.held_after_.at("005930").as_string() == "1");
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
  // Started again on the account holding that share: nothing owed, and the target already met buys nothing.
  script_target(1);
  const auto again = owed_run(fixture, earlier, false);
  CHECK(again.exit_ == 0);
  REQUIRE(!again.events_.empty());
  CHECK(again.events_.front().at("owed_at_start").as_array().empty());
  CHECK(events_of(again, "placed").empty());
  CHECK(again.orders_after_.size() == 1);
  CHECK(again.held_after_.at("005930").as_string() == "1");
  CHECK(again.reply_.at("outcome").as_string() == "clean");
}

TEST_CASE("an earlier run's order stays owed while its history fails or lags, and a later start settles it") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(std::nullopt);
  // Cancelled in the broker's app: only its history can show it ended with nothing filled.
  const auto cancelled = [](Dead& dead) {
    dead.place("005930", atl::OrderSide::BUY, 1, RESTS);
    dead.advance(std::chrono::minutes(1));
    REQUIRE(dead.client().order_history(atl::OrderHistoryQuery{}));
    REQUIRE(dead.market().cancel("1"));
    dead.advance(std::chrono::minutes(4));
  };
  SECTION("the history fails") {
    const auto ran = owed_run(
      fixture,
      [&cancelled](Dead& dead) {
        cancelled(dead);
        dead.client().fail_next(atl::mock::Call::ORDER_HISTORY, read_failure(), 100000);
      },
      true
    );
    CHECK(ran.exit_ == 4);
    CHECK(owed_ids(ran.reply_.at("owed_at_end")) == std::vector<std::string>{"dead-1"});
    CHECK(events_of(ran, "placed").empty());
    CHECK(!has(outcomes_of(fixture, "dead-1"), "confirmed"));
  }
  SECTION("the history lags, still listing it working") {
    const auto ran = owed_run(
      fixture,
      [&cancelled](Dead& dead) {
        cancelled(dead);
        dead.client().repeat_last(atl::mock::Call::ORDER_HISTORY, 100000);
      },
      true
    );
    CHECK(ran.exit_ == 4);
    CHECK(owed_ids(ran.reply_.at("owed_at_end")) == std::vector<std::string>{"dead-1"});
    CHECK(events_of(ran, "placed").empty());
    CHECK(!has(outcomes_of(fixture, "dead-1"), "confirmed"));
  }
  // The journal kept it: a later start with the history readable settles it.
  const auto again = owed_run(fixture, cancelled, false);
  CHECK(again.exit_ == 0);
  REQUIRE(!again.events_.empty());
  CHECK(owed_ids(again.events_.front().at("owed_at_start")) == std::vector<std::string>{"dead-1"});
  CHECK(again.reply_.at("outcome").as_string() == "clean");
  CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
}

TEST_CASE("an earlier run's order the broker never saw, or one held for the operator, stays owed with exit 4") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(1);
  std::function<void(Dead&)> earlier;
  std::string state;
  SECTION("a PENDING line and no order: the run died before it sent") {
    state = "unnamed";
    earlier = [](Dead& dead) {
      atl::runner::JournalEntry pending;
      pending.id_ = "dead-1";
      pending.action_ = atl::runner::JournalAction::PLACE;
      pending.outcome_ = atl::runner::JournalOutcome::PENDING;
      pending.recorded_at_ = dead.market().now();
      pending.instrument_ = kr("005930");
      pending.side_ = atl::OrderSide::BUY;
      pending.quantity_ = atl::Decimal64(1, 0);
      pending.price_ = atl::Decimal64(RESTS, 0);
      pending.baseline_ = atl::Decimal64(0, 0);
      dead.append(pending);
      dead.advance(std::chrono::minutes(5));
    };
  }
  SECTION("an order the journal holds for the operator") {
    state = "operator";
    earlier = [](Dead& dead) {
      dead.place("005930", atl::OrderSide::BUY, 1, FILLS);
      auto held = dead.last();
      held.outcome_ = atl::runner::JournalOutcome::HELD;
      held.requires_operator_ = true;
      held.baseline_ = atl::Decimal64(5, 0);
      held.detail_ = "holdings moved past what the order explains";
      held.recorded_at_ = dead.market().now() + std::chrono::minutes(1);
      dead.append(held);
      dead.advance(std::chrono::hours(24));
    };
  }
  const auto ran = owed_run(fixture, earlier, true, std::chrono::minutes(30));
  CHECK(ran.exit_ == 4);
  // The owed-order limit stops the run: nothing more is placed, and what rests stays.
  CHECK(code_of(ran) == "unknown.owed_order");
  REQUIRE(!ran.events_.empty());
  const auto& owed = ran.events_.front().at("owed_at_start").as_array();
  REQUIRE(owed.size() == 1);
  CHECK(owed.front().as_object().at("state").as_string() == state);
  CHECK(events_of(ran, "placed").empty());
  CHECK(owed_ids(ran.reply_.at("owed_at_end")) == std::vector<std::string>{"dead-1"});
  CHECK(!has(outcomes_of(fixture, "dead-1"), "confirmed"));
  // Started again: still owed, still nothing placed.
  const auto again = owed_run(fixture, earlier, false, std::chrono::minutes(30));
  CHECK(again.exit_ == 4);
  CHECK(code_of(again) == "unknown.owed_order");
  CHECK(events_of(again, "placed").empty());
  CHECK(owed_ids(again.reply_.at("owed_at_end")) == std::vector<std::string>{"dead-1"});
  CHECK(!has(outcomes_of(fixture, "dead-1"), "confirmed"));
}

TEST_CASE("an earlier day's order whose reply was lost is named from the history and settled") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(std::nullopt);
  const auto ran = owed_run(
    fixture,
    [](Dead& dead) {
      dead.place("005930", atl::OrderSide::BUY, 1, RESTS, true);
      // It expires unfilled at the next bar; today's working list never shows it.
      dead.advance(std::chrono::hours(24));
    },
    true
  );
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  const auto& owed = ran.events_.front().at("owed_at_start").as_array();
  REQUIRE(owed.size() == 1);
  CHECK(owed.front().as_object().at("state").as_string() == "unnamed");
  CHECK(owed.front().as_object().at("order_id").as_string().empty());
  CHECK(ran.reply_.at("outcome").as_string() == "clean");
  const auto outcomes = outcomes_of(fixture, "dead-1");
  CHECK(has(outcomes, "indeterminate"));
  CHECK(has(outcomes, "confirmed"));
  const auto again = owed_run(fixture, {}, false);
  CHECK(again.exit_ == 0);
  REQUIRE(!again.events_.empty());
  CHECK(again.events_.front().at("owed_at_start").as_array().empty());
}

TEST_CASE("with today's allowance spent, a run inheriting a working order withdraws it and places nothing") {
  PaperFixture fixture;
  fixture.write_config(kis_config(1));
  fixture.arm();
  // The program wants two shares; the allowance is zero.
  script_target(2);
  const auto ran = owed_run(
    fixture,
    [](Dead& dead) {
      dead.place("005930", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    },
    true
  );
  CHECK(ran.exit_ == 0);
  REQUIRE(!ran.events_.empty());
  CHECK(ran.events_.front().at("orders_left_today").as_int64() == 0);
  CHECK(owed_ids(ran.events_.front().at("owed_at_start")) == std::vector<std::string>{"dead-1"});
  CHECK(events_of(ran, "placed").empty());
  CHECK(ran.reply_.at("placed").as_int64() == 0);
  CHECK(ran.orders_after_.size() == 1);
  CHECK(status_of(ran, "1") == "canceled");
  CHECK(ran.reply_.at("owed_at_end").as_array().empty());
  CHECK(has(outcomes_of(fixture, "dead-1"), "confirmed"));
  // Settled, nothing is owed, and the spent allowance refuses as before.
  const auto again = owed_run(fixture, {}, false);
  CHECK(again.exit_ == 3);
  CHECK(code_of(again) == "refused.daily_order_limit");
  CHECK(events_of(again, "start").empty());
}

TEST_CASE("a run refuses an earlier run's owed order on another symbol, and sends nothing, again after a restart") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(1);
  const auto earlier = [](Dead& dead) {
    dead.place("000660", atl::OrderSide::BUY, 1, RESTS);
    dead.advance(std::chrono::minutes(5));
  };
  for (const bool seed : {true, false}) {
    const auto ran = owed_run(fixture, earlier, seed);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran) == "unknown.owed_at_start");
    const auto& error = ran.reply_.at("error").as_object();
    CHECK(std::string(error.at("message").as_string()).find("000660") != std::string::npos);
    const std::string next(error.at("next").as_string());
    CHECK(next.find("ask the operator, who starts a run on 000660") != std::string::npos);
    sent_nothing(ran, 1);
  }
}

TEST_CASE("a run refuses a working order the journal does not own, on any symbol, and sends nothing") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(1);
  // The earlier run's order on the run's symbol was cancelled; another order rests that no run placed.
  std::function<void(Dead&)> earlier;
  SECTION("on another symbol") {
    earlier = [](Dead& dead) {
      dead.place("005930", atl::OrderSide::BUY, 1, RESTS);
      REQUIRE(dead.market().cancel("1"));
      dead.place_elsewhere("000660", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    };
  }
  SECTION("on the run's symbol, beside the earlier run's resting one") {
    earlier = [](Dead& dead) {
      dead.place("005930", atl::OrderSide::BUY, 1, RESTS);
      dead.place_elsewhere("005930", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    };
  }
  for (const bool seed : {true, false}) {
    const auto ran = owed_run(fixture, earlier, seed);
    CHECK(ran.exit_ == 3);
    CHECK(code_of(ran) == "refused.orders_resting");
    CHECK(std::string(ran.reply_.at("error").as_object().at("message").as_string()).rfind("1 order(s)", 0) == 0);
    CHECK(events_of(ran, "start").empty());
    CHECK(events_of(ran, "placed").empty());
    CHECK(ran.orders_after_.size() == 2);
    CHECK(status_of(ran, "2") == "pending");
    CHECK(ran.journal_after_ == ran.journal_before_);
  }
}

TEST_CASE("a run refuses a working order that only looks like an earlier run's, and sends nothing") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(1);
  // A placement line as the earlier run wrote it, acknowledged with the market's number 1 or never answered.
  const auto line =
    [](const std::string& symbol, atl::OrderSide side, std::chrono::system_clock::time_point at, bool acknowledged) {
      std::vector<atl::runner::JournalEntry> lines;
      atl::runner::JournalEntry entry;
      entry.id_ = "dead-1";
      entry.action_ = atl::runner::JournalAction::PLACE;
      entry.outcome_ = atl::runner::JournalOutcome::PENDING;
      entry.recorded_at_ = at;
      entry.instrument_ = kr(symbol);
      entry.side_ = side;
      entry.quantity_ = atl::Decimal64(1, 0);
      entry.price_ = atl::Decimal64(RESTS, 0);
      entry.baseline_ = atl::Decimal64(side == atl::OrderSide::BUY ? 0 : 1, 0);
      lines.push_back(entry);
      if (acknowledged) {
        entry.outcome_ = atl::runner::JournalOutcome::ACCEPTED;
        entry.order_id_ = "1";
        lines.push_back(entry);
      }
      return lines;
    };
  std::function<void(Dead&)> earlier;
  SECTION("an earlier day's placement that never reached the broker, and a lookalike working today") {
    earlier = [&line](Dead& dead) {
      for (const auto& entry : line("005930", atl::OrderSide::BUY, dead.market().now(), false)) {
        dead.append(entry);
      }
      dead.advance(std::chrono::hours(24));
      dead.place_elsewhere("005930", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    };
  }
  SECTION("today's acknowledged number on another instrument") {
    earlier = [&line](Dead& dead) {
      for (const auto& entry : line("005930", atl::OrderSide::BUY, dead.market().now(), true)) {
        dead.append(entry);
      }
      dead.place_elsewhere("000660", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    };
  }
  SECTION("today's acknowledged number on the other side") {
    earlier = [&line](Dead& dead) {
      for (const auto& entry : line("005930", atl::OrderSide::SELL, dead.market().now(), true)) {
        dead.append(entry);
      }
      dead.place_elsewhere("005930", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    };
  }
  SECTION("the number an earlier day's acknowledgement gave, given again today") {
    earlier = [&line](Dead& dead) {
      for (const auto& entry : line("005930", atl::OrderSide::BUY, dead.market().now(), true)) {
        dead.append(entry);
      }
      dead.advance(std::chrono::hours(24));
      dead.place_elsewhere("005930", atl::OrderSide::BUY, 1, RESTS);
      dead.advance(std::chrono::minutes(5));
    };
  }
  for (const bool seed : {true, false}) {
    const auto ran = owed_run(fixture, earlier, seed);
    CHECK(ran.exit_ == 3);
    CHECK(code_of(ran) == "refused.orders_resting");
    sent_nothing(ran, 1);
  }
}

TEST_CASE("a placement sent yesterday and acknowledged in today's journal owns no working order today") {
  PaperFixture fixture;
  fixture.write_config(kis_config());
  fixture.arm();
  script_target(std::nullopt);
  // The earlier run sent yesterday and was suspended before it wrote the broker's answer, number 1, which it wrote
  // today; today the broker gave number 1 to an unrelated order of the same shape.
  const auto earlier = [](Dead& dead) {
    atl::runner::JournalEntry entry;
    entry.id_ = "dead-1";
    entry.action_ = atl::runner::JournalAction::PLACE;
    entry.outcome_ = atl::runner::JournalOutcome::PENDING;
    entry.recorded_at_ = dead.market().now();
    entry.instrument_ = kr("005930");
    entry.side_ = atl::OrderSide::BUY;
    entry.quantity_ = atl::Decimal64(1, 0);
    entry.price_ = atl::Decimal64(RESTS, 0);
    entry.baseline_ = atl::Decimal64(0, 0);
    dead.append(entry);
    dead.advance(std::chrono::hours(24));
    entry.outcome_ = atl::runner::JournalOutcome::ACCEPTED;
    entry.order_id_ = "1";
    entry.recorded_at_ = dead.market().now();
    dead.append(entry);
    dead.place_elsewhere("005930", atl::OrderSide::BUY, 1, RESTS);
    dead.advance(std::chrono::minutes(1));
  };
  bool undated = false;
  SECTION("the working row carries its order time") {
    undated = false;
  }
  SECTION("the working row carries none, as KIS real-account rows") {
    undated = true;
  }
  for (const bool seed : {true, false}) {
    const auto ran = owed_run(fixture, earlier, seed, std::chrono::minutes(1), undated);
    CHECK(ran.exit_ == 3);
    CHECK(code_of(ran) == "refused.orders_resting");
    sent_nothing(ran, 1);
    CHECK(status_of(ran, "1") == "pending");
  }
}

TEST_CASE("a working order is the journal's only by today's acknowledged number with the same order") {
  using Tag = Session::BrokerTag;
  using cmdatl::driver::inherited_orders;
  const auto ids = std::vector<atl::InstrumentId>{kr("005930")};
  // 2024-03-27 00:30 UTC, 09:30 KST.
  const auto at = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 19809) + std::chrono::minutes(30);
  const auto today = cmdatl::paper::kst_day(at);
  const auto placement = [&](const std::string& id, atl::runner::JournalOutcome outcome, const std::string& number) {
    atl::runner::JournalEntry entry;
    entry.id_ = id;
    entry.action_ = atl::runner::JournalAction::PLACE;
    entry.outcome_ = outcome;
    entry.recorded_at_ = at;
    entry.instrument_ = kr("005930");
    entry.side_ = atl::OrderSide::BUY;
    entry.quantity_ = atl::Decimal64(2, 0);
    entry.price_ = atl::Decimal64(RESTS, 0);
    entry.baseline_ = atl::Decimal64(0, 0);
    entry.order_id_ = number;
    return entry;
  };
  const auto owed = [](const std::string& id, const std::string& number, const std::string& state) {
    cmdatl::Owed row;
    row.journal_id_ = id;
    row.symbol_ = "005930";
    row.side_ = "buy";
    row.quantity_ = "2";
    row.order_id_ = number;
    row.state_ = state;
    return row;
  };
  const auto working = [&](const std::string& number) {
    atl::WorkingOrder<Tag> row;
    row.order_.order_id_ = number;
    row.order_.instrument_ = kr("005930");
    row.order_.side_ = atl::OrderSide::BUY;
    row.order_.quantity_ = atl::Decimal64::parse("2.0");
    row.order_.price_ = atl::Decimal64(RESTS, 0);
    row.order_.ordered_at_ = at;
    return row;
  };
  atl::runner::JournalReplay replay;
  replay.entries_ = {
    placement("a-1", atl::runner::JournalOutcome::PENDING, ""),
    placement("a-1", atl::runner::JournalOutcome::ACCEPTED, "7"),
  };
  const std::vector<cmdatl::Owed> named{owed("a-1", "7", "named")};
  const auto owners =
    [&](
      const atl::runner::JournalReplay& journal, const std::vector<cmdatl::Owed>& rows,
      const std::vector<atl::WorkingOrder<Tag>>& listed
    ) {
      return inherited_orders<Tag>(journal, rows, ids, today, listed);
    };

  // The same order: owned, once.
  CHECK(owners(replay, named, {working("7")}) == std::vector<std::string>{"a-1"});
  CHECK(owners(replay, named, {working("7"), working("7")}) == std::vector<std::string>{"a-1", ""});
  // A row without a price or a date is judged on the rest.
  auto bare = working("7");
  bare.order_.price_.reset();
  bare.order_.ordered_at_.reset();
  CHECK(owners(replay, named, {bare}) == std::vector<std::string>{"a-1"});
  // Another number, quantity, price, order date, an assumed date, or another day's run: not owned.
  CHECK(owners(replay, named, {working("8")}) == std::vector<std::string>{""});
  auto other = working("7");
  other.order_.quantity_ = atl::Decimal64(3, 0);
  CHECK(owners(replay, named, {other}) == std::vector<std::string>{""});
  other = working("7");
  other.order_.price_ = atl::Decimal64(RESTS + 100, 0);
  CHECK(owners(replay, named, {other}) == std::vector<std::string>{""});
  other = working("7");
  other.order_.ordered_at_ = at - std::chrono::hours(24);
  CHECK(owners(replay, named, {other}) == std::vector<std::string>{""});
  other = working("7");
  other.order_.ordered_date_assumed_ = true;
  CHECK(owners(replay, named, {other}) == std::vector<std::string>{""});
  CHECK(inherited_orders<Tag>(replay, named, ids, today + 1, {working("7")}) == std::vector<std::string>{""});
  // Not a run instrument, not named, or held for the operator: not owned.
  CHECK(inherited_orders<Tag>(replay, named, {kr("000660")}, today, {working("7")}) == std::vector<std::string>{""});
  CHECK(owners(replay, {owed("a-1", "", "unnamed")}, {working("7")}) == std::vector<std::string>{""});
  CHECK(owners(replay, {owed("a-1", "7", "operator")}, {working("7")}) == std::vector<std::string>{""});
  // A number only a sweep wrote is no acknowledgement.
  atl::runner::JournalReplay swept;
  swept.entries_ = {
    placement("a-1", atl::runner::JournalOutcome::PENDING, ""),
    placement("a-1", atl::runner::JournalOutcome::INDETERMINATE, ""),
    placement("a-1", atl::runner::JournalOutcome::RESOLVED, "7"),
  };
  CHECK(owners(swept, named, {working("7")}) == std::vector<std::string>{""});
  // Lines naming two numbers, or another placement naming the same one: not owned.
  auto contested = replay;
  contested.entries_.push_back(placement("a-1", atl::runner::JournalOutcome::RESOLVED, "9"));
  CHECK(owners(contested, named, {working("7")}) == std::vector<std::string>{""});
  contested = replay;
  contested.entries_.push_back(placement("b-1", atl::runner::JournalOutcome::PENDING, ""));
  contested.entries_.push_back(placement("b-1", atl::runner::JournalOutcome::ACCEPTED, "7"));
  CHECK(owners(contested, named, {working("7")}) == std::vector<std::string>{""});
  CHECK(
    owners(contested, {owed("a-1", "7", "named"), owed("b-1", "7", "named")}, {working("7")}) ==
    std::vector<std::string>{""}
  );
  // Sent yesterday and acknowledged today, or no line showing the send: not owned.
  auto late = replay;
  late.entries_.front().recorded_at_ = at - std::chrono::hours(24);
  CHECK(owners(late, named, {working("7")}) == std::vector<std::string>{""});
  atl::runner::JournalReplay unsent;
  unsent.entries_ = {placement("a-1", atl::runner::JournalOutcome::ACCEPTED, "7")};
  CHECK(owners(unsent, named, {working("7")}) == std::vector<std::string>{""});
  // An answer stamped before its send line: the send day is not shown.
  auto backwards = replay;
  backwards.entries_.back().recorded_at_ = at - std::chrono::minutes(1);
  CHECK(owners(backwards, named, {working("7")}) == std::vector<std::string>{""});
  // A line marked for the operator (a damaged line salvaged): not owned.
  auto damaged = replay;
  damaged.entries_.back().requires_operator_ = true;
  CHECK(owners(damaged, named, {working("7")}) == std::vector<std::string>{""});
}

#endif
