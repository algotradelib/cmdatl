#pragma once

// The run driver's program header for a program that states neither `atls_reads_data` nor `atls_data()`, as atlc
// generated before data {}: the driver must still compile (target driver-no-data-facts).

#include "../run-strategy.hpp"

#include <algotradelib/backtest>

#include <utility>
#include <vector>

using Program = runtest::Program;

inline algotradelib::backtest::NextBarFillModel program_fills(algotradelib::backtest::NextBarFillModel model) {
  return model;
}

inline std::vector<std::pair<const char*, double (*)(const algotradelib::backtest::MeasureInput&)>> program_measures() {
  return {};
}
