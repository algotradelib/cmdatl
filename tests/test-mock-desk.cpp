#include "./mock-desk-support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
using namespace support;
namespace atl = algotradelib;

static_assert(!std::is_copy_constructible<cmdatl::MockDesk>::value, "a desk is not copied");
static_assert(!std::is_move_constructible<cmdatl::MockDesk>::value, "a desk is not moved");

namespace {

/**
 * @brief Every order the market has, as id and status.
 * @param desk The desk.
 * @return The rows.
 */
std::vector<std::string> history_of(cmdatl::MockDesk& desk) {
  std::vector<std::string> rows;
  const auto history = desk.market_session().order_history(atl::OrderHistoryQuery{});
  REQUIRE(history);
  for (const auto& order : history.value().orders_) {
    rows.push_back(order.order_id_ + ":" + std::to_string(static_cast<int>(order.status_)));
  }
  return rows;
}

/**
 * @brief Every holding, as symbol and quantity.
 * @param desk The desk.
 * @return The rows.
 */
std::vector<std::string> holdings_of(cmdatl::MockDesk& desk) {
  std::vector<std::string> rows;
  const auto holdings = desk.session().holdings();
  REQUIRE(holdings);
  for (const auto& position : holdings.value().positions_) {
    rows.push_back(position.instrument_.symbol_ + ":" + position.quantity_.str());
  }
  return rows;
}

} // namespace

TEST_CASE("the mock desk answers its reads from the bars and the kept market") {
  const PaperFixture fixture;
  fixture.arm();
  const HeldAccount held(fixture);
  auto desk = held.desk();
  CHECK(desk->now() == kst("2024-03-25T16:00"));
  CHECK(desk->paper_cash() == "100000000");
  CHECK(desk->has_symbol("005930"));
  CHECK_FALSE(desk->has_symbol("035420"));
  CHECK(desk->accepts_orders_at("005930", desk->now()));
  CHECK_FALSE(desk->accepts_orders_at("035420", desk->now()));
  CHECK(desk->data_end() == cmdatl::kst_day(kst("2024-03-27T12:00")));
  CHECK(desk->clock_limit() == kst("2024-03-28T15:30"));
  CHECK(desk->next_session_after(desk->now()) == kst("2024-03-26T09:00"));
  CHECK_FALSE(desk->next_session_after(kst("2024-03-27T09:00")));
  CHECK(desk->sends() == 0);
  CHECK_FALSE(desk->order_status("1"));

  REQUIRE(desk->advance_to(kst("2024-03-26T10:00")) == cmdatl::Moved::OK);
  CHECK(desk->drain_events().empty());
  const auto quote = desk->market_session().quote(cmdatl::kr("005930"));
  REQUIRE(quote);
  CHECK(quote.value().last_price_.str() == "78500");
  const auto placed = desk->session().place(order("005930", true, "2", "79000"));
  INFO((placed ? std::string() : placed.error().message_));
  REQUIRE(placed);
  CHECK(placed.value().ref_.primary_ == "1");
  CHECK(desk->sends() == 1);
  CHECK(desk->order_status("1") == std::optional<std::string>("pending"));
  // The 26th's bar began before the order was accepted, so the 27th's fills it, at its limit.
  REQUIRE(desk->advance_to(kst("2024-03-26T16:00")) == cmdatl::Moved::OK);
  CHECK(desk->drain_events().empty());
  REQUIRE(desk->advance_to(kst("2024-03-27T16:00")) == cmdatl::Moved::OK);
  const auto events = desk->drain_events();
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind_ == "filled");
  CHECK(events[0].order_id_ == "1");
  CHECK(events[0].symbol_ == "005930");
  CHECK(events[0].side_ == "buy");
  CHECK(events[0].quantity_ == "2");
  CHECK(events[0].price_ == std::optional<std::string>("79000"));
  CHECK(events[0].commission_ == "0");
  CHECK(events[0].tax_ == "0");
  CHECK(events[0].at_ == kst("2024-03-27T15:30"));
  CHECK(desk->drain_events().empty());
  CHECK(desk->order_status("1") == std::optional<std::string>("filled"));
  CHECK(desk->paper_cash() == "99842000");
  CHECK(holdings_of(*desk) == std::vector<std::string>{"005930:2"});
}

TEST_CASE("an unfilled day order expires with its session and is reported once") {
  const PaperFixture fixture;
  fixture.arm();
  const HeldAccount held(fixture);
  auto desk = held.desk();
  REQUIRE(desk->advance_to(kst("2024-03-26T08:30")) == cmdatl::Moved::OK);
  // Before the open the boundary refuses by phase; the operator's file permits only `open`.
  const auto early = desk->session().place(order("005930", true, "1", "75000"));
  REQUIRE_FALSE(early);
  CHECK(early.error().broker_code_ == "phase");
  REQUIRE(desk->advance_to(kst("2024-03-26T09:30")) == cmdatl::Moved::OK);
  REQUIRE(desk->session().place(order("005930", true, "1", "75000")));
  REQUIRE(desk->advance_to(kst("2024-03-27T16:00")) == cmdatl::Moved::OK);
  const auto events = desk->drain_events();
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind_ == "expired");
  CHECK(events[0].quantity_ == "1");
  CHECK_FALSE(events[0].price_);
  CHECK(desk->order_status("1") == std::optional<std::string>("canceled"));
}

TEST_CASE("a mock desk closed and reopened gives the same ids and holdings and cash") {
  const PaperFixture fixture;
  fixture.arm();
  const HeldAccount held(fixture);
  std::vector<std::string> history;
  std::vector<std::string> holdings;
  std::string cash;
  {
    auto desk = held.desk();
    REQUIRE(desk->advance_to(kst("2024-03-26T10:00")) == cmdatl::Moved::OK);
    REQUIRE(desk->session().place(order("005930", true, "3", "79000")));
    REQUIRE(desk->session().place(order("000660", true, "1", "180000")));
    REQUIRE(desk->session().place(order("005930", true, "1", "78000")));
    const auto ref = desk->session().working_orders().value().orders_.back().ref_;
    atl::AmendInstruction whole;
    whole.all_remaining_ = true;
    REQUIRE(desk->session().cancel(ref, whole));
    // A refused placement takes no id and is not kept.
    REQUIRE_FALSE(desk->session().place(order("005930", false, "1", "79000")));
    REQUIRE(desk->advance_to(kst("2024-03-27T16:00")) == cmdatl::Moved::OK);
    history = history_of(*desk);
    holdings = holdings_of(*desk);
    cash = desk->paper_cash();
    CHECK(history.size() == 3);
    CHECK(desk->sends() == 5);
  }
  auto desk = held.desk();
  CHECK(desk->now() == kst("2024-03-27T16:00"));
  CHECK(history_of(*desk) == history);
  CHECK(holdings_of(*desk) == holdings);
  CHECK(desk->paper_cash() == cash);
  CHECK(desk->sends() == 0);
  CHECK(desk->drain_events().empty());
  // Past the calendar the boundary refuses by phase; the market itself still takes the order, with the next id.
  const auto next = desk->market_session().place(order("005930", true, "1", "80500"));
  REQUIRE(next);
  CHECK(next.value().ref_.primary_ == "4");
}

TEST_CASE("a placement outside the operator's phases is refused before anything is written to the state file") {
  const PaperFixture fixture;
  fixture.arm();
  const HeldAccount held(fixture);
  auto desk = held.desk();
  const auto before = slurp(state_file(fixture));
  const auto refused = desk->session().place(order("005930", true, "1", "78500"));
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "phase");
  CHECK_FALSE(refused.error().reached_broker_);
  CHECK(desk->sends() == 0);
  CHECK(slurp(state_file(fixture)) == before);
}

TEST_CASE("a mock desk refuses a lock that is not held or is another account's, and a lock replaced stops its clock") {
  const PaperFixture fixture;
  const HeldAccount held(fixture);
  cmdatl::OwnerLock unheld(fixture.account().string());
  CHECK_THROWS_AS(
    cmdatl::MockDesk(cmdatl::MockDeskTest::key(), *held.snapshot_.config_, unheld, held.data_), std::logic_error
  );
  auto desk = held.desk();
  const auto before = slurp(state_file(fixture));
  std::filesystem::rename(fixture.account() / "lock", fixture.account() / "lock.old");
  spit(fixture.account() / "lock", "");
  CHECK(desk->advance_to(kst("2024-03-26T10:00")) == cmdatl::Moved::LOCK_LOST);
  CHECK(desk->now() == kst("2024-03-25T16:00"));
  CHECK(slurp(state_file(fixture)) == before);
}
#endif
