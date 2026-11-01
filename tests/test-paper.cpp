#include "./paper-support.hpp"

#include <cmdatl/account/desk.hpp>
#include <cmdatl/paper/bars.hpp>
#include <cmdatl/paper/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace support;
namespace paper = cmdatl::paper;
namespace atl = algotradelib;

static_assert(!std::is_copy_constructible<cmdatl::Account>::value, "an account is not copied");
static_assert(!std::is_move_constructible<cmdatl::Account>::value, "an account is not moved");
static_assert(!std::is_copy_assignable<cmdatl::Account>::value, "an account is not assigned");
static_assert(!std::is_move_assignable<cmdatl::Account>::value, "an account is not assigned");
static_assert(!std::is_copy_constructible<paper::PaperDesk>::value, "a desk is not copied");
static_assert(!std::is_move_constructible<paper::PaperDesk>::value, "a desk is not moved");
static_assert(!std::is_move_assignable<paper::PaperDesk>::value, "a desk is not assigned");
static_assert(!std::is_move_constructible<cmdatl::OwnerLock>::value, "a lock is not moved");

namespace cmdatl {
namespace paper {

/** @brief The tests' way to a desk key, to hand a desk a lock it must refuse. */
struct DeskTest {
  /**
   * @brief A key.
   * @return It.
   */
  static DeskKey key() {
    return DeskKey{};
  }
};

} // namespace paper
} // namespace cmdatl

namespace {

/** @brief The paper account the fixture names, in process. */
const paper::PaperAccount ACCOUNT{"paper-0001"};

/**
 * @brief Runs `status` and returns its exit code and error code.
 * @param fixture The account.
 * @return The exit code and the error code (empty on success).
 */
std::pair<int, std::string> status_of(const PaperFixture& fixture) {
  const auto ran = fixture.json({"status"});
  const auto reply = reply_of(ran.out_);
  const auto* error = reply.if_contains("error");
  return {ran.exit_, error == nullptr ? "" : std::string(error->as_object().at("code").as_string())};
}

/**
 * @brief Checks that a paper log is refused as state that cannot be rebuilt, and that nothing was changed.
 * @param fixture The account, already initialized.
 * @param text The log's bytes.
 */
void expect_paper_state(const PaperFixture& fixture, const std::string& text) {
  spit(fixture.log(), text);
  const auto journal = slurp(fixture.journal());
  const auto result = status_of(fixture);
  CHECK(result.first == 4);
  CHECK(result.second == "unknown.paper_state");
  CHECK(slurp(fixture.log()) == text);
  CHECK(slurp(fixture.journal()) == journal);
  CHECK(declares({"status"}, 4, "unknown.paper_state"));
}

/**
 * @brief The lines of a text, each without its newline.
 * @param text The text.
 * @return The lines.
 */
std::vector<std::string> lines(const std::string& text) {
  return lines_of(text);
}

/**
 * @brief Lines joined, each with its newline.
 * @param parts The lines.
 * @return The text.
 */
std::string joined(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) {
    out += part + "\n";
  }
  return out;
}

/**
 * @brief A log with a placement: genesis, an advance to 2024-03-26 09:00, a limit buy of 10 at 78500 and its result.
 * @param fixture The account.
 * @return The log's lines.
 */
std::vector<std::string> placed_log(const PaperFixture& fixture) {
  fixture.arm();
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    auto& desk = opened->account_->desk();
    REQUIRE(desk.advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
    const auto placed = desk.session().place(order("005930", true, "10", "78500"));
    REQUIRE(placed);
    CHECK(placed.value().ack_.order_id_ == "1");
  }
  return lines(slurp(fixture.log()));
}

} // namespace

TEST_CASE("dates and instants are read strictly inside 1990-01-01 to 2100-12-31 KST") {
  CHECK(paper::parse_date("1990-01-01").has_value());
  CHECK(paper::parse_date("2100-12-31").has_value());
  CHECK(!paper::parse_date("1989-12-31"));
  CHECK(!paper::parse_date("2101-01-01"));
  CHECK(!paper::parse_date("2024-02-30"));
  CHECK(paper::parse_date("2024-02-29").has_value());
  CHECK(!paper::parse_date("2023-02-29"));
  CHECK(!paper::parse_date("2024-1-02"));
  CHECK(!paper::parse_date("2024-01-02 "));
  CHECK(!paper::parse_date("9999-12-31"));
  CHECK(paper::parse_kst("2024-03-25T09:30").has_value());
  CHECK(!paper::parse_kst("2024-03-25T24:00"));
  CHECK(!paper::parse_kst("2024-03-25T09:60"));
  CHECK(!paper::parse_kst("2024-03-25 09:30"));
  CHECK(!paper::parse_kst("2024-03-25T09:30:00"));
  CHECK(paper::kst_iso(kst("2024-03-25T09:30")) == "2024-03-25T09:30:00+09:00");
  CHECK(paper::kst_iso(kst("1990-01-01T00:00")) == "1990-01-01T00:00:00+09:00");
  CHECK(paper::kst_iso(kst("2100-12-31T23:59")) == "2100-12-31T23:59:00+09:00");
  // 00:00 KST is 15:00 UTC of the day before: the KST date changes there, not at 00:00 UTC.
  const auto before = kst("2024-03-25T23:59");
  const auto after = kst("2024-03-26T00:00");
  CHECK(paper::kst_day(after) == paper::kst_day(before) + 1);
  CHECK(paper::utc_day(after) == paper::utc_day(before));
  const auto at = kst("2024-03-25T09:30");
  CHECK(paper::from_nanos(paper::nanos(at)) == at);
  CHECK(!paper::from_nanos(paper::nanos(paper::end_instant())));
  CHECK(!paper::from_nanos(paper::nanos(paper::first_instant()) - 1));
  CHECK(!paper::from_nanos(INT64_MAX));
  CHECK(!paper::from_nanos(INT64_MIN));
}

TEST_CASE("the bars file is read strictly, its first problem named by line") {
  const std::string header = "symbol,date,open,high,low,close,volume\n";
  paper::MarketData data;
  CHECK(!paper::parse_bars(header + "AAA,2024-01-02,10,12,9,11,100\n", data));
  CHECK(data.bars_.at("AAA").size() == 1);
  CHECK(!paper::parse_bars(header + "AAA,2024-01-02,10,12,9,11,100\r\nBBB,2024-01-02,1,1,1,1,0", data));
  CHECK(!paper::parse_bars(header + "AAA,2024-01-02,10000000,10000000,1,1.0001,1000000000000\n", data));
  const std::vector<std::pair<std::string, std::size_t>> bad{
    {"symbol,date,open,high,low,close\n", 1},
    {header + "AAA,2024-01-02,10,12,9,11\n", 2},
    {header + "AAA,2024-01-02,10,12,9,11,100,1\n", 2},
    {header + "AAA,2024-01-02,10,12,9,11,100\n\nAAA,2024-01-03,10,12,9,11,100\n", 3},
    {header + "AAA,2024-01-02,10,12,9,11,100\n\n", 3},
    {header + "A A,2024-01-02,10,12,9,11,100\n", 2},
    {header + std::string(33, 'A') + ",2024-01-02,10,12,9,11,100\n", 2},
    {header + "\"AAA\",2024-01-02,10,12,9,11,100\n", 2},
    {header + "AAA,2024-01-02, 10,12,9,11,100\n", 2},
    {header + "AAA,2024-02-30,10,12,9,11,100\n", 2},
    {header + "AAA,1989-12-31,10,12,9,11,100\n", 2},
    {header + "AAA,2101-01-01,10,12,9,11,100\n", 2},
    {header + "AAA,2024-01-02,0.5,12,0.5,11,100\n", 2},
    {header + "AAA,2024-01-02,10.12345,12,9,11,100\n", 2},
    {header + "AAA,2024-01-02,10,10000001,9,11,100\n", 2},
    {header + "AAA,2024-01-02,10,12,11,10,100\n", 2},
    {header + "AAA,2024-01-02,13,12,9,11,100\n", 2},
    {header + "AAA,2024-01-02,10,12,9,11,1000000000001\n", 2},
    {header + "AAA,2024-01-02,10,12,9,11,-1\n", 2},
    {header + "AAA,2024-01-02,1e3,12,9,11,100\n", 2},
    {header + "AAA,2024-01-02,10,12,9,11,100\nAAA,2024-01-02,10,12,9,11,100\n", 3},
    {header + "AAA,2024-01-03,10,12,9,11,100\nAAA,2024-01-02,10,12,9,11,100\n", 3},
    {"", 1},
  };
  for (const auto& entry : bad) {
    INFO(entry.first);
    const auto problem = paper::parse_bars(entry.first, data);
    REQUIRE(problem.has_value());
    CHECK(problem->line_ == entry.second);
  }
  std::string many = header;
  many.reserve(header.size() + 25 * (paper::MAX_BAR_ROWS + 1));
  for (std::size_t i = 0; i <= paper::MAX_BAR_ROWS; ++i) {
    many += "S" + std::to_string(i % 1000) + "," +
      paper::date_text(paper::FIRST_DAY + static_cast<std::int64_t>(i / 1000)) + ",1,1,1,1,0\n";
  }
  const auto problem = paper::parse_bars(many, data);
  REQUIRE(problem.has_value());
  CHECK(problem->line_ == paper::MAX_BAR_ROWS + 2);
}

TEST_CASE("every paper.data case refuses at open and writes nothing") {
  const PaperFixture fixture;
  const std::string header = "symbol,date,open,high,low,close,volume\n";
  const std::vector<std::string> bad{
    header + "005930,2024-03-25,78000,79000,77500,78500\n000660,2024-03-25,1,1,1,1,1\n",
    header +
      "005930,2024-03-25,78000,79000,77500,78500,1\n000660,2024-03-25,1,1,1,1,1\nnot/allowed,2024-03-25,1,1,1,1,1\n",
    header + "005930,2024-03-25,78000,79000,79500,78500,1\n000660,2024-03-25,1,1,1,1,1\n",
    header + "005930,2024-03-25,0.9,1,0.9,1,1\n000660,2024-03-25,1,1,1,1,1\n",
    header + "005930,2024-03-25,1.00001,2,1,1,1\n000660,2024-03-25,1,1,1,1,1\n",
    // A symbol of limits.symbols without rows.
    header + "005930,2024-03-25,78000,79000,77500,78500,1\n",
    // paper.start before the first date's close, and after the last date's close plus a day.
    header + "005930,2024-03-26,78000,79000,77500,78500,1\n000660,2024-03-26,1,1,1,1,1\n",
    header + "005930,2024-03-21,78000,79000,77500,78500,1\n000660,2024-03-21,1,1,1,1,1\n",
  };
  for (const auto& text : bad) {
    INFO(text);
    fixture.write_bars(text);
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "paper.data");
    CHECK(!std::filesystem::exists(fixture.log()));
    CHECK(!std::filesystem::exists(fixture.journal()));
    CHECK(!std::filesystem::exists(fixture.account() / "owner.json"));
  }
  // Too large: a sparse file one byte past the limit is refused by its size, before it is read.
  {
    const int fd = ::open((fixture.dir() / "bars.csv").c_str(), O_WRONLY | O_TRUNC);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(paper::MAX_BARS_BYTES) + 1) == 0);
    ::close(fd);
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "paper.data");
    CHECK(
      reply_of(ran.out_).at("error").as_object().at("message").as_string().find("larger than") != std::string::npos
    );
  }
  // A FIFO with no writer: refused without blocking.
  {
    const Watchdog watchdog("a FIFO for bars", 20);
    std::filesystem::remove(fixture.dir() / "bars.csv");
    REQUIRE(::mkfifo((fixture.dir() / "bars.csv").c_str(), 0600) == 0);
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "paper.data");
  }
  CHECK(declares({"status"}, 1, "paper.data"));
}

TEST_CASE("a fresh desk replays the log to the same cash, holdings, orders and ids") {
  const PaperFixture fixture;
  fixture.arm();
  std::string cash;
  std::string positions;
  std::string orders;
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    auto& desk = opened->account_->desk();
    REQUIRE(desk.advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
    REQUIRE(desk.session().place(order("005930", true, "10", "78500")));
    REQUIRE(desk.session().place(order("000660", true, "2", "150000")));
    REQUIRE(desk.advance_to(kst("2024-03-26T09:05")).status_ == paper::Appended::OK);
    const auto events = desk.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind_ == "filled");
    CHECK(events[0].order_id_ == "1");
    const double value = 10 * *events[0].price_;
    CHECK(events[0].commission_ == std::round(value * 0.00015));
    CHECK(events[0].tax_ == 0);
    CHECK(desk.paper_cash() == static_cast<std::int64_t>(std::floor(100000000 - value - events[0].commission_)));
    // A market sell fills one tick against it, and a sell pays tax.
    REQUIRE(desk.session().place(order("005930", false, "4")));
    REQUIRE(desk.advance_to(kst("2024-03-26T09:10")).status_ == paper::Appended::OK);
    const auto sold = desk.drain_events();
    REQUIRE(sold.size() == 1);
    CHECK(sold[0].side_ == "sell");
    CHECK(sold[0].tax_ == std::round(4 * *sold[0].price_ * 0.002));
    CHECK(sold[0].commission_ == std::round(4 * *sold[0].price_ * 0.00015));
  }
  const auto first = reply_of(fixture.json({"status"}).out_);
  cash = boost::json::serialize(first.at("cash"));
  positions = boost::json::serialize(first.at("positions"));
  orders = boost::json::serialize(first.at("orders"));
  CHECK(first.at("positions").as_array().size() == 1);
  CHECK(first.at("orders").as_array().size() == 1);
  CHECK(first.at("orders").as_array()[0].as_object().at("order_id").as_string() == "2");
  // Reopened again and again: the same state, and the next id continues.
  for (int i = 0; i < 3; ++i) {
    const auto again = reply_of(fixture.json({"status"}).out_);
    CHECK(boost::json::serialize(again.at("cash")) == cash);
    CHECK(boost::json::serialize(again.at("positions")) == positions);
    CHECK(boost::json::serialize(again.at("orders")) == orders);
  }
  const auto opened = open_account();
  REQUIRE(opened->account_);
  const auto placed = opened->account_->desk().session().place(order("005930", true, "1", "79000"));
  REQUIRE(placed);
  CHECK(placed.value().ack_.order_id_ == "4");
}

TEST_CASE("a day order expires at the close, and the market refuses outside 08:30-15:30") {
  const PaperFixture fixture;
  fixture.arm();
  const auto opened = open_account();
  REQUIRE(opened->account_);
  auto& desk = opened->account_->desk();
  // 16:00 on 2024-03-25: after the market's acceptance hours.
  // Logged, with its refusal.
  const auto before = slurp(fixture.log());
  const auto late = desk.session().place(order("005930", true, "1", "78500"));
  REQUIRE(!late);
  CHECK(late.error().reached_broker_);
  CHECK(late.error().message_ == "not an order-acceptance time");
  const auto logged = lines(slurp(fixture.log()));
  REQUIRE(logged.size() == lines(before).size() + 2);
  CHECK(logged.back() == R"({"op":"result","of":"place","id":"","error":"not an order-acceptance time"})");
  // 08:30 on 2024-03-26 accepts; a limit far below the price rests and expires at 15:30.
  REQUIRE(desk.advance_to(kst("2024-03-26T08:30")).status_ == paper::Appended::OK);
  CHECK(desk.accepts_orders_at("005930", desk.now()));
  CHECK(!desk.accepts_orders_at("005930", kst("2024-03-26T08:29")));
  CHECK(!desk.accepts_orders_at("005930", kst("2024-03-26T15:30")));
  CHECK(!desk.accepts_orders_at("000660", kst("2024-03-27T10:00")));
  REQUIRE(desk.session().place(order("005930", true, "1", "60000")));
  REQUIRE(desk.advance_to(kst("2024-03-26T16:00")).status_ == paper::Appended::OK);
  const auto events = desk.drain_events();
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind_ == "expired");
  CHECK(paper::kst_iso(events[0].at_) == "2024-03-26T15:30:00+09:00");
}

TEST_CASE("a placement without the armed file is refused, not reached and not logged") {
  const PaperFixture fixture;
  const auto opened = open_account();
  REQUIRE(opened->account_);
  auto& desk = opened->account_->desk();
  REQUIRE(desk.advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
  const auto before = slurp(fixture.log());
  const auto refused = desk.session().place(order("005930", true, "1", "78500"));
  REQUIRE(!refused);
  CHECK(!refused.error().reached_broker_);
  CHECK(refused.error().broker_code_ == "NOT_ARMED");
  CHECK(slurp(fixture.log()) == before);
}

TEST_CASE("inputs the rules refuse are not logged; refusals the market makes are, and every log replays") {
  const PaperFixture fixture;
  fixture.arm();
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    REQUIRE(opened->account_->desk().advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
  }
  struct Case {
    const char* what_;
    atl::OrderRequest request_;
    bool logged_;
  };
  auto stop = order("005930", true, "1", "78500");
  stop.type_ = atl::OrderType::STOP_LIMIT;
  auto triggered = stop;
  triggered.trigger_price_ = atl::Decimal64::parse("78000");
  auto ioc = order("005930", true, "1", "78500");
  ioc.time_in_force_ = atl::TimeInForce::IMMEDIATE_OR_CANCEL;
  auto routed = order("005930", true, "1", "78500");
  routed.venue_ = atl::Venue::KRX;
  auto priced_market = order("005930", true, "1");
  priced_market.price_ = atl::Decimal64::parse("78500");
  auto unpriced_limit = order("005930", true, "1");
  unpriced_limit.type_ = atl::OrderType::LIMIT;
  const std::vector<Case> cases{
    {"a stop-limit without a trigger", stop, false},
    {"a stop-limit with a trigger", triggered, false},
    {"an IOC order", ioc, false},
    {"a routed order", routed, false},
    {"a market order with a price", priced_market, false},
    {"a limit without a price", unpriced_limit, false},
    {"no bars", order("999999", true, "1", "78500"), false},
    {"quantity 0", order("005930", true, "0", "78500"), false},
    {"a fractional quantity", order("005930", true, "1.5", "78500"), false},
    {"quantity past 10^8", order("005930", true, "100000001", "78500"), false},
    {"a price below 1", order("005930", true, "1", "0.5"), false},
    {"five decimals", order("005930", true, "1", "78500.00001"), false},
    {"a price past 10^7", order("005930", true, "1", "10000001"), false},
    {"quantity 10^8, refused for buying power", order("005930", true, "100000000", "78500"), true},
    {"price 10^7, refused by the band", order("005930", true, "1", "10000000"), true},
    {"a whole quantity written 1.0", order("005930", true, "1.0", "78500"), true},
    {"price 1, refused by the band", order("005930", true, "1", "1"), true},
  };
  for (const auto& c : cases) {
    INFO(c.what_);
    const auto before = slurp(fixture.log());
    {
      const auto opened = open_account();
      REQUIRE(opened->account_);
      // At the client's boundary, so the library's own checks do not stand in for the rules.
      (void)opened->account_->desk().session().client().place(ACCOUNT, c.request_);
    }
    const auto after = slurp(fixture.log());
    CHECK((after != before) == c.logged_);
    const auto status = status_of(fixture);
    CHECK(status.first == 0);
  }
  // An unknown cancel id is a valid input: logged with the market's refusal, and it replays.
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    const auto canceled =
      opened->account_->desk().session().client().cancel(ACCOUNT, paper::PaperAmendment{"99", {}, {}});
    REQUIRE(!canceled);
    CHECK(canceled.error().message_ == "no such order");
    const auto bad_id =
      opened->account_->desk().session().client().cancel(ACCOUNT, paper::PaperAmendment{"x1", {}, {}});
    REQUIRE(!bad_id);
    CHECK(!bad_id.error().reached_broker_);
  }
  const auto logged = lines(slurp(fixture.log()));
  CHECK(logged[logged.size() - 2] == R"({"op":"cancel","at":1711411200000000000,"id":"99"})");
  CHECK(logged.back() == R"({"op":"result","of":"cancel","id":"99","error":"no such order"})");
  CHECK(status_of(fixture).first == 0);
  CHECK(status_of(fixture).first == 0);
}

TEST_CASE("every log the writer could not have written is refused, and nothing is cut or appended") {
  const PaperFixture fixture;
  const auto base = placed_log(fixture);
  REQUIRE(base.size() == 4);
  const auto& genesis = base[0];
  const auto& advance = base[1];
  const auto& place = base[2];
  const auto& result = base[3];
  CHECK(advance == R"({"op":"advance","to":1711411200000000000})");
  CHECK(result == R"({"op":"result","of":"place","id":"1","error":""})");
  const auto to = std::string("1711411200000000000");
  const auto later = std::string("1711411500000000000");
  const auto with = [&](std::vector<std::string> replaced) {
    return joined(replaced);
  };
  const auto replace_first = [](std::string text, const std::string& from, const std::string& to_text) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to_text);
  };
  const std::vector<std::pair<const char*, std::string>> cases{
    {"format 2", with({replace_first(genesis, "\"format\":1", "\"format\":2"), advance, place, result})},
    {"market 2", with({replace_first(genesis, "\"market\":1", "\"market\":2"), advance, place, result})},
    {"no genesis", with({advance, place, result})},
    {"two genesis lines", with({genesis, genesis, advance})},
    {"a reordered key", with({genesis, R"({"to":)" + to + R"(,"op":"advance"})", place, result})},
    {"a repeated key", with({genesis, R"({"op":"advance","to":)" + to + R"(,"to":)" + to + "}", place, result})},
    {"whitespace", with({genesis, R"({"op": "advance","to":)" + to + "}", place, result})},
    {"a blank line", with({genesis, "", advance, place, result})},
    {"an unknown op", with({genesis, R"({"op":"jump","to":)" + to + "}"})},
    {"a missing field", with({genesis, R"({"op":"advance"})"})},
    {"a wrong type", with({genesis, R"({"op":"advance","to":")" + to + "\"}"})},
    {"another number spelling", with({genesis, R"({"op":"advance","to":1.7114112E18})"})},
    {"an extra member", with({genesis, R"({"op":"advance","to":)" + to + R"(,"x":1})"})},
    {"an invalid enum",
      with({genesis, advance, replace_first(place, "\"side\":\"buy\"", "\"side\":\"short\""), result})},
    {"quantity 0", with({genesis, advance, replace_first(place, "\"quantity\":10", "\"quantity\":0"), result})},
    {"a fractional quantity",
      with({genesis, advance, replace_first(place, "\"quantity\":10", "\"quantity\":1.5"), result})},
    {"a price on a market order",
      with({genesis, advance, replace_first(place, "\"type\":\"limit\"", "\"type\":\"market\""), result})},
    {"a limit without its price", with({genesis, advance, replace_first(place, ",\"price\":\"78500\"", ""), result})},
    {"a symbol without bars", with({genesis, advance, replace_first(place, "005930", "999999"), result})},
    {"a placement at another instant", with({genesis, advance, replace_first(place, to, later), result})},
    {"a backwards advance", with({genesis, advance, R"({"op":"advance","to":1711400000000000000})"})},
    {"an advance to the same instant", with({genesis, advance, advance})},
    {"an advance outside the interval", with({genesis, R"({"op":"advance","to":4133894400000000000})"})},
    {"an advance past the data", with({genesis, R"({"op":"advance","to":1711700000000000000})"})},
    {"an input without its result, then another input", with({genesis, advance, place, advance})},
    {"a result the market does not give",
      with({genesis, advance, place, replace_first(result, "\"id\":\"1\"", "\"id\":\"2\"")})},
    {"an orphan result", with({genesis, advance, result})},
    {"a duplicate result", with({genesis, advance, place, result, result})},
    {"a cancel of an id the market never writes",
      with({genesis, advance, R"({"op":"cancel","at":)" + to + R"(,"id":"01"})"})},
    {"a bad prefix and a torn tail", with({genesis, R"({"op":"jump"})"}) + R"({"op":"adv)"},
    {"not JSON", with({genesis, "not json"})},
    {"invalid UTF-8", with({genesis, "{\"op\":\"\xff\"}"})},
  };
  for (const auto& c : cases) {
    INFO(c.first);
    expect_paper_state(fixture, c.second);
  }
  // The same log, valid again, opens.
  spit(fixture.log(), joined(base));
  CHECK(status_of(fixture).first == 0);
}

TEST_CASE("a log begun on other bars, cash, start or lags is refused") {
  ConfigParts parts;
  const PaperFixture fixture(parts);
  const auto base = joined(placed_log(fixture));
  REQUIRE(status_of(fixture).first == 0);
  const std::vector<std::pair<std::string, std::string>> configs{
    {"cash", "bars = \"bars.csv\"\ncash = 100000001\nstart = \"2024-03-25T16:00\"\n"},
    {"start", "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-25T16:01\"\n"},
    {"listing lag", "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-25T16:00\"\nlisting_lag = 1\n"},
    {"holdings lag", "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-25T16:00\"\nholdings_lag = 1\n"},
  };
  for (const auto& entry : configs) {
    INFO(entry.first);
    auto changed = parts;
    changed.paper_ = entry.second;
    fixture.write_config(changed.text());
    expect_paper_state(fixture, base);
  }
  fixture.write_config(parts.text());
  // A bars file of the same size with one price changed.
  auto bars = BARS;
  const auto at = bars.find("78500,1000000");
  REQUIRE(at != std::string::npos);
  bars.replace(at, 5, "78400");
  REQUIRE(bars.size() == BARS.size());
  fixture.write_bars(bars);
  expect_paper_state(fixture, base);
  fixture.write_bars(BARS);
  CHECK(status_of(fixture).first == 0);
}

TEST_CASE("a torn last line is cut after a complete read, and a trailing input is answered first") {
  const PaperFixture fixture;
  const auto base = placed_log(fixture);
  const auto complete = joined(base);
  spit(fixture.log(), complete + R"({"op":"advance","to":17114)");
  CHECK(status_of(fixture).first == 0);
  CHECK(slurp(fixture.log()) == complete);
  // A placement whose result was never written: the next command writes the market's answer, then goes on.
  spit(fixture.log(), joined({base[0], base[1], base[2]}));
  CHECK(status_of(fixture).first == 0);
  CHECK(slurp(fixture.log()) == complete);
  const auto advanced = fixture.json({"clock", "advance", "--minutes", "5"});
  CHECK(advanced.exit_ == 0);
  const auto now = lines(slurp(fixture.log()));
  REQUIRE(now.size() == 5);
  CHECK(now[3] == base[3]);
  CHECK(now[4] == R"({"op":"advance","to":1711411500000000000})");
  const auto events = reply_of(advanced.out_).at("events").as_array();
  REQUIRE(events.size() == 1);
  CHECK(events[0].as_object().at("kind").as_string() == "filled");
}

TEST_CASE("the writer's bytes are the fixture's, so a dependency that changes them fails here") {
  const PaperFixture fixture;
  fixture.arm();
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    auto& desk = opened->account_->desk();
    REQUIRE(desk.advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
    REQUIRE(desk.session().place(order("005930", true, "10", "78500.5")));
    // Refused by the market (nothing held), and logged with its refusal.
    CHECK(!desk.session().place(order("000660", false, "1")));
    (void)desk.session().client().cancel(ACCOUNT, paper::PaperAmendment{"1", {}, {}});
    REQUIRE(desk.advance_to(kst("2024-03-26T09:05")).status_ == paper::Appended::OK);
  }
  const auto fixture_path = std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "paper-log.jsonl";
  if (std::getenv("CMDATL_TEST_RECORD") != nullptr) {
    spit(fixture_path, slurp(fixture.log()));
  }
  const auto expected = slurp(fixture_path);
  REQUIRE(!expected.empty());
  CHECK(slurp(fixture.log()) == expected);
  // And the fixture itself replays.
  CHECK(status_of(fixture).first == 0);
}

TEST_CASE("amounts past 2^53 are refused as paper state, never invented and never undefined") {
  // Holdings of 10^8 shares bought at 1 and sold at 10^7, over and over: each round trip adds about 10^15 to cash.
  std::string bars = "symbol,date,open,high,low,close,volume\n";
  auto day = paper::days_from_civil(2024, 1, 1);
  bars += "AAA," + paper::date_text(day) + ",1,1,1,1,0\n";
  for (int k = 0; k < 12; ++k) {
    bars += "AAA," + paper::date_text(++day) + ",1,10000000,1,10000000,0\n";
    bars += "AAA," + paper::date_text(++day) + ",10000000,10000000,1,1,0\n";
  }
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"AAA\"]\nposition = { \"AAA\" = 100000000 }\norder_value = 10000000000000\ndaily_orders = 3\n";
  parts.paper_ = "bars = \"bars.csv\"\ncash = 10000000000000\nstart = \"2024-01-01T16:00\"\n";
  const PaperFixture fixture(parts, bars);
  fixture.arm();
  bool refused = false;
  for (int k = 0; k < 12 && !refused; ++k) {
    {
      const auto opened = open_account();
      if (!opened->account_) {
        CHECK(code_of_reply(*opened->refusal_) == "unknown.paper_state");
        refused = true;
        break;
      }
      auto& desk = opened->account_->desk();
      const auto buy_day = paper::days_from_civil(2024, 1, 2) + 2 * k;
      REQUIRE(desk.advance_to(paper::open_of(buy_day)).status_ == paper::Appended::OK);
      REQUIRE(desk.session().place(order("AAA", true, "100000000", "1")));
      REQUIRE(desk.advance_to(paper::open_of(buy_day + 1)).status_ == paper::Appended::OK);
      REQUIRE(desk.session().place(order("AAA", false, "100000000", "7000000")));
      REQUIRE(desk.advance_to(paper::open_of(buy_day + 1) + std::chrono::minutes(5)).status_ == paper::Appended::OK);
    }
    const auto status = status_of(fixture);
    if (status.first != 0) {
      CHECK(status.first == 4);
      CHECK(status.second == "unknown.paper_state");
      refused = true;
    }
  }
  CHECK(refused);
}

TEST_CASE("a desk refuses a lock that is not held or is another account's") {
  const PaperFixture fixture;
  REQUIRE(status_of(fixture).first == 0);
  const auto snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.config_);
  cmdatl::OwnerLock unheld(fixture.account().string());
  paper::MarketData data;
  REQUIRE(!paper::parse_bars(BARS, data));
  CHECK_THROWS_AS(paper::PaperDesk(paper::DeskTest::key(), *snapshot.config_, unheld, data), std::logic_error);
  const auto other = fixture.dir() / "state" / "cmdatl" / "accounts" / "paper-paper-other";
  std::filesystem::create_directories(other);
  cmdatl::OwnerLock held(other.string());
  REQUIRE(
    held.acquire(
      std::chrono::milliseconds(100),
      [] {
        return false;
      },
      [](const std::string&) {}
    ) == cmdatl::OwnerLock::Outcome::HELD
  );
  CHECK_THROWS_AS(paper::PaperDesk(paper::DeskTest::key(), *snapshot.config_, held, data), std::logic_error);
}
#endif
