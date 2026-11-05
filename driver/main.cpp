// The run driver's entry: the program atlc generated, its data read and the strategy built by cmdatl's replay module,
// run on the mock market by cmdatl's driver. `program.hpp` is written for each build and names the program's struct.

#include "program.hpp"

#include <cmdatl/driver/run.hpp>
#include <cmdatl/replay/replay.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace atl = algotradelib;
namespace replay = cmdatl::replay;

/**
 * @brief The settings from the run's `key=value` arguments, as `cmdatl::replay::main` reads its command line.
 * @param args The arguments.
 * @return The settings.
 */
cmdatl::replay::Settings settings_of(const std::vector<std::string>& args) {
  std::vector<std::string> owned{"driver"};
  owned.insert(owned.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& arg : owned) {
    argv.push_back(arg.data());
  }
  return cmdatl::replay::Settings::from_args(static_cast<int>(argv.size()), argv.data());
}

/**
 * @brief The market's inputs, as `cmdatl::replay::main` reads them: the data, its events by the replay's own dispatch,
 *   regrouped per instrument; the instruments, the cash and the costs.
 * @param args The run's arguments.
 * @return The inputs.
 */
cmdatl::driver::Loaded load(const std::vector<std::string>& args) {
  const auto s = settings_of(args);
  const auto symbols = replay::symbols_of<Program>(s);
  const auto data = replay::load(s, symbols);
  if (std::string(Program::atls_mode) == "single" && data.symbols_.size() != 1) {
    throw std::invalid_argument(
      "a single-instrument program trades one symbol, and the data holds " + std::to_string(data.symbols_.size()) +
      ": name it in [instruments]"
    );
  }
  const auto market = replay::market_of(s);
  const bool wide = data.series_.empty() && !data.close_.names_.empty();
  const auto feed = wide ? replay::events(data, market, false) : replay::bar_events(data, market, false);
  const auto config = replay::replay_config(s, data);
  cmdatl::driver::Loaded loaded;
  loaded.symbols_ = data.symbols_;
  loaded.instruments_ = config.instruments_;
  loaded.cash_ = config.cash_;
  // The run's fill model: costs only, since a run refuses a market-price rule (its fills are the market's).
  loaded.fills_ = program_fills(replay::fill_model(s));
  std::vector<bool> started(loaded.instruments_.size(), false);
  for (const auto& instrument : loaded.instruments_) {
    atl::BarSeries series;
    series.instrument_ = instrument.id_;
    series.currency_ = instrument.currency_;
    loaded.series_.push_back(std::move(series));
  }
  for (const auto& event : feed) {
    std::size_t at = 0;
    while (at < loaded.series_.size() && !(loaded.series_[at].instrument_ == event.instrument_)) {
      ++at;
    }
    if (at == loaded.series_.size()) {
      throw std::invalid_argument("a bar of " + event.instrument_.symbol_ + ", which the run does not trade");
    }
    auto& series = loaded.series_[at];
    if (!started[at]) {
      started[at] = true;
      series.interval_ = event.interval_;
      series.stamp_ = event.stamp_;
    } else if (series.interval_ != event.interval_ || series.stamp_ != event.stamp_) {
      throw std::invalid_argument(event.instrument_.symbol_ + "'s bars change their interval or stamp");
    }
    series.bars_.push_back(event.bar_);
  }
  if constexpr (replay::reads_data<Program>::value) {
    for (const auto& row : replay::load_data(s, Program::atls_data(), data.symbols_)) {
      cmdatl::driver::DataPoint point;
      point.at_ = row.known_at_;
      point.name_ = row.name_;
      if (row.symbol_) {
        point.instrument_ = replay::instrument_of(*row.symbol_, market).id_;
      }
      point.value_ = row.value_;
      loaded.data_.push_back(std::move(point));
    }
  }
  return loaded;
}

/**
 * @brief The strategy, as `cmdatl::replay::main` builds it: for the instruments, its parameters, then its pricing.
 * @param args The run's arguments.
 * @param ids The instruments.
 * @return The strategy.
 */
Program make(const std::vector<std::string>& args, const std::vector<atl::InstrumentId>& ids) {
  const auto s = settings_of(args);
  Program strategy(ids);
  replay::set_params(strategy, s);
  // After the final parameter values, though `set` keeps the pricing.
  replay::set_pricing(strategy, s);
  return strategy;
}

} // namespace

/**
 * @brief Runs the program on its own mock market; see `cmdatl::driver::run_main`.
 * @param argc The count.
 * @param argv `run.id=ID`, then atlc's arguments.
 * @return The run's exit.
 */
int main(int argc, char** argv) {
  return cmdatl::driver::run_main<Program>(argc, argv, load, make);
}
