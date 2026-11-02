// The replay module's Parquet store loader, compiled with CMDATL_DUCKDB (this executable's own definition) against
// DuckDB's C library: a store of two fetched versions written here, read as it is, as of an instant, and for a
// coordinate it does not hold.
#include <cmdatl/replay/replay.hpp>

#include <catch2/catch_test_macros.hpp>

#include <duckdb.h>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if !defined(CMDATL_DUCKDB)
#error "test-replay-store is compiled with CMDATL_DUCKDB"
#endif

namespace {

namespace replay = cmdatl::replay;

/**
 * @brief Settings from `key=value` arguments.
 * @param args The arguments.
 * @return The settings.
 */
replay::Settings settings(std::vector<std::string> args) {
  std::vector<char*> argv{const_cast<char*>("run")};
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  return replay::Settings::from_args(static_cast<int>(argv.size()), argv.data());
}

/**
 * @brief Runs a statement.
 * @param con The connection.
 * @param sql The statement.
 * @return DuckDB's error, or empty.
 */
std::string exec(duckdb_connection con, const std::string& sql) {
  duckdb_result result;
  std::string error;
  if (duckdb_query(con, sql.c_str(), &result) == DuckDBError) {
    error = duckdb_result_error(&result);
  }
  duckdb_destroy_result(&result);
  return error;
}

/**
 * @brief Writes a store holding one series, `test/1d/AAA`, in two fetched versions:
 *   fetched 2026-01-01, days 0 to 9 from 2020-01-01, open 100 + day, close open + 1;
 *   fetched 2026-02-01, days 5 to 10, close open + 3 (days 5 to 9 restated, day 10 new).
 *   Read as it is, 11 bars, the last closing at 113; as of 2026-01-15, 10 bars, the last closing at 110.
 * @param root The store's root.
 * @return DuckDB's error, or empty.
 */
std::string make_store(const std::filesystem::path& root) {
  const auto dir = root / "bars" / "source=test" / "interval=1d" / "symbol=AAA";
  std::filesystem::create_directories(dir);
  duckdb_database db;
  duckdb_connection con;
  if (duckdb_open(nullptr, &db) == DuckDBError) {
    return "cannot open DuckDB";
  }
  if (duckdb_connect(db, &con) == DuckDBError) {
    duckdb_close(&db);
    return "cannot connect";
  }
  const auto version =
    [](const std::string& up, const std::string& fetched, const std::string& from, const std::string& to) {
      std::string sql = "SELECT TIMESTAMPTZ '2020-01-01 00:00:00+00' + to_days(d::INTEGER) AS ts, 'close' AS stamp, ";
      sql += "(100 + d)::DECIMAL(18,4) AS open, (103 + d)::DECIMAL(18,4) AS high, (99 + d)::DECIMAL(18,4) AS low, ";
      sql += "(100 + d + " + up + ")::DECIMAL(18,4) AS close, 1000::DECIMAL(24,4) AS volume, ";
      sql += "TIMESTAMPTZ '" + fetched + " 00:00:00+00' AS fetched_at FROM range(" + from + ", " + to + ") t(d)";
      return sql;
    };
  const auto path = (dir / "bars.parquet").generic_string();
  std::string error = exec(con, "SET TimeZone = 'UTC'");
  if (error.empty()) {
    error = exec(
      con,
      "CREATE TABLE bars AS " + version("1", "2026-01-01", "0", "10") + " UNION ALL " +
        version("3", "2026-02-01", "5", "11")
    );
  }
  if (error.empty()) {
    error = exec(con, "COPY (SELECT * FROM bars ORDER BY ts, fetched_at) TO '" + path + "' (FORMAT parquet)");
  }
  duckdb_disconnect(&con);
  duckdb_close(&db);
  return error;
}

/** @brief A store written for one test, removed with its directory. */
class Store {
public:
  /** @brief Writes it under the system's temporary directory. */
  Store() {
    static std::atomic<int> count{0};
    root_ = std::filesystem::temp_directory_path() /
      ("cmdatl-store-" + std::to_string(::getpid()) + "-" + std::to_string(count++));
    std::filesystem::remove_all(root_);
    error_ = make_store(root_);
  }

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  /** @brief Removes it. */
  ~Store() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /** @brief Its root. */
  std::string root() const {
    return root_.string();
  }

  /** @brief DuckDB's error writing it, or empty. */
  const std::string& error() const {
    return error_;
  }

private:
  std::filesystem::path root_;
  std::string error_;
};

} // namespace

TEST_CASE("replay store: the latest version of each bar") {
  const Store store;
  REQUIRE(store.error().empty());
  const auto s = settings({"data.store=test/1d/AAA", "data.store_root=" + store.root()});
  const auto data = replay::load(s, {});
  REQUIRE(data.series_.size() == 1);
  const auto& series = data.series_[0];
  CHECK(series.symbol_ == "AAA");
  CHECK(series.interval_ == algotradelib::BarInterval::ONE_DAY);
  CHECK(series.stamp_ == algotradelib::BarStamp::CLOSE);
  REQUIRE(series.bars_.size() == 11);
  CHECK(replay::iso(series.bars_.back().timestamp_) == "2020-01-11T00:00:00Z");
  CHECK(series.bars_.back().close_ == algotradelib::Decimal64::parse("113"));
  // Day 4 was fetched once, day 5 restated by the later fetch.
  CHECK(series.bars_[4].close_ == algotradelib::Decimal64::parse("105"));
  CHECK(series.bars_[5].close_ == algotradelib::Decimal64::parse("108"));
  CHECK(data.symbols_ == std::vector<std::string>{"AAA"});
  // A store coordinate is read as `store` unless a source is named, and a source starting with kis is Korean.
  CHECK(replay::market_of(s) == "us");
  CHECK(replay::market_of(settings({"data.store=kis/1d/005930"})) == "kr");
}

TEST_CASE("replay store: as of an instant between the two fetches the first version") {
  const Store store;
  REQUIRE(store.error().empty());
  const auto data = replay::load(
    settings({"data.store=test/1d/AAA", "data.store_root=" + store.root(), "data.as_of=2026-01-15T00:00:00Z"}), {}
  );
  REQUIRE(data.series_.size() == 1);
  const auto& bars = data.series_[0].bars_;
  REQUIRE(bars.size() == 10);
  CHECK(replay::iso(bars.back().timestamp_) == "2020-01-10T00:00:00Z");
  CHECK(bars.back().close_ == algotradelib::Decimal64::parse("110"));
  CHECK(bars[5].close_ == algotradelib::Decimal64::parse("106"));
  // replay.from and replay.to cut the bars read.
  const auto cut = replay::load(
    settings(
      {"data.store=test/1d/AAA", "data.store_root=" + store.root(), "replay.from=2020-01-03", "replay.to=20200105"}
    ),
    {}
  );
  CHECK(cut.series_.at(0).bars_.size() == 3);
}

TEST_CASE("replay store: a coordinate the store does not hold and one that is not a coordinate") {
  const Store store;
  REQUIRE(store.error().empty());
  try {
    replay::load(settings({"data.store=test/1d/ZZZ", "data.store_root=" + store.root()}), {});
    FAIL("a missing coordinate was read");
  } catch (const std::invalid_argument& e) {
    CHECK(
      std::string(e.what()) ==
      "the store has no test/1d/ZZZ (" + store.root() + "/bars/source=test/interval=1d/symbol=ZZZ/bars.parquet)"
    );
  }
  try {
    replay::load(settings({"data.store=test/AAA", "data.store_root=" + store.root()}), {});
    FAIL("a coordinate of two parts was read");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "a store coordinate is source/interval/symbol, not 'test/AAA'");
  }
  try {
    replay::load(settings({"data.store=test/1d/AAA", "data.store_root=" + store.root()}), {"BBB"});
    FAIL("a symbol the store series does not hold was read");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "the data has no symbol 'BBB'");
  }
  CHECK_THROWS_AS(replay::load(settings({"data.store=test/1d/AAA"}), {}), std::invalid_argument);
  // An instant DuckDB cannot read is DuckDB's error, never a quietly empty series.
  CHECK_THROWS_AS(
    replay::load(settings({"data.store=test/1d/AAA", "data.store_root=" + store.root(), "data.as_of=yesterday'"}), {}),
    std::runtime_error
  );
}
