#pragma once

// A hand-written program with the interface of an atlc-generated strategy, and a loader standing in for atlc's, for
// the run's in-process tests: the driver never needs atlc's runtime for them.

#include <cmdatl/driver/run.hpp>

#include <algotradelib/backtest>
#include <algotradelib/strategy>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace runtest {

namespace atl = algotradelib;

/** @brief One decision of the script: a target on the first symbol, as a limit at a multiple of the bar's close. */
struct Decision {
  /** @brief The target, in shares. */
  std::int64_t target_{0};
  /** @brief The limit, as a multiple of the bar's close (rounded onto the tick grid). */
  std::string factor_{"1"};
  /** @brief Whether to submit a market order (no limit) instead. */
  bool market_{false};
};

/** @brief What every test program does, set by a test before the run. */
struct Script {
  /** @brief The bars before the first decision. */
  std::size_t warmup_{1};
  /** @brief The decisions, by bar number from 1; a bar without one submits nothing. */
  std::map<std::size_t, Decision> decisions_;
  /** @brief Side effects inside the run, by bar number, before that bar's decision: a step hook without a hook. */
  std::map<std::size_t, std::function<void()>> actions_;
  /** @brief The warm-up the program states, when not `warmup_` (a program that never decides). */
  std::optional<std::size_t> stated_warmup_;
};

/** @brief The script the programs read; each test sets it before it runs. */
inline Script& script() {
  static Script s;
  return s;
}

/** @brief What the test loader gives, set by a test before the run. */
struct Data {
  /** @brief How many weekdays of bars, from 2024-03-25. */
  int days_{10};
  /** @brief The bars' interval. */
  atl::BarInterval interval_{atl::BarInterval::ONE_DAY};
  /** @brief The opening cash. */
  std::string cash_{"100000000"};
  /** @brief The commission rate. */
  std::string commission_{"0"};
  /** @brief Whether the loader refuses the data. */
  bool refuse_{false};
};

/** @brief The data the loader reads; each test sets it before it runs. */
inline Data& data() {
  static Data d;
  return d;
}

/**
 * @brief Daily bars stamped at their close at 00:00 UTC, as atlc stamps date-only rows: a walk around 78,000 won, each
 *   open the previous close, each range 1,000 won either side.
 * @param symbol The symbol.
 * @param days How many weekdays, from Monday 2024-03-25.
 * @param interval The interval.
 * @return The series.
 */
inline atl::BarSeries bars_of(const std::string& symbol, int days, atl::BarInterval interval) {
  atl::BarSeries series;
  series.instrument_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol};
  series.interval_ = interval;
  series.stamp_ = atl::BarStamp::CLOSE;
  series.currency_ = atl::Currency::krw();
  // 2024-03-25 00:00 UTC is day 19807 from 1970-01-01, a Monday.
  long close = 78000;
  for (int k = 0, made = 0; made < days; ++k) {
    const int weekday = (19807 + k + 3) % 7;
    if (weekday >= 5) {
      continue;
    }
    const long open = close;
    close = open + ((k * 37) % 7 - 3) * 300;
    atl::Bar bar;
    bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (19807 + k));
    bar.open_ = atl::Decimal64(open, 0);
    bar.high_ = atl::Decimal64(std::max(open, close) + 1000, 0);
    bar.low_ = atl::Decimal64(std::min(open, close) - 1000, 0);
    bar.close_ = atl::Decimal64(close, 0);
    bar.volume_ = atl::Decimal64(100000, 0);
    series.bars_.push_back(bar);
    ++made;
  }
  return series;
}

/**
 * @brief The symbols a run names: `instrument.x=SYMBOL`, in order; `005930` when none.
 * @param args atlc's arguments.
 * @return The symbols.
 */
inline std::vector<std::string> symbols_of(const std::vector<std::string>& args) {
  std::vector<std::string> symbols;
  for (const auto& arg : args) {
    if (arg.rfind("instrument.", 0) == 0) {
      symbols.push_back(arg.substr(arg.find('=') + 1));
    }
  }
  if (symbols.empty()) {
    symbols.push_back("005930");
  }
  return symbols;
}

/** @brief The `load` the tests give the driver: the script's data for the named symbols, KRX instruments. */
struct Load {
  /**
   * @brief Loads.
   * @param args atlc's arguments.
   * @return The market's inputs.
   */
  cmdatl::driver::Loaded operator()(const std::vector<std::string>& args) const {
    if (data().refuse_) {
      throw std::invalid_argument("the data has no symbol 'XYZ'");
    }
    cmdatl::driver::Loaded loaded;
    for (const auto& symbol : symbols_of(args)) {
      loaded.symbols_.push_back(symbol);
      loaded.series_.push_back(bars_of(symbol, data().days_, data().interval_));
      atl::Instrument instrument;
      instrument.id_ = atl::InstrumentId{atl::Country::KR, atl::Venue::COMBINED, symbol};
      instrument.name_ = symbol;
      instrument.type_ = atl::SecurityType::STOCK;
      instrument.currency_ = atl::Currency::krw();
      instrument.lot_size_ = atl::Decimal64::parse("1");
      instrument.price_rule_ = atl::PriceRule::krx_equity();
      loaded.instruments_.push_back(instrument);
    }
    const auto rate = atl::Decimal64::parse(data().commission_);
    if (!(rate == atl::Decimal64::parse("0"))) {
      loaded.fills_ = atl::backtest::NextBarFillModel(
        atl::backtest::CommissionModel::of(rate, atl::Decimal64::parse("0")), atl::backtest::SlippageModel::none()
      );
    }
    loaded.cash_ = atl::Decimal64::parse(data().cash_);
    return loaded;
  }
};

/** @brief A program trading the script on its first symbol. */
struct Program {
  static constexpr const char* atls_mode = "named";
  static constexpr bool atls_legs = false;
  static std::vector<std::string> atls_instruments() { return {"x"}; }

  explicit Program(std::vector<atl::InstrumentId> ids) : ids_(std::move(ids)) {}

  std::size_t warmup_bars() const { return script().stated_warmup_.value_or(script().warmup_); }

  template <class Ctx>
  void on_bar(Ctx& ctx, const atl::strategy::BarEvent& event) {
    if (ids_.empty() || !(event.instrument_ == ids_.front())) {
      return;
    }
    ++seen_;
    const auto action = script().actions_.find(seen_);
    if (action != script().actions_.end() && action->second) {
      action->second();
    }
    if (seen_ < script().warmup_) {
      return;
    }
    const auto found = script().decisions_.find(seen_);
    if (found == script().decisions_.end()) {
      return;
    }
    const auto instrument = ctx.instrument(event.instrument_);
    if (!instrument) {
      return;
    }
    atl::strategy::TargetPosition target;
    target.instrument_ = event.instrument_;
    target.target_quantity_ = atl::Decimal64(found->second.target_, 0);
    if (!found->second.market_) {
      const auto price = atl::decimal_multiply(event.bar_.close_, atl::Decimal64::parse(found->second.factor_));
      target.limit_ = atl::strategy::Price::rounded(*price, instrument->price_rule_, atl::OrderSide::BUY);
    }
    target.tag_ = "script";
    ctx.submit(target);
  }

protected:
  std::vector<atl::InstrumentId> ids_;
  std::size_t seen_{0};
};

/** @brief The same program with session legs, which a run refuses. */
struct LegsProgram : Program {
  using Program::Program;
  static constexpr bool atls_legs = true;
};

/** @brief The same program with `on_fill`, which a live runner never calls. */
struct FillProgram : Program {
  using Program::Program;

  template <class Ctx>
  void on_fill(Ctx& ctx, const atl::strategy::Fill& fill) {
    (void)ctx;
    (void)fill;
  }
};

/** @brief The `make` the tests give the driver: the program for the ids; a parameter `param.bad` is refused. */
template <typename P>
struct Make {
  /**
   * @brief Makes.
   * @param args atlc's arguments.
   * @param ids The instruments.
   * @return The program.
   */
  P operator()(const std::vector<std::string>& args, const std::vector<atl::InstrumentId>& ids) const {
    for (const auto& arg : args) {
      if (arg.rfind("param.bad=", 0) == 0) {
        throw std::invalid_argument("param.bad: the program has no parameter 'bad'");
      }
    }
    return P(ids);
  }
};

}  // namespace runtest
