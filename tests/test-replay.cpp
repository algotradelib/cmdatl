// The replay module (include/cmdatl/replay/): the settings' rules against hand-written strategies with a generated
// strategy's interface, the loaders' rules on files written here, the replay through `cmdatl::replay::main` with the
// hand-written program of run-strategy.hpp, and a generated sweep of hostile settings and files.
// replay.hpp is included first and alone, so the module is shown to need nothing of cmdatl's or a generated strategy's.
#include <cmdatl/replay/replay.hpp>

#include "./run-strategy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

namespace replay = cmdatl::replay;
namespace atl = algotradelib;

/**
 * @brief Settings from `key=value` arguments, as a driver's command line gives them.
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

/** @brief A directory of its own for a test's files, removed with everything in it. */
class Scratch {
public:
  /** @brief Makes it under the system's temporary directory. */
  Scratch() {
    static std::atomic<int> count{0};
    path_ = std::filesystem::temp_directory_path() /
      ("cmdatl-replay-" + std::to_string(::getpid()) + "-" + std::to_string(count++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  /** @brief Removes it. */
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  /**
   * @brief Writes a file in it.
   * @param name The file's name.
   * @param text The contents.
   * @return Its path.
   */
  std::string write(const std::string& name, const std::string& text) const {
    const auto path = path_ / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return path.string();
  }

  /**
   * @brief A path in it.
   * @param name The name.
   * @return The path.
   */
  std::string at(const std::string& name) const {
    return (path_ / name).string();
  }

private:
  std::filesystem::path path_;
};

/**
 * @brief Reads a whole file.
 * @param path The file.
 * @return Its contents, or empty.
 */
std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

/** @brief What `cmdatl::replay::main` gave. */
struct Replayed {
  /** @brief Its return. */
  int exit_{-1};
  /** @brief What it printed on standard output. */
  std::string out_;
  /** @brief What it printed on standard error. */
  std::string err_;
};

/**
 * @brief Runs `cmdatl::replay::main` with arguments and a fill model's maker, its standard output and error captured.
 * @tparam Strategy The program.
 * @tparam Fills The fill model's maker.
 * @param args The arguments.
 * @param fills Gives the fill model the replay uses.
 * @return What it gave.
 */
template <typename Strategy, typename Fills>
Replayed replay_main(std::vector<std::string> args, Fills fills) {
  std::vector<char*> argv{const_cast<char*>("driver")};
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  std::ostringstream out;
  std::ostringstream err;
  auto* const saved_out = std::cout.rdbuf(out.rdbuf());
  auto* const saved_err = std::cerr.rdbuf(err.rdbuf());
  Replayed replayed;
  replayed.exit_ = replay::main<Strategy>(static_cast<int>(argv.size()), argv.data(), fills);
  std::cout.rdbuf(saved_out);
  std::cerr.rdbuf(saved_err);
  replayed.out_ = out.str();
  replayed.err_ = err.str();
  return replayed;
}

/**
 * @brief Runs `cmdatl::replay::main` with arguments, its standard output and error captured.
 * @tparam Strategy The program.
 * @param args The arguments.
 * @return What it gave.
 */
template <typename Strategy>
Replayed replay_main(std::vector<std::string> args) {
  return replay_main<Strategy>(std::move(args), [](atl::backtest::NextBarFillModel model) {
    return model;
  });
}

/**
 * @brief A field of a report, as written (a string keeps its quotes).
 * @param json The report.
 * @param name The field.
 * @return Its text, or empty when it is not there.
 */
std::string field(const std::string& json, const std::string& name) {
  const auto key = "\"" + name + "\": ";
  const auto at = json.find(key);
  if (at == std::string::npos) {
    return {};
  }
  const auto from = at + key.size();
  return json.substr(from, json.find_first_of(",\n", from) - from);
}

/**
 * @brief An OHLCV CSV with Unix times, one bar a day from 2020-01-01, each bar's four prices one value.
 * @param prices Each day's price.
 * @return The file's text.
 */
std::string flat_bars(const std::vector<int>& prices) {
  std::string text = "ts,open,high,low,close,volume\n";
  for (std::size_t day = 0; day < prices.size(); ++day) {
    const auto p = std::to_string(prices[day]);
    text += std::to_string(1577836800 + 86400 * static_cast<long long>(day)) + "," + p + "," + p + "," + p + "," + p +
      ",1000\n";
  }
  return text;
}

/** @brief Clears the hand-written program's script before a test, and after it. */
struct FreshScript {
  FreshScript() {
    runtest::script() = runtest::Script{};
  }
  FreshScript(const FreshScript&) = delete;
  FreshScript& operator=(const FreshScript&) = delete;
  ~FreshScript() {
    runtest::script() = runtest::Script{};
  }
};

struct Named {
  static constexpr const char* atls_mode = "named";
  static std::vector<std::string> atls_instruments() {
    return {"long", "short"};
  }
};

struct Single {
  static constexpr const char* atls_mode = "single";
  static std::vector<std::string> atls_instruments() {
    return {};
  }
};

struct Universe {
  static constexpr const char* atls_mode = "universe";
  static std::vector<std::string> atls_instruments() {
    return {};
  }
};

// Two parameters with a generated strategy's `set`: the value is stored, then every bound is checked against the
// values so far (`exit` at most `entry`, both in [0, 100]); every call is recorded, refused or not.
struct Bounded {
  double entry_ = 50;
  double exit_ = 10;
  std::vector<std::pair<std::string, double>> calls_;
  std::vector<std::pair<atl::OrderType, std::string>> pricing_;
  bool refuse_pricing_ = false;

  void set(std::string_view name, double value) {
    calls_.emplace_back(std::string(name), value);
    if (name == "entry") {
      entry_ = value;
    } else if (name == "exit") {
      exit_ = value;
    } else {
      throw std::invalid_argument("the program has no parameter '" + std::string(name) + "'");
    }
    if (entry_ < 0 || entry_ > 100 || exit_ < 0 || exit_ > entry_) {
      throw std::invalid_argument("a bound is broken");
    }
  }

  void set_order_pricing(atl::OrderType type, const atl::Decimal64& offset) {
    if (refuse_pricing_) {
      throw std::invalid_argument("the pricing is refused");
    }
    pricing_.emplace_back(type, offset.str());
  }
};

} // namespace

TEST_CASE("replay settings: key=value read in order with later values winning") {
  const auto s = settings({"a=1", "b=x=y", "a=2", "c="});
  CHECK(s.get("a") == "2");
  CHECK(s.get("b") == "x=y");
  CHECK(s.get("c") == "");
  CHECK_FALSE(s.get("d").has_value());
  CHECK_THROWS_AS(settings({"novalue"}), std::invalid_argument);
}

TEST_CASE("replay settings: a named program takes each declared instrument in declaration order") {
  CHECK(
    replay::symbols_of<Named>(settings({"instrument.short=B", "instrument.long=A", "instrument.extra=C"})) ==
    std::vector<std::string>{"A", "B"}
  );
  CHECK(
    replay::symbols_of<Named>(settings({"instrument.long=A", "instrument.short=B", "instrument.long=Z"})) ==
    std::vector<std::string>{"Z", "B"}
  );
  try {
    replay::symbols_of<Named>(settings({"instrument.long=A"}));
    FAIL("a missing instrument was accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "the run gives no symbol for instrument 'short' (instruments.short = \"...\")");
  }
}

TEST_CASE("replay settings: a single program takes at most one instrument and none means the data's") {
  CHECK(replay::symbols_of<Single>(settings({})).empty());
  CHECK(replay::symbols_of<Single>(settings({"instrument.x=A"})) == std::vector<std::string>{"A"});
  CHECK(replay::symbols_of<Single>(settings({"instrument.x=A", "instrument.x=B"})) == std::vector<std::string>{"B"});
  try {
    replay::symbols_of<Single>(settings({"instrument.x=A", "instrument.y=B"}));
    FAIL("two instruments were accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "a single-instrument program takes one entry in [instruments]");
  }
}

TEST_CASE("replay settings: a universe takes every instrument in order of first appearance") {
  CHECK(
    replay::symbols_of<Universe>(settings({"instrument.b=B", "instrument.a=A", "instrument.b=C"})) ==
    std::vector<std::string>{"C", "A"}
  );
  CHECK(replay::symbols_of<Universe>(settings({"other=1"})).empty());
}

TEST_CASE("replay settings: parameters are set twice in argument order and the second pass checks the final values") {
  // Raising both: exit=80 alone breaks `exit <= entry` (entry is 50) until entry=90 is set.
  Bounded strategy;
  replay::set_params(strategy, settings({"param.exit=80", "param.entry=90"}));
  CHECK(strategy.entry_ == 90);
  CHECK(strategy.exit_ == 80);
  const std::vector<std::pair<std::string, double>> expected{{"exit", 80}, {"entry", 90}, {"exit", 80}, {"entry", 90}};
  CHECK(strategy.calls_ == expected);
}

TEST_CASE("replay settings: a duplicate parameter keeps its last value at its first position") {
  Bounded strategy;
  replay::set_params(strategy, settings({"param.entry=60", "param.exit=5", "param.entry=70"}));
  const std::vector<std::pair<std::string, double>> expected{{"entry", 70}, {"exit", 5}, {"entry", 70}, {"exit", 5}};
  CHECK(strategy.calls_ == expected);
}

TEST_CASE("replay settings: a bound still broken after both passes or an unknown name is refused by the second pass") {
  Bounded broken;
  CHECK_THROWS_AS(replay::set_params(broken, settings({"param.entry=200"})), std::invalid_argument);
  CHECK(broken.calls_.size() == 2);
  Bounded unknown;
  try {
    replay::set_params(unknown, settings({"param.nope=1"}));
    FAIL("an unknown parameter was accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "the program has no parameter 'nope'");
  }
  CHECK(unknown.calls_.size() == 2);
}

TEST_CASE("replay settings: a value that does not parse is refused before any parameter is set") {
  Bounded junk;
  try {
    replay::set_params(junk, settings({"param.entry=60", "param.exit=1x"}));
    FAIL("a value with trailing text was accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "param.exit: '1x' is not a number");
  }
  CHECK(junk.calls_.empty());
  // std::stod's own exceptions, as before: not a number at all, and too large for a double.
  Bounded empty;
  CHECK_THROWS_AS(replay::set_params(empty, settings({"param.entry=60", "param.exit="})), std::invalid_argument);
  CHECK(empty.calls_.empty());
  Bounded huge;
  CHECK_THROWS_AS(replay::set_params(huge, settings({"param.entry=1e999"})), std::out_of_range);
  CHECK(huge.calls_.empty());
}

TEST_CASE("replay settings: the pricing defaults to market at zero and is set as given") {
  Bounded plain;
  replay::set_pricing(plain, settings({}));
  REQUIRE(plain.pricing_.size() == 1);
  CHECK(plain.pricing_[0].first == atl::OrderType::MARKET);
  CHECK(plain.pricing_[0].second == atl::Decimal64::parse("0").str());
  Bounded limit;
  replay::set_pricing(limit, settings({"orders.type=limit", "orders.offset=0.01"}));
  REQUIRE(limit.pricing_.size() == 1);
  CHECK(limit.pricing_[0].first == atl::OrderType::LIMIT);
  CHECK(limit.pricing_[0].second == atl::Decimal64::parse("0.01").str());
}

TEST_CASE("replay settings: a pricing that does not parse or that the strategy refuses is an error") {
  Bounded type;
  try {
    replay::set_pricing(type, settings({"orders.type=stop"}));
    FAIL("an unknown order type was accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "orders.type is market or limit, not 'stop'");
  }
  Bounded offset;
  try {
    replay::set_pricing(offset, settings({"orders.type=limit", "orders.offset=abc"}));
    FAIL("a bad offset was accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).rfind("orders.offset: 'abc' is not a decimal", 0) == 0);
  }
  Bounded refused;
  refused.refuse_pricing_ = true;
  try {
    replay::set_pricing(refused, settings({}));
    FAIL("a refused pricing was accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == "the pricing is refused");
  }
}

TEST_CASE("replay decimals: plain decimals read exactly and an offset of 19 digits read whole") {
  // The run spec writes a number given with an exponent out as a plain decimal; the replay reads only that.
  CHECK(replay::decimal_of("100000", "x") == atl::Decimal64::parse("100000"));
  CHECK(replay::decimal_of("0.00015", "x").str() == "0.00015");
  CHECK_THROWS_AS(replay::decimal_of("1e5", "replay.cash"), std::invalid_argument);
  // Eighteen fractional digits, nineteen with the leading zero: read exactly, never through a binary number.
  CHECK(replay::decimal_of("0.299999999999999999", "orders.offset").str() == "0.299999999999999999");
  try {
    replay::decimal_of("abc", "f.csv:3");
    FAIL("a word was read as a decimal");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).rfind("f.csv:3: 'abc' is not a decimal (", 0) == 0);
  }
  Scratch dir;
  const auto file = dir.write("bars.csv", flat_bars({1}));
  const auto s = settings(
    {"data.source=csv", "data.file=" + file, "data.symbol=AAA", "replay.cash=100000", "replay.commission=0.00015"}
  );
  const auto data = replay::load(s, {});
  CHECK(replay::replay_config(s, data).cash_ == atl::Decimal64::parse("100000"));
}

TEST_CASE("replay CSV: times are read as written or refused and never read differently") {
  Scratch dir;
  const std::string long_header = "ts,open,high,low,close,volume\n";
  const auto load = [&dir](const std::string& csv, std::vector<std::string> args) {
    args.push_back("data.source=csv");
    args.push_back("data.file=" + dir.write("bars.csv", csv));
    return replay::load(settings(args), {});
  };
  const std::vector<std::string> iso{"data.symbol=AAA", "data.time_format=iso"};
  {
    const auto data =
      load(long_header + "2020-01-01T09:00:00+09:00,1,1,1,1,1\n2020-01-02T09:00+09:00,1,1,1,1,1\n", iso);
    REQUIRE(data.series_.size() == 1);
    REQUIRE(data.series_[0].bars_.size() == 2);
    CHECK(replay::iso(data.series_[0].bars_[0].timestamp_) == "2020-01-01T00:00:00Z");
    CHECK(replay::iso(data.series_[0].bars_[1].timestamp_) == "2020-01-02T00:00:00Z");
  }
  // A year past what a nanosecond clock holds: read on this platform, or refused, never wrapped.
  try {
    const auto data = load(long_header + "2500-01-01,1,1,1,1,1\n", iso);
    CHECK(replay::iso(data.series_.at(0).bars_.at(0).timestamp_) == "2500-01-01T00:00:00Z");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).find("this platform's clock") != std::string::npos);
  }
  const std::vector<std::string> symbol{"data.symbol=AAA"};
  const std::vector<std::string> wide{"data.layout=wide"};
  const std::vector<std::tuple<std::string, std::vector<std::string>, std::string, std::string>> refused{
    {"no-day", iso, long_header + "2020-02-30,1,1,1,1,1\n", "bars.csv:2: not a date"},
    {"year", iso, long_header + "999999-01-01,1,1,1,1,1\n", "bars.csv:2: not a date"},
    {"junk", iso, long_header + "2020-01-01junk,1,1,1,1,1\n", "bars.csv:2: not an ISO time"},
    {"hour", iso, long_header + "2020-01-01T24:00,1,1,1,1,1\n", "bars.csv:2: not an ISO time"},
    {"far", symbol, long_header + "9223372036854775807,1,1,1,1,1\n", "outside the years 1 to 9999"},
    {"short", wide, "ts,AAA,stamp\n1577836800,100\n", "bars.csv:2: expected 3 cells, found 2"},
    {"stamps", symbol, "ts,stamp,open,high,low,close,volume\n1577836800,close,1,1,1,1,1\n1577923200,open,1,1,1,1,1\n",
      "bars.csv:3: the stamp of AAA changes; a symbol's bars share one stamp"},
    {"spelling", symbol, "ts,stamp,open,high,low,close,volume\n1577836800,clsoe,1,1,1,1,1\n",
      "bars.csv:2: a stamp is 'open' or 'close', not 'clsoe'"},
    {"order", symbol, long_header + "1577923200,1,1,1,1,1\n1577836800,1,1,1,1,1\n",
      "bars.csv:3: times must increase within a symbol"},
    {"price", symbol, long_header + "1577836800,1,1,0,1,1\n", "bars.csv:2: prices must be positive, got 0"},
  };
  for (const auto& [name, args, csv, says] : refused) {
    INFO(name);
    try {
      load(csv, args);
      FAIL("refused data was read");
    } catch (const std::invalid_argument& e) {
      CHECK(std::string(e.what()).find(says) != std::string::npos);
    }
  }
}

TEST_CASE("replay wide tables: dates must increase and the fill reads them") {
  Scratch dir;
  const auto s =
    settings({"data.source=wide", "data.file=" + dir.write("close.csv", "date,A\n20200102,1\n20200101,2\n")});
  try {
    replay::load(s, {});
    FAIL("decreasing dates were read");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()) == dir.at("close.csv") + ":3: dates must increase");
  }
  const auto ok =
    settings({"data.file=" + dir.write("ok.csv", "date,A,B\n20200101,1,\n20200102,2,NaN\n20200103,4,5\n")});
  const auto data = replay::load(ok, {});
  CHECK(data.symbols_ == std::vector<std::string>{"A", "B"});
  // A row is a bar with open = high = low = close; a missing cell is no bar.
  const auto feed = replay::events(data, "us", false);
  REQUIRE(feed.size() == 4);
  CHECK(feed[1].bar_.open_->str() == "2");
  CHECK(feed[1].bar_.close_.str() == "2");
  // An open point needs the opens, highs and lows.
  CHECK_THROWS_AS(replay::events(data, "us", true), std::invalid_argument);
  auto full = data;
  full.open_ = full.close_;
  full.high_ = full.close_;
  full.low_ = full.close_;
  // Each day as a point at the open (09:30 at UTC-5, 14:30 UTC) and the day's bar at the close (21:00 UTC): every
  // instrument's point before any day's bar.
  const auto split = replay::events(full, "us", true);
  REQUIRE(split.size() == 8);
  CHECK(split[0].bar_.timestamp_ == replay::day_of(20200101) + std::chrono::hours(14) + std::chrono::minutes(30));
  CHECK(split[0].interval_ == algotradelib::BarInterval::ONE_DAY);
  CHECK(split[0].stamp_ == algotradelib::BarStamp::CLOSE);
  CHECK(split[1].bar_.timestamp_ == replay::day_of(20200101) + std::chrono::hours(21));
  CHECK(split[4].bar_.timestamp_ == replay::day_of(20200103) + std::chrono::hours(14) + std::chrono::minutes(30));
  CHECK(split[5].bar_.timestamp_ == split[4].bar_.timestamp_);
  CHECK(split[6].bar_.timestamp_ == replay::day_of(20200103) + std::chrono::hours(21));
}

TEST_CASE("replay main: shorts go through only with replay.shorts true") {
  const FreshScript fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", flat_bars({100, 100, 100, 100, 100}));
  runtest::script().decisions_[1] = runtest::Decision{-1, "1", true};
  const std::vector<std::string> base{"data.source=csv", "data.file=" + file, "data.symbol=AAA", "instrument.x=AAA"};
  auto shorts = base;
  shorts.push_back("replay.shorts=true");
  shorts.push_back("output.fills=" + dir.at("fills.csv"));
  const auto sold = replay_main<runtest::Program>(shorts);
  INFO(sold.err_);
  REQUIRE(sold.exit_ == 0);
  CHECK(field(sold.out_, "executions") == "1");
  CHECK(field(sold.out_, "refused") == "0");
  CHECK(
    slurp(dir.at("fills.csv")) ==
    "at,symbol,side,quantity,price,commission,tax\n2020-01-02T00:00:00Z,AAA,sell,1,100,0,0\n"
  );
  auto long_only = base;
  long_only.push_back("replay.shorts=false");
  const auto refused = replay_main<runtest::Program>(long_only);
  REQUIRE(refused.exit_ == 0);
  CHECK(field(refused.out_, "executions") == "0");
  CHECK(field(refused.out_, "refused") != "0");
  CHECK(refused.out_.find("\"guard:") != std::string::npos);
}

TEST_CASE("replay main: a market-price rule prices market orders and the report names it") {
  const FreshScript fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", flat_bars({100, 110, 120}));
  runtest::script().decisions_[2] = runtest::Decision{1, "1", true};
  const std::vector<std::string> args{
    "data.source=csv", "data.file=" + file, "data.symbol=AAA", "instrument.x=AAA", "output.fills=" + dir.at("f.csv")
  };
  const auto plain = replay_main<runtest::Program>(args);
  REQUIRE(plain.exit_ == 0);
  CHECK(slurp(dir.at("f.csv")).find("AAA,buy,1,120,") != std::string::npos);
  CHECK(plain.out_.find("market_price_rule") == std::string::npos);
  const auto ruled = replay_main<runtest::Program>(args, [](atl::backtest::NextBarFillModel model) {
    return model.with_market_price("decision_close", [](const atl::backtest::MarketPriceInput& in) {
      return in.decision_price_ ? in.decision_price_ : in.next_.bar_.open_;
    });
  });
  INFO(ruled.err_);
  REQUIRE(ruled.exit_ == 0);
  // Decided on the second bar, filled against the third at the second's close.
  CHECK(slurp(dir.at("f.csv")).find("AAA,buy,1,110,") != std::string::npos);
  CHECK(
    ruled.out_.find(", \"market_price_rule\": \"decision_close\", \"market_fills_by_rule\": 1}") != std::string::npos
  );
}

TEST_CASE("replay main: a limit never reached expires with its session and the last is still queued") {
  const FreshScript fresh;
  Scratch dir;
  // Prices that gap up every day: each day's buy limit at its close is never reached.
  const auto file = dir.write("gaps.csv", flat_bars({100, 200, 300, 400}));
  for (std::size_t bar = 1; bar <= 4; ++bar) {
    runtest::script().decisions_[bar] = runtest::Decision{1, "1", false};
  }
  const auto gaps = replay_main<runtest::Program>(
    {"data.source=csv", "data.file=" + file, "data.symbol=AAA", "instrument.x=AAA", "orders.type=limit"}
  );
  INFO(gaps.err_);
  REQUIRE(gaps.exit_ == 0);
  CHECK(field(gaps.out_, "executions") == "0");
  CHECK(
    gaps.out_.find(
      "  \"caveats\": {\"gaps\": 0, \"unfilled\": 1, \"expired\": 3, \"unvalued_bars\": 0, \"multiple_instruments\": false"
    ) != std::string::npos
  );
  REQUIRE(runtest::script().pricing_.size() == 1);
  CHECK(runtest::script().pricing_[0].first == atl::OrderType::LIMIT);
}

TEST_CASE("replay main: parameter bounds are checked against the final values and a broken one stops the replay") {
  const FreshScript fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", flat_bars({100, 101, 102}));
  const std::vector<std::string> base{"data.source=csv", "data.file=" + file, "data.symbol=AAA", "instrument.x=AAA"};
  auto raised = base;
  raised.push_back("param.exit=80");
  raised.push_back("param.entry=90");
  const auto ok = replay_main<runtest::Program>(raised);
  INFO(ok.err_);
  CHECK(ok.exit_ == 0);
  CHECK(ok.err_.empty());
  for (
    const auto& [param, says] : std::vector<std::pair<std::string, std::string>>{
      {"param.entry=200", "error: entry is outside [0, 100]\n"},
      {"param.exit=60", "error: exit is outside [0, entry]\n"},
      {"param.nope=1", "error: the program has no parameter 'nope'\n"},
      {"param.entry=1x", "error: param.entry: '1x' is not a number\n"},
    }
  ) {
    auto args = base;
    args.push_back(param);
    const auto refused = replay_main<runtest::Program>(args);
    CHECK(refused.exit_ == 1);
    CHECK(refused.out_.empty());
    CHECK(refused.err_ == says);
  }
}

TEST_CASE("replay main: the pricing refusals by reason and the refusals before any replay") {
  const FreshScript fresh;
  Scratch dir;
  const auto file = dir.write("bars.csv", flat_bars({100, 101, 102}));
  const std::vector<std::string> base{"data.source=csv", "data.file=" + file, "data.symbol=AAA", "instrument.x=AAA"};
  runtest::script().refusals_.price_not_representable_ = 4;
  runtest::script().refusals_.no_basis_price_ = 1;
  const auto counted = replay_main<runtest::Program>(base);
  REQUIRE(counted.exit_ == 0);
  CHECK(counted.out_.find("  \"pricing_refused\": 5,\n") != std::string::npos);
  CHECK(
    counted.out_.find("  \"pricing_refusal_reasons\": {\"no_basis_price\": 1, \"price_not_representable\": 4},\n") !=
    std::string::npos
  );
  runtest::script().refusals_ = runtest::Refusals{};
  const auto none = replay_main<runtest::Program>(base);
  CHECK(none.out_.find("  \"pricing_refused\": 0,\n  \"pricing_refusal_reasons\": {},\n") != std::string::npos);
  const auto with = [&base](std::vector<std::string> more) {
    auto args = base;
    args.insert(args.end(), more.begin(), more.end());
    return args;
  };
  const std::vector<std::pair<std::vector<std::string>, std::string>> refused{
    {{"orders.type=market", "orders.offset=0.001"}, "error: an offset prices a limit\n"},
    {{"data.open_point=true"}, "error: data.open_point is for a program with on_open/on_close\n"},
    {{"data.source=store", "data.store=test/1d/AAA"},
      "error: this binary was built without DuckDB (cmdatl builds with it when a spec reads a store)\n"},
    {{"output.fills=" + dir.at("no/such/dir/fills.csv")},
      "error: cannot write " + dir.at("no/such/dir/fills.csv") + "\n"},
    {{"noequals"}, "error: expected key=value, got 'noequals'\n"},
  };
  for (const auto& [more, says] : refused) {
    const auto ran = replay_main<runtest::Program>(with(more));
    CHECK(ran.exit_ == 1);
    CHECK(ran.err_ == says);
  }
  const auto session = replay_main<runtest::SessionProgram>(base);
  CHECK(session.exit_ == 1);
  CHECK(
    session.err_ ==
    "error: the program has on_open/on_close and a daily bar is a whole session: set data.open_point = true, or give "
    "intraday bars\n"
  );
  // With open points but no session edge met, the replay fails instead of reporting a run that traded on nothing.
  auto open_points = base;
  open_points.push_back("data.open_point=true");
  const auto missed = replay_main<runtest::SessionProgram>(open_points);
  CHECK(missed.exit_ == 1);
  CHECK(missed.err_.find("on_open never ran") != std::string::npos);
}

TEST_CASE("replay sweep: generated settings and files are read or refused with an exception and nothing else") {
  const FreshScript fresh;
  Scratch dir;
  std::mt19937_64 random(20261102);
  const auto pick = [&random](const std::vector<std::string>& from) {
    return from[std::uniform_int_distribution<std::size_t>(0, from.size() - 1)(random)];
  };
  const auto chance = [&random](int percent) {
    return std::uniform_int_distribution<int>(0, 99)(random) < percent;
  };
  const std::vector<std::string> columns{
    "ts", "date", "open", "high", "low", "close", "volume", "stamp", "symbol", "AAA", "BBB", "", " ", "close\r"
  };
  const std::vector<std::string> cells{
    "1", "-1", "0", "100", "100.5", "1e5", "1e400", "1e-400", "-0", "nan", "NaN", "", " ", "9223372036854775807",
    "-9223372036854775808", "99999999999999999999", "1577836800", "1577836800000", "-62135596801", "253402300800",
    "2020-01-01", "2020-02-30", "2020-01-01T09:00:00+09:00", "2020-01-01T23:59:59Z", "2020-01-01T09:00-14:59",
    "2020-01-01 09:00", "20200101", "00000000", "99991231", "open", "close", "clsoe", "AAA", "BBB", "\"", "\x01",
    "\xff\xfe", "0.299999999999999999", "123456789012345678901234567890"
  };
  const std::vector<std::pair<std::string, std::vector<std::string>>> keys{
    {"data.source", {"csv", "wide", "store", "junk"}},
    {"data.layout", {"long", "wide", "x"}},
    {"data.time", {"ts", "date", "nope"}},
    {"data.time_format", {"unix", "unix_ms", "yyyymmdd", "iso", "x"}},
    {"data.timezone", {"UTC", "Z", "+09:00", "-05:30", "+9:00", "", "+-1:00", "+99:99", "x"}},
    {"data.interval", {"1m", "5m", "1h", "1d", "1w", "1mo", "2d"}},
    {"data.stamp", {"open", "close", "stamp", "date", "nope"}},
    {"data.symbol", {"AAA", "BBB", ""}},
    {"data.symbols", {"AAA", "AAA,BBB", ""}},
    {"data.symbol_column", {"symbol", "nope"}},
    {"data.columns.close", {"close", "AAA", "nope"}},
    {"replay.from", {"2020-01-01", "20200102", "2020-13-01", "x", "00010101"}},
    {"replay.to", {"2020-01-03", "99991231", "2019-12-31", ""}},
    {"replay.fill", {"next-open", "close", "legs", "x"}},
    {"data.open_point", {"true", "false", "x"}},
    {"replay.market", {"us", "kr", "x"}},
    {"replay.cash", {"100000", "1e5", "0", "-1", "x", "1e400"}},
    {"replay.commission", {"0", "0.001", "-1", "x"}},
    {"replay.min_commission", {"0", "1", "x"}},
    {"replay.sell_tax", {"0", "0.0018", "x"}},
    {"replay.slippage_ticks", {"0", "1", "-1", "99999999999", "x"}},
    {"replay.shorts", {"true", "false", "x"}},
    {"instrument.x", {"AAA", "BBB", "ZZZ"}},
  };
  std::size_t read = 0;
  std::size_t refused = 0;
  std::size_t replayed = 0;
  // CMDATL_TEST_SWEEP names a longer sweep, as the sanitizer runs take.
  const char* const asked = std::getenv("CMDATL_TEST_SWEEP");
  const int count = asked ? std::atoi(asked) : 3000;
  for (int k = 0; k < count; ++k) {
    // A third each: a long CSV, a wide table, or columns at random; then cells and settings changed at random.
    const auto shape = k % 3;
    const auto mutate = shape == 2 ? 100 : 8;
    std::vector<std::string> header;
    if (shape == 0) {
      header = {"ts", "open", "high", "low", "close", "volume"};
      if (chance(30)) {
        header.push_back("stamp");
      }
    } else if (shape == 1) {
      header = {"date", "AAA", "BBB"};
    } else {
      const auto width = std::uniform_int_distribution<int>(0, 8)(random);
      for (int c = 0; c < width; ++c) {
        header.push_back(chance(80) ? columns[static_cast<std::size_t>(c) % 8] : pick(columns));
      }
    }
    std::string text;
    for (std::size_t c = 0; c < header.size(); ++c) {
      text += (c == 0 ? "" : ",") + (chance(3) ? pick(columns) : header[c]);
    }
    text += chance(10) ? "\r\n" : "\n";
    const auto rows = std::uniform_int_distribution<int>(0, 8)(random);
    for (int r = 0; r < rows; ++r) {
      const auto n = chance(95) ? header.size() : std::uniform_int_distribution<std::size_t>(0, 9)(random);
      for (std::size_t c = 0; c < n; ++c) {
        std::string cell;
        if (chance(mutate)) {
          cell = pick(cells);
        } else if (c == 0) {
          cell = shape == 1 ? std::to_string(20200101 + r) : std::to_string(1577836800 + 86400 * r);
        } else if (c < header.size() && header[c] == "stamp") {
          cell = "close";
        } else {
          cell = std::to_string(100 + r + static_cast<int>(c));
        }
        text += (c == 0 ? "" : ",") + cell;
      }
      text += chance(3) ? "," : "";
      text += chance(5) ? "" : "\n";
    }
    const auto file = dir.write("f" + std::to_string(k % 4) + ".csv", text);
    std::vector<std::string> args;
    if (shape == 0) {
      args = {"data.source=csv", "data.symbol=AAA"};
    } else if (shape == 1) {
      args = {"data.source=wide"};
    }
    args.push_back("data.file=" + file);
    for (const auto& [key, values] : keys) {
      if (chance(shape == 2 ? 25 : 6)) {
        args.push_back(key + "=" + pick(values));
      }
    }
    if (chance(10)) {
      args.push_back("data.open=" + file);
      args.push_back("data.high=" + file);
      args.push_back("data.low=" + file);
    }
    if (chance(5)) {
      args.push_back("data.names=" + file);
    }
    if (chance(3)) {
      args.push_back(pick(cells));
    }
    INFO(k << ": " << text);
    try {
      const auto s = settings(args);
      const auto data = replay::load(s, chance(30) ? std::vector<std::string>{"AAA"} : std::vector<std::string>{});
      const bool wide = data.series_.empty() && !data.close_.names_.empty();
      const auto market = replay::market_of(s);
      const bool open_point = s.get_or("data.open_point", "false") == "true";
      const auto feed = wide ? replay::events(data, market, open_point) : replay::bar_events(data, market, open_point);
      (void)replay::replay_config(s, data);
      (void)replay::fill_model(s);
      (void)feed;
      ++read;
      if (chance(40)) {
        args.push_back("instrument.x=AAA");
        const auto ran = replay_main<runtest::Program>(args);
        CHECK((ran.exit_ == 0 || ran.err_.rfind("error: ", 0) == 0));
        replayed += ran.exit_ == 0 ? 1 : 0;
      }
    } catch (const std::exception&) {
      ++refused;
    }
  }
  INFO("read " << read << ", refused " << refused << ", replayed " << replayed);
  CHECK(read > static_cast<std::size_t>(count / 20));
  CHECK(refused > static_cast<std::size_t>(count / 20));
  CHECK(replayed > static_cast<std::size_t>(count / 30));
}
