// The store writer (`replay/store_write.hpp`), compiled with CMDATL_DUCKDB in the store loader's executable: fetched
// series written as versions, read back by the loader as of each fetch, and the cases it refuses.
#include <cmdatl/replay/fetch.hpp>
#include <cmdatl/replay/store_write.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

namespace atl = algotradelib;
namespace replay = cmdatl::replay;

/** @brief A scratch store root, removed with everything in it. */
class Root {
public:
  /** @brief Makes it. */
  Root() {
    static std::atomic<int> count{0};
    path_ = std::filesystem::temp_directory_path() /
      ("cmdatl-store-write-" + std::to_string(::getpid()) + "-" + std::to_string(count++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;

  /** @brief Removes it. */
  ~Root() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  /** @brief Its path. */
  std::string str() const {
    return path_.string();
  }

  /** @brief Its path. */
  const std::filesystem::path& path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

/**
 * @brief A daily series of `005930` from day `first` (days after 2024-01-01), closes as given.
 * @param first The first day.
 * @param closes The closes.
 * @return The series.
 */
atl::BarSeries series_of(int first, const std::vector<std::string>& closes) {
  atl::BarSeries series;
  series.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, "005930"};
  series.interval_ = atl::BarInterval::ONE_DAY;
  series.stamp_ = atl::BarStamp::OPEN;
  series.adjusted_ = false;
  series.currency_ = atl::Currency("KRW");
  for (std::size_t k = 0; k < closes.size(); ++k) {
    atl::Bar bar;
    bar.timestamp_ =
      std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (19723 + first + static_cast<int>(k)));
    bar.open_ = atl::Decimal64::parse("100");
    bar.high_ = atl::Decimal64::parse("120");
    bar.low_ = atl::Decimal64::parse("90");
    bar.close_ = atl::Decimal64::parse(closes[k]);
    bar.volume_ = atl::Decimal64::parse("1000");
    series.bars_.push_back(bar);
  }
  return series;
}

/**
 * @brief The store's closes of `005930`, as the loader reads them.
 * @param root The store.
 * @param extra Further settings (`bars.as_of`, `replay.from`).
 * @return The closes.
 */
std::vector<std::string> closes(const Root& root, std::vector<std::string> extra = {}) {
  std::vector<std::string> args{"bars.store=vendor/1d/005930", "bars.store_root=" + root.str()};
  args.insert(args.end(), extra.begin(), extra.end());
  std::vector<char*> argv{const_cast<char*>("run")};
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  const auto loaded =
    replay::load_store_source(replay::Settings::from_args(static_cast<int>(argv.size()), argv.data()));
  std::vector<std::string> out;
  for (const auto& bar : loaded.at(0).bars_) {
    out.push_back(bar.close_.str());
  }
  return out;
}

/** @brief 2026-01-01 00:00 UTC, Unix microseconds. */
constexpr std::int64_t T0 = 1767225600LL * 1000000LL;

} // namespace

TEST_CASE("the store writer appends a version only where a bar is new or changed, and the loader reads each fetch") {
  const Root root;
  auto first = replay::store_series(root.str(), "vendor", "1d", series_of(0, {"100", "101", "102"}), T0);
  CHECK(first.rows_ == 3);
  CHECK(first.new_rows_ == 3);
  CHECK(first.path_ == (root.path() / "bars/source=vendor/interval=1d/symbol=005930/bars.parquet").string());
  // The same bars again: nothing new.
  CHECK(replay::store_series(root.str(), "vendor", "1d", series_of(0, {"100", "101", "102"}), T0 + 1).new_rows_ == 0);
  // A revision, a new bar, then the revision undone: every change is a version (100, 110, 100 is three).
  CHECK(replay::store_series(root.str(), "vendor", "1d", series_of(1, {"110", "102", "103"}), T0 + 2).new_rows_ == 2);
  CHECK(replay::store_series(root.str(), "vendor", "1d", series_of(1, {"101"}), T0 + 3).new_rows_ == 1);
  CHECK(closes(root) == std::vector<std::string>{"100", "101", "102", "103"});
  CHECK(
    closes(root, {"bars.as_of=" + replay::utc_instant(T0 + 2)}) == std::vector<std::string>{"100", "110", "102", "103"}
  );
  CHECK(closes(root, {"bars.as_of=" + replay::utc_instant(T0)}) == std::vector<std::string>{"100", "101", "102"});
  // The fetch's snapshot: as of it, from its first bar, an older history in the store left out.
  CHECK(
    closes(root, {"bars.as_of=" + replay::utc_instant(T0 + 3), "replay.from=2024-01-02"}) ==
    std::vector<std::string>{"101", "102", "103"}
  );
}

TEST_CASE(
  "the store writer refuses a version not after the store's latest, a link, a symbol and a price it cannot keep"
) {
  const Root root;
  replay::store_series(root.str(), "vendor", "1d", series_of(0, {"100"}), T0);
  CHECK_THROWS_AS(replay::store_series(root.str(), "vendor", "1d", series_of(0, {"105"}), T0), std::invalid_argument);
  CHECK_THROWS_AS(
    replay::store_series(root.str(), "vendor", "1d", series_of(0, {"105"}), T0 - 1), std::invalid_argument
  );
  CHECK(closes(root) == std::vector<std::string>{"100"});

  auto bad = series_of(0, {"100"});
  for (const auto* symbol : {"../x", "a/b", "a,b", "a=b", "..", ""}) {
    INFO(symbol);
    bad.instrument_.symbol_ = symbol;
    CHECK_THROWS_AS(replay::store_series(root.str(), "vendor", "1d", bad, T0 + 10), std::invalid_argument);
  }
  CHECK(replay::store_symbol("005930.KO"));
  CHECK(replay::store_symbol("^GSPC"));
  CHECK_THROWS_AS(
    replay::store_series(root.str(), "vendor", "1d", series_of(0, {"100.12345"}), T0 + 10), std::invalid_argument
  );

  // A series directory that is a link would write outside the store.
  std::filesystem::create_directories(root.path() / "elsewhere");
  std::filesystem::create_directories(root.path() / "bars/source=vendor/interval=1d");
  std::filesystem::create_directory_symlink(
    root.path() / "elsewhere", root.path() / "bars/source=vendor/interval=1d/symbol=000660"
  );
  auto other = series_of(0, {"100"});
  other.instrument_.symbol_ = "000660";
  CHECK_THROWS_AS(replay::store_series(root.str(), "vendor", "1d", other, T0 + 10), std::invalid_argument);
  CHECK(std::filesystem::is_empty(root.path() / "elsewhere"));
}

TEST_CASE("an adapter's error is shown by its category, status and code, never its text") {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::HTTP;
  error.http_status_ = 401;
  error.broker_code_ = "E401";
  error.message_ = "GET /bars?api_token=SECRET-1 answered 401";
  const auto text = replay::adapter_error_text(error);
  CHECK(text.find("SECRET-1") == std::string::npos);
  CHECK(text.find("http, HTTP 401, code E401") != std::string::npos);
  error.broker_code_ = "token=SECRET-1 and more";
  CHECK(replay::adapter_error_text(error).find("SECRET-1") == std::string::npos);
}
