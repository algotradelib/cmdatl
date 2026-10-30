// The order commands in process: their arguments, their checks in order, the plan book, once()'s mapping, release(),
// reconcile's rows, the owed rows' hints, and the safety items the reviews assigned to them.

#include "./order-support.hpp"

#include <cmdatl/cli.hpp>
#include <cmdatl/command_list.hpp>
#include <cmdatl/once.hpp>
#include <cmdatl/orders.hpp>
#include <cmdatl/plans.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algotradelib/runner>

#include <climits>
#include <random>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>

using namespace support;
namespace atl = algotradelib;
namespace paper = cmdatl::paper;
namespace sending = cmdatl::sending;

namespace {

/** @brief The account's journal scope in the fixtures. */
const std::string SCOPE = "paper:paper:paper-0001";

/**
 * @brief Arms a fixture and moves it to the first session's open.
 * @param fixture The account.
 */
void open_session(const PaperFixture& fixture) {
  fixture.arm();
  REQUIRE(ask(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);
}

/**
 * @brief The day's price range of a symbol in the fixture's bars, for D5's check.
 * @param symbol The symbol.
 * @param at The instant, nanoseconds.
 * @return A price inside the day's range nearest to nothing in particular: the day's open.
 */
double day_mark(const std::string& symbol, std::int64_t at) {
  (void)at;
  return symbol == "005930" ? 78500 : 181000;
}

/**
 * @brief A journal entry for a placement.
 * @param id The journal id.
 * @param outcome The outcome.
 * @param at The instant.
 * @param order_id The broker's id.
 * @param symbol The symbol.
 * @return The entry.
 */
atl::runner::JournalEntry place_entry(
  const std::string& id,
  atl::runner::JournalOutcome outcome,
  paper::Clock::time_point at,
  const std::string& order_id = "",
  const std::string& symbol = "005930"
) {
  atl::runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = atl::runner::JournalAction::PLACE;
  entry.outcome_ = outcome;
  entry.recorded_at_ = at;
  entry.instrument_ = paper::kr(symbol);
  entry.side_ = atl::OrderSide::BUY;
  entry.type_ = atl::OrderType::LIMIT;
  entry.time_in_force_ = atl::TimeInForce::DAY;
  entry.session_ = atl::OrderSession::REGULAR;
  entry.venue_ = atl::Venue::UNSPECIFIED;
  entry.quantity_ = atl::Decimal64(10, 0);
  entry.price_ = atl::Decimal64(75000, 0);
  entry.baseline_ = atl::Decimal64(0, 0);
  entry.order_id_ = order_id;
  return entry;
}

/**
 * @brief Appends entries to the fixture's journal.
 * @param fixture The account.
 * @param entries The entries.
 */
void journal(const PaperFixture& fixture, const std::vector<atl::runner::JournalEntry>& entries) {
  atl::runner::FileJournal file(fixture.journal().string(), SCOPE);
  REQUIRE(file.open());
  for (const auto& entry : entries) {
    REQUIRE(file.append(entry));
  }
}

/** @brief The report type once() maps. */
using Report = atl::runner::DryRunReport<cmdatl::paper::PaperDesk::Session::BrokerTag>;

}  // namespace

TEST_CASE("order arguments: each grammar at its edges and refused before the account opens") {
  const PaperFixture fixture;
  const std::vector<std::vector<std::string>> bad{
    {"order", "plan", "005930", "--target", "50.5", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "-1", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "+5", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "05", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "1e2", "--limit", "78500"},
    {"order", "plan", "005930", "--target", " 5", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "5 ", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "1000000000", "--limit", "78500"},
    {"order", "plan", "005930", "--target", "1", "--limit", "0"},
    {"order", "plan", "005930", "--target", "1", "--limit", "0.5"},
    {"order", "plan", "005930", "--target", "1", "--limit", "10000001"},
    {"order", "plan", "005930", "--target", "1", "--limit", "1.00001"},
    {"order", "plan", "005930", "--target", "1", "--limit", "abc"},
    {"order", "plan", "00 5930", "--target", "1", "--limit", "78500"},
    {"order", "place", "p123"},
    {"order", "place", "P0123456789"},
    {"order", "place", "p0123456789a"},
    {"order", "place", "p012345678G"},
    {"order", "release", "005930", "--reason", ""},
    {"order", "release", "005930", "--reason", "   "},
    {"order", "release", "005930", "--reason", std::string(201, 'x')},
    {"order", "release", "005930", "--reason", "a\x01" "b"},
    {"order", "release", "005930", "--reason", "\xc3\x28"},
    {"order", "release", "005930", "--reason", "\xed\xa0\x80"},
    {"order", "cancel", "005930/x"},
  };
  for (const auto& args : bad) {
    INFO(cmdatl::join(args));
    const auto answer = ask(fixture, args);
    CHECK(answer.exit_ == 2);
    CHECK(answer.code_ == "usage.bad_value");
  }
  // No lock taken, no owner record written: the account's directory was never made.
  CHECK(!std::filesystem::exists(fixture.account()));
  CHECK(cmdatl::valid_target("0"));
  CHECK(cmdatl::valid_target("999999999"));
  CHECK(cmdatl::valid_limit("1"));
  CHECK(cmdatl::valid_limit("10000000"));
  CHECK(cmdatl::valid_limit("78500.1234"));
  CHECK(cmdatl::valid_reason(std::string(200, 'x')));
  CHECK(cmdatl::valid_reason("읽었습니다: 주문 목록과 보유 주식"));
}

TEST_CASE("plan and then place by id: one limit order and once and and the plan cannot send again") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto id = plan_id(fixture, "005930", "10", "75000");
  const auto placed = ask_checked(fixture, {"order", "place", id});
  INFO(boost::json::serialize(placed.body_));
  REQUIRE(placed.exit_ == 0);
  CHECK(placed.reason_ == "placed");
  CHECK(placed.body_.at("env").as_string() == "paper");
  const auto& order = placed.body_.at("order").as_object();
  CHECK(order.at("side").as_string() == "buy");
  CHECK(order.at("quantity").as_string() == "10");
  CHECK(order.at("limit").as_string() == "75000");
  CHECK(order.at("order_id").as_string() == "1");
  CHECK(placed.body_.at("owed").as_array().size() == 1);
  const auto again = ask_checked(fixture, {"order", "place", id});
  CHECK(again.exit_ == 3);
  CHECK(again.code_ == "refused.plan_used");
  const auto log = slurp(fixture.log());
  CHECK(ops_of(log, "place").size() == 1);
  CHECK(limits_in_collar(log, 0.05, day_mark));
  // B3: the journal once() wrote carries the account's scope.
  CHECK(atl::runner::read_journal_file(fixture.journal().string()).scope_ == SCOPE);
}

TEST_CASE("plan checks and in order and each refusing alone with nothing sent") {
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"005930\"]\n"
    "position = { \"005930\" = 100 }\n"
    "order_value = 785000\n"
    "daily_orders = 1\n";
  const PaperFixture fixture(parts);
  open_session(fixture);
  CHECK(ask_checked(fixture, {"order", "plan", "000660", "--target", "1", "--limit", "181000"}).code_ == "refused.symbol_not_allowed");
  // The first failing check wins: a symbol not allowed and a target over any cap.
  CHECK(ask_checked(fixture, {"order", "plan", "000660", "--target", "500", "--limit", "181000"}).code_ == "refused.symbol_not_allowed");
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "101", "--limit", "78500"}).code_ == "refused.position_limit");
  // The order's value equal to the ceiling passes; one tick above refuses.
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "78500"}).reason_ == "plan");
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "78600"}).code_ == "refused.order_value");
  // The collar and the tick grid are once()'s.
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "70000"}).code_ == "refused.collar");
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "78550"}).code_ == "refused.off_tick");
  // The daily count: the fourth placement of a day with three allowed, here the second with one.
  const auto id = plan_id(fixture, "005930", "5", "75000");
  REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "6", "--limit", "75000"}).code_ == "refused.daily_order_limit");
  // A target that changes no holding is not a placement: no daily refusal (it plans to withdraw the working 5).
  const auto withdraw = ask_checked(fixture, {"order", "plan", "005930", "--target", "0", "--limit", "75000"});
  CHECK(withdraw.reason_ == "plan");
  CHECK(withdraw.body_.at("would").as_string() == "withdraw");
}

TEST_CASE("the collar's exact edges at 0.05 and 0.30: the edge plans and places and the next tick outward refuses") {
  // 005930 flat at 80000, so the last price at every instant is 80000 and each edge is a whole number of ticks.
  const std::string bars =
    "symbol,date,open,high,low,close,volume\n"
    "005930,2024-03-25,80000,80000,80000,80000,1000000\n"
    "005930,2024-03-26,80000,80000,80000,80000,1000000\n"
    "005930,2024-03-27,80000,80000,80000,80000,1000000\n";
  struct Edge {
    std::string collar_;
    std::string printed_;
    std::string inside_;
    std::string outside_;
  };
  const std::vector<Edge> edges{
    {"0.05", "0.05", "84000", "84100"},
    {"0.05", "0.05", "76000", "75900"},
    {"0.30", "0.3", "104000", "104100"},
    {"0.30", "0.3", "56000", "55900"},
  };
  for (const auto& edge : edges) {
    INFO(edge.collar_ << " " << edge.inside_);
    ConfigParts parts;
    parts.limits_ =
      "symbols = [\"005930\"]\nposition = { \"005930\" = 100 }\norder_value = 5000000\ndaily_orders = 3\n"
      "collar = " + edge.collar_ + "\n";
    const PaperFixture fixture(parts, bars);
    open_session(fixture);
    const auto id = plan_id(fixture, "005930", "1", edge.inside_);
    const auto outside = ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", edge.outside_});
    CHECK(outside.exit_ == 3);
    CHECK(outside.code_ == "refused.collar");
    // The collar as env prints it.
    CHECK(
      std::string(outside.body_.at("error").as_object().at("message").as_string()) ==
      "limit " + edge.outside_ + " is outside " + edge.printed_ + " of the last price 80000"
    );
    const auto placed = ask_checked(fixture, {"order", "place", id});
    INFO(boost::json::serialize(placed.body_));
    CHECK(placed.exit_ == 0);
    CHECK(placed.reason_ == "placed");
    CHECK(placed.body_.at("order").as_object().at("limit").as_string() == edge.inside_);
  }
}

TEST_CASE("place checks the operator's file as it is now: a lowered cap and a removed symbol and the armed file") {
  const PaperFixture fixture;
  open_session(fixture);
  auto id = plan_id(fixture, "005930", "10", "75000");
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"005930\", \"000660\"]\n"
    "position = { \"005930\" = 5, \"000660\" = 20 }\n"
    "order_value = 5000000\n"
    "daily_orders = 3\n";
  fixture.write_config(parts.text());
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.position_limit");
  parts.limits_ = "symbols = [\"000660\"]\nposition = { \"000660\" = 20 }\norder_value = 5000000\ndaily_orders = 3\n";
  fixture.write_config(parts.text());
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.symbol_not_allowed");
  fixture.write_config(ConfigParts{}.text());
  std::filesystem::remove(fixture.armed());
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.not_armed");
  fixture.arm();
  // A plan refused by a precheck is not consumed.
  CHECK(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
}

TEST_CASE("the plan book: stale when the account moves or the plan ages and and bound to the account") {
  const PaperFixture fixture;
  open_session(fixture);
  // The journal's size: another placement moved it.
  auto id = plan_id(fixture, "005930", "10", "75000");
  const auto other = plan_id(fixture, "000660", "1", "175000");
  REQUIRE(ask_checked(fixture, {"order", "place", other}).reason_ == "placed");
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.stale_plan");
  // A working order on the symbol changing (filled), and the cash.
  id = plan_id(fixture, "000660", "2", "175000");
  REQUIRE(ask(fixture, {"order", "cancel", "000660"}).reason_ == "withdrawal_sent");
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.stale_plan");
  // Age: plan_ttl (60 s) plus a little.
  id = plan_id(fixture, "005930", "10", "75000");
  REQUIRE(ask(fixture, {"clock", "advance", "--minutes", "1"}).exit_ == 0);
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.stale_plan");
  // A plan book copied into another account's directory: the scope differs.
  id = plan_id(fixture, "005930", "10", "75000");
  const auto book = slurp(fixture.account() / "plans.json");
  const auto now = std::string(ask(fixture, {"clock"}).body_.at("now").as_string());
  ConfigParts parts;
  parts.account_ = "broker = \"paper\"\nnumber = \"paper-0002\"\n";
  const PaperFixture second(parts);
  second.arm();
  // A minute later than the plan, inside plan_ttl: the clock moves to the next whole minute.
  const auto next_minute = paper::kst_iso(*paper::parse_kst(now.substr(0, 16)) + std::chrono::minutes(1)).substr(0, 16);
  REQUIRE(ask(second, {"clock", "advance", "--to", next_minute}).exit_ == 0);
  const auto dir = second.dir() / "state" / "cmdatl" / "accounts" / "paper-paper-paper-0002";
  spit(dir / "plans.json", book);
  CHECK(ask(second, {"order", "place", id}).code_ == "refused.stale_plan");
}

TEST_CASE("a damaged plan book is an empty book: place answers no_plan and plan replaces it") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto id = plan_id(fixture, "005930", "10", "75000");
  const auto good = slurp(fixture.account() / "plans.json");
  const auto path = fixture.account() / "plans.json";
  const std::vector<std::pair<std::string, std::string>> texts{
    {"malformed", good.substr(0, good.size() / 2)},
    {"another format", std::string(good).replace(good.find("\"format\":1"), 10, "\"format\":2")},
    {"a missing member", std::string(good).replace(good.find(",\"used\":false"), 13, "")},
    {"an extra member", std::string(good).replace(good.find("\"used\":false"), 12, "\"used\":false,\"x\":1")},
    {"a repeated member", std::string(good).replace(good.find("\"used\":false"), 12, "\"used\":true,\"used\":false")},
    {"too large", good.substr(0, good.size() - 2) + std::string(65536, ' ') + "}\n"},
  };
  for (const auto& entry : texts) {
    INFO(entry.first);
    spit(path, entry.second);
    CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.no_plan");
    spit(path, entry.second);
    plan_id(fixture, "005930", "10", "75000");
    CHECK(cmdatl::PlanBook::parse(slurp(path)).has_value());
  }
  // A symbolic link, a hard link to the journal, a FIFO (no hang).
  std::filesystem::remove(path);
  std::filesystem::create_symlink(fixture.dir() / "elsewhere.json", path);
  spit(fixture.dir() / "elsewhere.json", good);
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.no_plan");
  std::filesystem::remove(path);
  std::filesystem::create_hard_link(fixture.dir() / "elsewhere.json", path);
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.no_plan");
  std::filesystem::remove(path);
  REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.no_plan");
  std::filesystem::remove(path);
  // A stray temporary is account state that changes nothing.
  spit(fixture.account() / "plans.1.0000000000000000.tmp", "junk");
  plan_id(fixture, "005930", "10", "75000");
}

TEST_CASE("the plan book's values: every rule refuses the whole book and and the age is computed only in range") {
  const auto book = [](const std::string& key, const std::string& members) {
    return "{\"format\":1,\"plans\":{\"" + key + "\":{" + members + "}}}";
  };
  const std::string ok_members =
    "\"symbol\":\"005930\",\"target\":\"10\",\"limit\":\"75000\",\"snapshot\":\"0123456789abcdef\",\"sim\":1711411200000000000,\"used\":false";
  REQUIRE(cmdatl::PlanBook::parse(book("p0123456789", ok_members)).has_value());
  const auto with = [&ok_members](const std::string& from, const std::string& to) {
    auto members = ok_members;
    members.replace(members.find(from), from.size(), to);
    return members;
  };
  const std::vector<std::string> bad{
    with("1711411200000000000", std::to_string(LLONG_MIN)),
    with("1711411200000000000", std::to_string(LLONG_MAX)),
    with("1711411200000000000", std::to_string(paper::nanos(paper::first_instant()) - 1)),
    with("1711411200000000000", std::to_string(paper::nanos(paper::end_instant()))),
    with("\"target\":\"10\"", "\"target\":\"-1\""),
    with("\"target\":\"10\"", "\"target\":\"1.5\""),
    with("\"target\":\"10\"", "\"target\":\"1000000000\""),
    with("\"target\":\"10\"", "\"target\":\"05\""),
    with("\"limit\":\"75000\"", "\"limit\":\"0\""),
    with("\"limit\":\"75000\"", "\"limit\":\"0.5\""),
    with("\"limit\":\"75000\"", "\"limit\":\"10000001\""),
    with("\"limit\":\"75000\"", "\"limit\":\"1.00001\""),
    with("\"limit\":\"75000\"", "\"limit\":\"abc\""),
    with("0123456789abcdef", "0123456789abcde"),
    with("0123456789abcdef", "0123456789abcdef0"),
    with("0123456789abcdef", "0123456789ABCDEF"),
    with("\"used\":false", "\"used\":0"),
    with("\"sim\":1711411200000000000", "\"sim\":1.7e18"),
  };
  for (const auto& members : bad) {
    INFO(members);
    CHECK(!cmdatl::PlanBook::parse(book("p0123456789", members)).has_value());
  }
  CHECK(!cmdatl::PlanBook::parse(book("q0123456789", ok_members)).has_value());
  CHECK(!cmdatl::PlanBook::parse(book("p012345678", ok_members)).has_value());
  std::string many = "{\"format\":1,\"plans\":{";
  for (int i = 0; i < 65; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "p%010d", i);
    many += std::string(i == 0 ? "" : ",") + "\"" + key + "\":{" + ok_members + "}";
  }
  CHECK(!cmdatl::PlanBook::parse(many + "}}").has_value());
}

TEST_CASE("a plan's age: exactly plan_ttl is accepted and one clock tick more is stale and a future instant is damage") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto path = fixture.account() / "plans.json";
  for (const std::int64_t offset : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}}) {
    INFO(offset);
    const auto id = plan_id(fixture, "005930", "10", "75000");
    auto text = slurp(path);
    auto plans = cmdatl::PlanBook::parse(text);
    REQUIRE(plans);
    // The place's own second moves the clock one second: the plan's instant is set so that the age at place is
    // plan_ttl plus the offset.
    const auto now = paper::nanos(*paper::parse_kst(std::string(ask(fixture, {"clock"}).body_.at("now").as_string()).substr(0, 16)));
    (void)now;
    const auto clock = paper::nanos(paper::Clock::time_point(std::chrono::duration_cast<paper::Clock::duration>(
      std::chrono::nanoseconds(plans->back().second.sim_)
    )));
    const auto sim = std::to_string(plans->back().second.sim_);
    const auto place_at = clock + 1000000000LL;
    // The clock's own tick: a microsecond on macOS, a nanosecond on Linux; finer instants are refused as damage.
    const auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(paper::Clock::duration(1)).count();
    const auto wanted = offset < 0 ? place_at + tick : place_at - 60000000000LL - offset * tick;
    text.replace(text.find(sim), sim.size(), std::to_string(wanted));
    spit(path, text);
    const auto answer = ask_checked(fixture, {"order", "place", id});
    if (offset == 0) {
      CHECK(answer.reason_ == "placed");
      REQUIRE(ask(fixture, {"order", "cancel", "005930"}).exit_ == 0);
      REQUIRE(ask(fixture, {"order", "reconcile", "005930"}).exit_ == 0);
    } else if (offset == 1) {
      CHECK(answer.code_ == "refused.stale_plan");
    } else {
      CHECK(answer.code_ == "refused.no_plan");
    }
  }
}

TEST_CASE("map_report: every step reached and and its precedence") {
  using sending::OnceExit;
  const auto mapped = [](const Report& report, std::uint64_t logged, bool failed, std::size_t withdrawals) {
    return sending::map_report(
      report, logged, failed, withdrawals, "[NOT_ARMED] binding text", "[refused] market text"
    );
  };
  Report placed;
  placed.would_place_.emplace_back();
  CHECK(mapped(placed, 1, false, 0).reason_ == "placed");
  // A pure placement with a failed journal answer is 4, not 0.
  CHECK(mapped(placed, 1, true, 0).reason_ == "journal_write");
  CHECK(mapped(placed, 1, true, 0).exit_ == OnceExit::UNKNOWN);
  Report withdrawn;
  withdrawn.would_cancel_.emplace_back();
  CHECK(mapped(withdrawn, 1, false, 1).reason_ == "withdrawal_sent");
  CHECK(mapped(withdrawn, 1, true, 1).exit_ == OnceExit::UNKNOWN);
  CHECK(mapped(withdrawn, 1, false, 2).reason_ == "withdrawal");
  Report indeterminate = placed;
  indeterminate.indeterminate_.emplace_back();
  CHECK(mapped(indeterminate, 1, true, 0).reason_ == "indeterminate");
  Report broker;
  broker.rejected_.push_back({atl::strategy::RejectReason::BROKER_REFUSED, paper::kr("005930"), ""});
  CHECK(mapped(broker, 1, false, 0).reason_ == "broker_refused");
  CHECK(mapped(broker, 1, false, 0).exit_ == OnceExit::UNKNOWN);
  // The market's own text is kept; the binding's is not its to say.
  CHECK(mapped(broker, 1, false, 0).detail_.find("([refused] market text)") != std::string::npos);
  CHECK(mapped(broker, 1, false, 0).detail_.find("binding text") == std::string::npos);
  CHECK(
    sending::map_report(broker, 1, false, 0, "", "").detail_ ==
    "the request reached the paper market, which refused it; nothing rests"
  );
  CHECK(mapped(broker, 1, true, 0).exit_ == OnceExit::UNKNOWN);
  Report binding;
  binding.rejected_.push_back({atl::strategy::RejectReason::BINDING_REFUSED, paper::kr("005930"), ""});
  CHECK(mapped(binding, 0, false, 0).reason_ == "not_reached");
  CHECK(mapped(binding, 0, false, 0).exit_ == OnceExit::REFUSED);
  CHECK(
    mapped(binding, 0, false, 0).detail_ ==
    "the paper client refused the request before writing it: [NOT_ARMED] binding text"
  );
  CHECK(
    sending::map_report(binding, 0, false, 0, "", "").detail_ == "the paper client refused the request before writing it"
  );
  CHECK(mapped(binding, 0, true, 0).reason_ == "journal_unwritable");
  CHECK(mapped(binding, 0, true, 0).exit_ == OnceExit::REFUSED);
  Report guard;
  guard.refused_.emplace_back();
  CHECK(mapped(guard, 0, false, 0).reason_ == "guard");
  Report withheld;
  withheld.withheld_.emplace_back();
  CHECK(mapped(withheld, 0, false, 0).reason_ == "withheld");
  CHECK(mapped(withheld, 1, false, 0).reason_ == "unmapped");
  Report quiet;
  CHECK(mapped(quiet, 0, false, 0).reason_ == "met");
  Report mixed = placed;
  mixed.refused_.emplace_back();
  CHECK(mapped(mixed, 1, false, 0).reason_ == "mixed");
}

TEST_CASE("reconcile's rows and its overall exit and as a table") {
  sending::OnceResult result;
  result.exit_ = sending::OnceExit::DONE;
  result.reason_ = "met";
  CHECK(cmdatl::reconcile_row(result).state_ == "clean");
  result.reason_ = "plan";
  result.plan_kind_ = "withdraw";
  CHECK(cmdatl::reconcile_row(result).state_ == "working");
  CHECK(cmdatl::reconcile_row(result).exit_ == cmdatl::Exit::OK);
  result.plan_kind_ = "buy";
  CHECK(cmdatl::reconcile_row(result).state_ == "holding_moved");
  CHECK(cmdatl::reconcile_row(result).exit_ == cmdatl::Exit::REFUSED);
  result.exit_ = sending::OnceExit::UNKNOWN;
  result.reason_ = "unresolved";
  CHECK(cmdatl::reconcile_row(result).code_ == "unknown.unresolved");
  const cmdatl::ReconcileRow clean{"clean", cmdatl::Exit::OK, ""};
  const cmdatl::ReconcileRow refused{"awaiting_confirmation", cmdatl::Exit::REFUSED, "refused.awaiting_confirmation"};
  const cmdatl::ReconcileRow unknown{"unresolved", cmdatl::Exit::UNKNOWN, "unknown.unresolved"};
  CHECK(cmdatl::reconcile_exit({clean}, false).first == cmdatl::Exit::OK);
  CHECK(cmdatl::reconcile_exit({clean}, true).second == "refused.awaiting");
  CHECK(cmdatl::reconcile_exit({clean, refused, unknown}, false).second == "unknown.unresolved");
  CHECK(cmdatl::reconcile_exit({refused, clean}, false).second == "refused.awaiting_confirmation");
  CHECK(cmdatl::reconcile_exit({}, false).first == cmdatl::Exit::OK);
}

TEST_CASE("the owed rows' next and by case") {
  CHECK(cmdatl::owed_next("005930", "named", std::string("pending")).find("cmdatl order cancel 005930") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "named", std::string("filled")).find("records it from holdings") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "named", std::string("canceled")).find("records it from holdings") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "unnamed", std::nullopt).find("never named it") != std::string::npos);
  CHECK(cmdatl::owed_next("005930", "operator", std::nullopt).find("only the operator") != std::string::npos);
}

TEST_CASE("approval files: the grammar and line by line") {
  const auto symbol = [](std::string_view text) { return paper::valid_symbol(text); };
  CHECK(sending::parse_approval("005930 j1\n005930 j2\r\n000660 j3\n", "005930", symbol).ids_ == std::vector<std::string>{"j1", "j2"});
  CHECK(sending::parse_approval("000660 j3\n", "005930", symbol).ids_.empty());
  CHECK(sending::parse_approval("005930 j1\n\n", "005930", symbol).refused_ == "line 2 is not SYMBOL JOURNAL_ID with one space");
  CHECK(sending::parse_approval("005930  j1\n", "005930", symbol).refused_.find("line 1") == 0);
  CHECK(sending::parse_approval("005930\tj1\n", "005930", symbol).refused_.find("line 1") == 0);
  CHECK(sending::parse_approval("005930 j1 x\n", "005930", symbol).refused_.find("line 1") == 0);
  CHECK(sending::parse_approval("005930 j1", "005930", symbol).refused_.find("no line ending") != std::string::npos);
  CHECK(sending::parse_approval("005930 " + std::string(129, 'a') + "\n", "005930", symbol).refused_.find("line 1") == 0);
  CHECK(sending::parse_approval("", "005930", symbol).refused_ == "it is empty");
  CHECK(sending::valid_journal_id("1793396756070367000-1"));
  CHECK(!sending::valid_journal_id("a/b"));
}

TEST_CASE("order release: the approval names exactly the owed set and renamed after one use") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto at = paper::Clock::time_point(std::chrono::duration_cast<paper::Clock::duration>(
    std::chrono::nanoseconds(paper::nanos(*paper::parse_kst("2024-03-26T09:00")))
  ));
  journal(fixture, {
    place_entry("j1", atl::runner::JournalOutcome::PENDING, at),
    place_entry("j2", atl::runner::JournalOutcome::PENDING, at, "", "000660"),
  });
  const auto approval = fixture.dir() / "paper.release";
  // Missing, a symbolic link to nothing, a FIFO, oversized, malformed.
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read the order list"}).code_ == "refused.not_approved");
  ::mkfifo(approval.c_str(), 0600);
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  std::filesystem::remove(approval);
  spit(approval, std::string(70000, 'x'));
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  spit(approval, "005930 j1\nbad line here\n");
  auto refused = ask_checked(fixture, {"order", "release", "005930", "--reason", "read"});
  CHECK(refused.code_ == "refused.not_approved");
  CHECK(std::string(refused.body_.at("error").as_object().at("message").as_string()).find("line 2") != std::string::npos);
  // Another symbol's lines alone, a superset, a subset (here: the wrong id).
  spit(approval, "000660 j2\n");
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  spit(approval, "005930 j1\n005930 j9\n");
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  spit(approval, "005930 j9\n");
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  // Disarmed.
  spit(approval, "005930 j1\n000660 j2\n");
  std::filesystem::remove(fixture.armed());
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_armed");
  fixture.arm();
  // The owed symbol answers unknown until released.
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "75000"}).exit_ == 4);
  const auto released = ask_checked(fixture, {"order", "release", "005930", "--reason", "read the order list and holdings"});
  INFO(boost::json::serialize(released.body_));
  CHECK(released.exit_ == 0);
  CHECK(released.reason_ == "released");
  CHECK(released.body_.at("released").as_array().size() == 1);
  CHECK(!released.body_.at("approval_kept").as_bool());
  CHECK(!std::filesystem::exists(approval));
  CHECK(ask(fixture, {"account", "--symbol", "005930"}).body_.at("owed").as_array().empty());
  CHECK(ask(fixture, {"account", "--symbol", "000660"}).body_.at("owed").as_array().size() == 1);
  // The same release again: no approval file.
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  spit(approval, "005930 j1\n");
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.nothing_owed");
  // After the release a plan works again.
  plan_id(fixture, "005930", "1", "75000");
}

TEST_CASE("order release refuses a named order and which waits for holdings") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto id = plan_id(fixture, "005930", "10", "75000");
  REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
  const auto owed = ask(fixture, {"account"}).body_.at("owed").as_array();
  REQUIRE(owed.size() == 1);
  spit(fixture.dir() / "paper.release", "005930 " + std::string(owed[0].as_object().at("journal_id").as_string()) + "\n");
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.named");
}

TEST_CASE("H5: a cancel needs no armed file and no permitted phase and and never places") {
  ConfigParts parts;
  parts.limits_ += "phases = [\"pre_open\", \"open\"]\n";
  const PaperFixture fixture(parts);
  fixture.arm();
  REQUIRE(ask(fixture, {"clock", "advance", "--to", "2024-03-26T08:40"}).exit_ == 0);
  const auto id = plan_id(fixture, "005930", "10", "75000");
  const auto placed = ask_checked(fixture, {"order", "place", id});
  INFO(boost::json::serialize(placed.body_));
  REQUIRE(placed.reason_ == "placed");
  fixture.write_config(ConfigParts{}.text());
  std::filesystem::remove(fixture.armed());
  REQUIRE(ask(fixture, {"clock", "advance", "--to", "2024-03-26T08:50"}).exit_ == 0);
  const auto places = ops_of(slurp(fixture.log()), "place").size();
  const auto canceled = ask_checked(fixture, {"order", "cancel", "005930"});
  INFO(boost::json::serialize(canceled.body_));
  CHECK(canceled.exit_ == 0);
  CHECK(canceled.reason_ == "withdrawal_sent");
  CHECK(canceled.body_.at("withdrawals").as_array().size() == 1);
  CHECK(ops_of(slurp(fixture.log()), "place").size() == places);
  CHECK(ops_of(slurp(fixture.log()), "cancel").size() == 1);
}

TEST_CASE("H6: a cancel reports what it sent and never met; nothing listed is nothing_working") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto empty = ask_checked(fixture, {"order", "cancel", "005930"});
  CHECK(empty.exit_ == 0);
  CHECK(empty.reason_ == "nothing_working");
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    REQUIRE(opened->account_->desk().session().place(order("005930", true, "10", "75000")));
  }
  const auto at = paper::Clock::time_point(std::chrono::duration_cast<paper::Clock::duration>(
    std::chrono::nanoseconds(paper::nanos(*paper::parse_kst("2024-03-26T09:00")))
  ));
  journal(fixture, {
    place_entry("j1", atl::runner::JournalOutcome::PENDING, at),
    place_entry("j1", atl::runner::JournalOutcome::ACCEPTED, at, "1"),
  });
  // Held 0 and working 10 meet a target of 10, which a cancel never reads as met.
  const auto canceled = ask_checked(fixture, {"order", "cancel", "005930"});
  INFO(boost::json::serialize(canceled.body_));
  CHECK(canceled.exit_ == 0);
  CHECK(canceled.reason_ == "withdrawal_sent");
  CHECK(canceled.body_.at("withdrawals").as_array().size() == 1);
  CHECK(std::string(canceled.body_.at("detail").as_string()).find("sent, not confirmed") != std::string::npos);
}

TEST_CASE("two journaled orders on one symbol are the operator's: a cancel sends nothing and answers unknown") {
  const PaperFixture fixture;
  open_session(fixture);
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    REQUIRE(opened->account_->desk().session().place(order("005930", true, "10", "75000")));
    REQUIRE(opened->account_->desk().session().place(order("005930", true, "10", "75000")));
  }
  const auto at = paper::Clock::time_point(std::chrono::duration_cast<paper::Clock::duration>(
    std::chrono::nanoseconds(paper::nanos(*paper::parse_kst("2024-03-26T09:00")))
  ));
  journal(fixture, {
    place_entry("j1", atl::runner::JournalOutcome::PENDING, at),
    place_entry("j1", atl::runner::JournalOutcome::ACCEPTED, at, "1"),
    place_entry("j2", atl::runner::JournalOutcome::PENDING, at),
    place_entry("j2", atl::runner::JournalOutcome::ACCEPTED, at, "2"),
  });
  const auto log = slurp(fixture.log());
  const auto canceled = ask_checked(fixture, {"order", "cancel", "005930"});
  CHECK(canceled.exit_ == 4);
  CHECK(canceled.code_ == "unknown.unaccounted");
  CHECK(ops_of(slurp(fixture.log()), "cancel").size() == ops_of(log, "cancel").size());
}

TEST_CASE("H12: an order the journal does not name refuses plan and place and cancel and and reconcile says so") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto id = plan_id(fixture, "005930", "10", "75000");
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    REQUIRE(opened->account_->desk().session().place(order("005930", true, "5", "75000")));
  }
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"}).code_ == "refused.unjournaled_order");
  // A plan made before the order: the snapshot wall refuses it first, nothing sent either way.
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.stale_plan");
  CHECK(ask_checked(fixture, {"order", "cancel", "005930"}).code_ == "refused.unjournaled_order");
  const auto reconciled = ask_checked(fixture, {"order", "reconcile", "005930"});
  CHECK(reconciled.code_ == "refused.unjournaled_order");
  CHECK(reconciled.body_.at("checked").as_array()[0].as_object().at("state").as_string() == "unjournaled_order");
}

TEST_CASE("review 2's wording: a place whose plan withdraws answers withdrawal_sent and exit 0 and the plan used") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto first = plan_id(fixture, "005930", "10", "75000");
  REQUIRE(ask_checked(fixture, {"order", "place", first}).reason_ == "placed");
  REQUIRE(ask(fixture, {"order", "reconcile", "005930"}).exit_ == 3);
  const auto planned = ask_checked(fixture, {"order", "plan", "005930", "--target", "20", "--limit", "75000"});
  REQUIRE(planned.reason_ == "plan");
  CHECK(planned.body_.at("would").as_string() == "withdraw");
  const auto id = std::string(planned.body_.at("plan_id").as_string());
  const auto placed = ask_checked(fixture, {"order", "place", id});
  INFO(boost::json::serialize(placed.body_));
  CHECK(placed.exit_ == 0);
  CHECK(placed.reason_ == "withdrawal_sent");
  CHECK(placed.body_.at("withdrawals").as_array().size() == 1);
  CHECK(placed.body_.at("next").as_string() == "plan again once cmdatl order list shows them gone");
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.plan_used");
}

TEST_CASE("reconcile: working while an order rests and awaiting and then clean after the fill") {
  const PaperFixture fixture;
  open_session(fixture);
  CHECK(ask_checked(fixture, {"order", "reconcile"}).reason_ == "clean");
  const auto id = plan_id(fixture, "005930", "10", "78000");
  REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
  const auto working = ask_checked(fixture, {"order", "reconcile"});
  INFO(boost::json::serialize(working.body_));
  CHECK(working.exit_ == 3);
  CHECK(working.code_ == "refused.awaiting");
  CHECK(working.body_.at("checked").as_array()[0].as_object().at("state").as_string() == "working");
  // The price reaches 78000 within the session's first third.
  REQUIRE(ask(fixture, {"clock", "advance", "--hours", "2"}).exit_ == 0);
  const auto clean = ask_checked(fixture, {"order", "reconcile"});
  INFO(boost::json::serialize(clean.body_));
  CHECK(clean.exit_ == 0);
  CHECK(clean.reason_ == "clean");
  CHECK(clean.body_.at("owed").as_array().empty());
  // A plan and a place right after the confirming reconcile: the per-call second lets it through.
  const auto next = plan_id(fixture, "005930", "0", "78000");
  CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
}

TEST_CASE("the per-call second: an order command past the data's end is clock.past_data and nothing written") {
  const PaperFixture fixture;
  fixture.arm();
  REQUIRE(ask(fixture, {"clock", "advance", "--to", "2024-03-28T15:30"}).exit_ == 0);
  const auto log = slurp(fixture.log());
  const auto answer = ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "80000"});
  CHECK(answer.exit_ == 1);
  CHECK(answer.code_ == "clock.past_data");
  CHECK(slurp(fixture.log()) == log);
}

TEST_CASE("the market refusing a logged request is unknown.broker_refused and says why and the journal's answer settles it") {
  ConfigParts parts;
  parts.limits_ += "phases = [\"pre_open\", \"open\"]\n";
  const PaperFixture fixture(parts);
  fixture.arm();
  REQUIRE(ask(fixture, {"clock", "advance", "--to", "2024-03-26T08:10"}).exit_ == 0);
  const auto id = plan_id(fixture, "005930", "10", "78500");
  const auto before = slurp(fixture.log());
  const auto refused = ask_checked(fixture, {"order", "place", id});
  INFO(boost::json::serialize(refused.body_));
  CHECK(refused.exit_ == 4);
  CHECK(refused.code_ == "unknown.broker_refused");
  // The market's own reason: 08:10 is before it accepts orders (08:30), though the operator permits pre_open.
  CHECK(
    std::string(refused.body_.at("error").as_object().at("message").as_string()).find("not an order-acceptance time") !=
    std::string::npos
  );
  const auto after = slurp(fixture.log());
  CHECK(ops_of(after, "place").size() == ops_of(before, "place").size() + 1);
  const auto results = ops_of(after, "result");
  REQUIRE(!results.empty());
  CHECK(!results.back().at("error").as_string().empty());
  CHECK(ask_checked(fixture, {"order", "reconcile"}).reason_ == "clean");
  CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().empty());
}

TEST_CASE("B2 and the table: no order option names a ceiling; each catalogued once() reason is declared by the order commands") {
  const cmdatl::Table table(cmdatl::command_list());
  std::vector<std::string> options;
  for (const auto& command : table.commands()) {
    if (command.path_.size() == 2 && command.path_[0] == "order") {
      for (const auto& option : command.options_) {
        if (std::find(options.begin(), options.end(), option.name_) == options.end()) {
          options.push_back(option.name_);
        }
      }
    }
  }
  std::sort(options.begin(), options.end());
  CHECK(options == std::vector<std::string>{"limit", "reason", "target"});
  for (const auto& reason : sending::ONCE_REASONS) {
    if (reason.exit_ == sending::OnceExit::DONE) {
      continue;
    }
    const auto code = cmdatl::once_code(reason.exit_, reason.reason_);
    INFO(code);
    const int exit = static_cast<int>(reason.exit_);
    CHECK(declares({"order", "place"}, exit, code));
    CHECK(declares({"order", "cancel"}, exit, code));
    CHECK(declares({"order", "release"}, exit, code));
  }
  for (const auto* path : {"order plan", "order place", "order cancel", "order reconcile", "order release"}) {
    const auto* command = table.resolve(cmdatl::split(path, ' ')).command_;
    REQUIRE(command != nullptr);
    CHECK(command->exits_ == cmdatl::ORDER_EXITS);
    CHECK(command->available_);
  }
  CHECK(table.resolve({"order", "place"}).command_->may_send_);
  CHECK(table.resolve({"order", "cancel"}).command_->may_send_);
  CHECK(!table.resolve({"order", "release"}).command_->may_send_);
  CHECK(table.resolve({"order", "release"}).command_->mutates_);
  CHECK(!table.resolve({"order", "plan"}).command_->may_send_);
  CHECK(cmdatl::check_table(table).empty());
}

TEST_CASE("--fields and --value accept each new field before the command runs") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto ran = fixture.json({"--value", "plan_id", "order", "plan", "005930", "--target", "1", "--limit", "75000"});
  CHECK(ran.exit_ == 0);
  CHECK(ran.out_.size() > 5);
  const auto fields = fixture.json({"--fields", "now,would,expires_at", "order", "plan", "005930", "--target", "1", "--limit", "75000"});
  CHECK(fields.exit_ == 0);
  CHECK(reply_of(fields.out_).contains("would"));
  CHECK(fixture.json({"--value", "nonsense", "order", "place", "p0123456789"}).exit_ == 2);
  CHECK(fixture.json({"--value", "withdrawals", "order", "cancel", "005930"}).exit_ == 0);
  CHECK(fixture.json({"--value", "checked", "order", "reconcile"}).exit_ == 0);
}

TEST_CASE("hostile inputs: mutated plan books never authorize a send their snapshot does not match", "[hostile]") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto id = plan_id(fixture, "005930", "10", "75000");
  const auto path = fixture.account() / "plans.json";
  const auto good = slurp(path);
  const auto original = cmdatl::PlanBook::parse(good);
  REQUIRE(original);
  std::mt19937 random(20261031);
  const std::string alphabet = "{}[]\":,0123456789abcdefp-. truefalsnul\\\n";
  int placed = 0;
  for (int i = 0; i < 1000; ++i) {
    auto text = good;
    const int edits = 1 + static_cast<int>(random() % 4);
    for (int k = 0; k < edits; ++k) {
      const auto at = random() % (text.size() + 1);
      switch (random() % 3) {
        case 0:
          text.insert(at, 1, alphabet[random() % alphabet.size()]);
          break;
        case 1:
          if (at < text.size()) {
            text.erase(at, 1);
          }
          break;
        default:
          if (at < text.size()) {
            text[at] = alphabet[random() % alphabet.size()];
          }
          break;
      }
    }
    spit(path, text);
    const auto ran = fixture.json({"order", "place", id});
    INFO(text);
    REQUIRE(lines_of(ran.out_).size() == 1);
    const auto body = reply_of(ran.out_);
    CHECK((ran.exit_ == 0 || ran.exit_ == 1 || ran.exit_ == 3 || ran.exit_ == 4));
    if (ran.exit_ == 0 && body.contains("reason") && body.at("reason").as_string() == "placed") {
      ++placed;
      const auto mutated = cmdatl::PlanBook::parse(text);
      REQUIRE(mutated);
      bool same = false;
      for (const auto& entry : *mutated) {
        if (entry.first == id) {
          const auto& plan = entry.second;
          const auto& was = original->front().second;
          same = plan.symbol_ == was.symbol_ && plan.target_ == was.target_ && plan.limit_ == was.limit_ &&
            plan.snapshot_ == was.snapshot_ && !plan.used_;
        }
      }
      CHECK(same);
      // Back to an account with nothing working, for the next mutation.
      REQUIRE(ask(fixture, {"order", "cancel", "005930"}).exit_ == 0);
      REQUIRE(ask(fixture, {"order", "reconcile"}).exit_ == 0);
    }
  }
  CHECK(placed <= 1);
}

TEST_CASE("hostile inputs: mutated approval files never release a set they do not name exactly", "[hostile]") {
  const PaperFixture fixture;
  open_session(fixture);
  const auto at = paper::Clock::time_point(std::chrono::duration_cast<paper::Clock::duration>(
    std::chrono::nanoseconds(paper::nanos(*paper::parse_kst("2024-03-26T09:00")))
  ));
  int serial = 0;
  const auto owe = [&] {
    journal(fixture, {place_entry("j" + std::to_string(++serial), atl::runner::JournalOutcome::PENDING, at)});
  };
  owe();
  std::mt19937 random(31102026);
  const std::string alphabet = "0123456789j 005930\r\n\t:.-~_x";
  int released = 0;
  for (int i = 0; i < 1000; ++i) {
    const auto wanted = "j" + std::to_string(serial);
    auto text = "005930 " + wanted + "\n000660 j0\n";
    const int edits = static_cast<int>(random() % 4);
    for (int k = 0; k < edits; ++k) {
      const auto pos = random() % (text.size() + 1);
      if (random() % 2 == 0) {
        text.insert(pos, 1, alphabet[random() % alphabet.size()]);
      } else if (pos < text.size()) {
        text.erase(pos, 1);
      }
    }
    const auto approval = fixture.dir() / "paper.release";
    spit(approval, text);
    const auto ran = fixture.json({"order", "release", "005930", "--reason", "read"});
    INFO(text);
    REQUIRE(lines_of(ran.out_).size() == 1);
    CHECK((ran.exit_ == 0 || ran.exit_ == 3));
    if (ran.exit_ == 0) {
      ++released;
      const auto parsed = sending::parse_approval(text, "005930", [](std::string_view t) { return paper::valid_symbol(t); });
      CHECK(parsed.refused_.empty());
      CHECK(parsed.ids_ == std::vector<std::string>{wanted});
      owe();
    }
    std::filesystem::remove(approval);
  }
  CHECK(released > 0);
}

TEST_CASE("hostile inputs: generated order command lines end in a declared exit with one JSON object", "[hostile]") {
  const PaperFixture fixture;
  open_session(fixture);
  const std::vector<std::string> words{
    "order", "plan", "place", "cancel", "reconcile", "release", "005930", "000660", "999999", "--target", "--limit",
    "--reason", "10", "0", "-1", "1e3", "75000", "75000.5", "0.0001", "p0123456789", "pXYZ", "", " ", "--json",
    "--fields", "now", "--value", "plan_id", "read", "\xff", "--", "-x", "100000000000",
  };
  std::mt19937 random(7);
  for (int i = 0; i < 2000; ++i) {
    std::vector<std::string> line{"order"};
    const int length = 1 + static_cast<int>(random() % 7);
    for (int k = 0; k < length; ++k) {
      line.push_back(words[random() % words.size()]);
    }
    const auto before = slurp(fixture.log());
    const auto ran = fixture.json(line);
    INFO(cmdatl::join(line));
    REQUIRE(lines_of(ran.out_).size() <= 1);
    CHECK((ran.exit_ == 0 || ran.exit_ == 1 || ran.exit_ == 2 || ran.exit_ == 3 || ran.exit_ == 4));
    if (!ran.out_.empty() && ran.out_.front() == '{') {
      Answer answer;
      answer.exit_ = ran.exit_;
      answer.body_ = reply_of(ran.out_);
      if (const auto* reason = answer.body_.if_contains("reason"); reason != nullptr && reason->is_string()) {
        answer.reason_ = std::string(reason->as_string());
      }
      CHECK(no_send_ok(answer, before, slurp(fixture.log())));
    }
  }
}

TEST_CASE("snapshots: a failed read is written ? and never matches a good read") {
  using Working = decltype(std::declval<paper::PaperDesk::Session&>().working_orders());
  using WorkingValue = std::decay_t<decltype(std::declval<Working&>().value())>;
  struct Failing {
    bool holdings_{true};
    bool orders_{true};
    bool cash_{true};
    atl::Result<atl::HoldingsSnapshot> holdings() {
      if (!holdings_) {
        return atl::Result<atl::HoldingsSnapshot>(paper::refusal("x", "x", false));
      }
      return atl::HoldingsSnapshot{};
    }
    Working working_orders() {
      if (!orders_) {
        return Working(paper::refusal("x", "x", false));
      }
      return Working(WorkingValue{});
    }
    atl::Result<atl::BuyingPower> buying_power(const atl::Currency&) {
      if (!cash_) {
        return atl::Result<atl::BuyingPower>(paper::refusal("x", "x", false));
      }
      return atl::BuyingPower{};
    }
  };
  Failing good;
  const auto ok = cmdatl::snapshot_text(good, SCOPE, "005930", "1", "75000", 10);
  for (int k = 0; k < 4; ++k) {
    Failing bad;
    bad.holdings_ = k != 0;
    bad.orders_ = k != 1;
    bad.cash_ = k != 2;
    const auto text = cmdatl::snapshot_text(bad, SCOPE, "005930", "1", "75000", k == 3 ? std::nullopt : std::optional<std::int64_t>(10));
    INFO(text);
    CHECK(text.find("?") != std::string::npos);
    CHECK(cmdatl::snapshot_of(bad, SCOPE, "005930", "1", "75000", k == 3 ? std::nullopt : std::optional<std::int64_t>(10)) !=
      cmdatl::snapshot_of(good, SCOPE, "005930", "1", "75000", 10));
  }
  CHECK(ok.find("?") == std::string::npos);
}
#endif
