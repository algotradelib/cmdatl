#include "./paper-support.hpp"

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
static_assert(!std::is_move_constructible<cmdatl::OwnerLock>::value, "a lock is not moved");

namespace {

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
  CHECK(result.second == "unknown.mock_state");
  CHECK(slurp(fixture.log()) == text);
  CHECK(slurp(fixture.journal()) == journal);
  CHECK(declares({"status"}, 4, "unknown.mock_state"));
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
    REQUIRE(desk.advance_to(kst("2024-03-26T09:00")) == cmdatl::Moved::OK);
    const auto placed = desk.session().place(order("005930", true, "10", "78500"));
    REQUIRE(placed);
    CHECK(placed.value().ref_.primary_ == "1");
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
  for (
    const auto& wrong :
    {"symbol,date,open,high,low,close\n", "symbol,date,open,high,low,close,volume,extra\n",
      "symbol,date,open,high,low,close,close\n", "symbol,date,open,high,low,close,volume,volume\n",
      "symbol,day,open,high,low,close,volume\n", "symbol,date,open,high,low,close,volume,\n", ",\n"}
  ) {
    INFO(wrong);
    const auto problem = paper::parse_bars(std::string(wrong) + "AAA,2024-01-02,10,12,9,11,100\n", data);
    REQUIRE(problem.has_value());
    CHECK(problem->line_ == 1);
    CHECK(problem->message_.find("the seven columns") != std::string::npos);
  }
  const auto bom = paper::parse_bars("\xEF\xBB\xBFsymbol,date,open,high,low,close,volume\n", data);
  REQUIRE(bom.has_value());
  CHECK(bom->message_.find("byte-order mark") != std::string::npos);
  const auto empty = paper::parse_bars("symbol,,date,open,high,low,close,volume\n", data);
  REQUIRE(empty.has_value());
  CHECK(empty->message_.find("an empty column name") != std::string::npos);
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

TEST_CASE("the bars file's columns are read by name, in any order") {
  paper::MarketData documented;
  REQUIRE(!paper::parse_bars(
    "symbol,date,open,high,low,close,volume\nAAA,2024-01-02,10,12,9,11,100\nAAA,2024-01-03,11,13,10,12,200\n"
    "BBB,2024-01-02,5,6,4,5.5,7\n",
    documented
  ));
  paper::MarketData reordered;
  REQUIRE(!paper::parse_bars(
    "volume,close,low,high,open,date,symbol\n100,11,9,12,10,2024-01-02,AAA\n200,12,10,13,11,2024-01-03,AAA\n"
    "7,5.5,4,6,5,2024-01-02,BBB\n",
    reordered
  ));
  REQUIRE(reordered.bars_.size() == documented.bars_.size());
  for (const auto& entry : documented.bars_) {
    const auto& other = reordered.bars_.at(entry.first);
    REQUIRE(other.size() == entry.second.size());
    for (std::size_t i = 0; i < other.size(); ++i) {
      CHECK(other[i].day_ == entry.second[i].day_);
      CHECK(other[i].open_ == entry.second[i].open_);
      CHECK(other[i].high_ == entry.second[i].high_);
      CHECK(other[i].low_ == entry.second[i].low_);
      CHECK(other[i].close_ == entry.second[i].close_);
      CHECK(other[i].volume_ == entry.second[i].volume_);
    }
  }
  // A row is still checked cell by cell, through the header's order.
  const auto problem =
    paper::parse_bars("date,symbol,open,high,low,close,volume\nAAA,2024-01-02,10,12,9,11,100\n", reordered);
  REQUIRE(problem.has_value());
  CHECK(problem->line_ == 2);
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
    CHECK(code_of(ran.out_) == "mock.data");
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
    CHECK(code_of(ran.out_) == "mock.data");
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
    CHECK(code_of(ran.out_) == "mock.data");
  }
  CHECK(declares({"status"}, 1, "mock.data"));
}

TEST_CASE("a placement without the armed file is refused at the boundary and not reached and not recorded") {
  const PaperFixture fixture;
  const auto opened = open_account();
  REQUIRE(opened->account_);
  auto& desk = opened->account_->desk();
  REQUIRE(desk.advance_to(kst("2024-03-26T09:00")) == cmdatl::Moved::OK);
  const auto before = slurp(fixture.log());
  const auto refused = desk.session().place(order("005930", true, "1", "78500"));
  REQUIRE(!refused);
  CHECK(!refused.error().reached_broker_);
  CHECK(refused.error().broker_code_ == "not_armed");
  CHECK(desk.sends() == 0);
  CHECK(slurp(fixture.log()) == before);
}

TEST_CASE("a state file begun on other bars or cash or start is refused and left as it was") {
  ConfigParts parts;
  const PaperFixture fixture(parts);
  const auto base = joined(placed_log(fixture));
  REQUIRE(status_of(fixture).first == 0);
  const std::vector<std::pair<std::string, std::string>> configs{
    {"cash", "bars = \"bars.csv\"\ncash = 100000001\nstart = \"2024-03-25T16:00\"\n"},
    {"start", "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-25T16:01\"\n"},
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

#endif
