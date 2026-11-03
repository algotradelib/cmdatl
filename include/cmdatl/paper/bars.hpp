#pragma once

// The paper market's history: one strict CSV of daily bars with the columns `symbol`, `date`, `open`, `high`, `low`,
// `close` and `volume`, in any order: the columns `cmdatl strategy new` documents for bars.csv.
// A daily bar stands for that date's 09:00-15:30 KST session.

#include "./time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmdatl {
namespace paper {

/** @brief The most rows a bars file may have. */
inline constexpr std::size_t MAX_BAR_ROWS = 1000000;
/** @brief The largest bars file, in bytes. */
inline constexpr std::size_t MAX_BARS_BYTES = 64u << 20;
/** @brief The lowest price a bar or an order may have: no price rounds to zero on the KRX grid. */
inline constexpr double MIN_PRICE = 1;
/** @brief The highest price a bar or an order may have. */
inline constexpr double MAX_PRICE = 1e7;
/** @brief The largest order, and the largest holding of one symbol, in shares. */
inline constexpr std::int64_t MAX_SHARES = 100000000;
/** @brief The largest volume a bar may have. */
inline constexpr std::int64_t MAX_VOLUME = 1000000000000;

/** @brief One date's session of one symbol. */
struct DayBar {
  /** @brief The date, as days from 1970-01-01; its session opens at 00:00 UTC of it. */
  std::int64_t day_{0};
  /** @brief The open. */
  double open_{0};
  /** @brief The high. */
  double high_{0};
  /** @brief The low. */
  double low_{0};
  /** @brief The close. */
  double close_{0};
  /** @brief The volume. */
  std::int64_t volume_{0};
};

/** @brief Every symbol's bars, as one bars file gave them. */
struct MarketData {
  /** @brief The bars of each symbol, dates strictly increasing. */
  std::map<std::string, std::vector<DayBar>> bars_;
  /** @brief The file's bytes hashed (FNV-1a 64): the bytes the bars were parsed from. */
  std::uint64_t hash_{0};
};

/**
 * @brief Whether a text is a symbol: 1 to 32 of `[A-Za-z0-9._-]`.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_symbol(std::string_view text) {
  return !text.empty() && text.size() <= 32 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
      c == '-';
  });
}

/**
 * @brief FNV-1a 64 over bytes, continuing a hash.
 * @param bytes The bytes.
 * @param hash The hash so far (the offset basis to start).
 * @return The hash.
 */
inline std::uint64_t fnv1a(std::string_view bytes, std::uint64_t hash = 1469598103934665603ULL) {
  for (const unsigned char c : bytes) {
    hash = (hash ^ c) * 1099511628211ULL;
  }
  return hash;
}

/**
 * @brief Reads a price written `[0-9]{1,8}(\.[0-9]{1,4})?`, from 1 to 10^7.
 * @param text The text.
 * @return The price, or nothing.
 */
inline std::optional<double> parse_price(std::string_view text) {
  const auto dot = text.find('.');
  const auto whole = text.substr(0, dot);
  const auto fraction = dot == std::string_view::npos ? std::string_view() : text.substr(dot + 1);
  if (
    whole.empty() || whole.size() > 8 || (dot != std::string_view::npos && (fraction.empty() || fraction.size() > 4))
  ) {
    return std::nullopt;
  }
  std::int64_t units = 0;
  for (const char c : whole) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    units = units * 10 + (c - '0');
  }
  std::int64_t parts = 0;
  std::int64_t scale = 1;
  for (const char c : fraction) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    parts = parts * 10 + (c - '0');
    scale *= 10;
  }
  const double value = static_cast<double>(units) + static_cast<double>(parts) / static_cast<double>(scale);
  if (value < MIN_PRICE || value > MAX_PRICE) {
    return std::nullopt;
  }
  return value;
}

/**
 * @brief Reads a volume written `[0-9]{1,13}`, at most 10^12.
 * @param text The text.
 * @return The volume, or nothing.
 */
inline std::optional<std::int64_t> parse_volume(std::string_view text) {
  if (text.empty() || text.size() > 13) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  if (value > MAX_VOLUME) {
    return std::nullopt;
  }
  return value;
}

/** @brief Where a bars file is wrong. */
struct BarsProblem {
  /** @brief The line, from 1. */
  std::size_t line_{0};
  /** @brief What is wrong. */
  std::string message_;
};

/**
 * @brief Reads a bars file's header: exactly the seven field names, each once, in any order.
 * @param line The header line, its `\r` removed.
 * @param fields The field names, in the order the parser reads them.
 * @param column Where each field's cell is, by the field's index in `fields`.
 * @return What is wrong with the header, or nothing.
 */
inline std::optional<std::string> read_header(
  std::string_view line, const std::array<std::string_view, 7>& fields, std::array<std::size_t, 7>& column
) {
  const std::string expected = "the header names the seven columns symbol,date,open,high,low,close,volume in any order";
  std::array<bool, 7> seen{};
  std::size_t cell = 0;
  std::size_t from = 0;
  while (true) {
    const auto comma = line.find(',', from);
    const auto name = line.substr(from, comma == std::string_view::npos ? std::string_view::npos : comma - from);
    const auto at = std::find(fields.begin(), fields.end(), name);
    if (at == fields.end()) {
      if (cell == 0 && name.substr(0, 3) == "\xEF\xBB\xBF") {
        return expected + ", and begins with a byte-order mark: save the file as UTF-8 without one";
      }
      if (name.empty()) {
        return expected + ", and has an empty column name";
      }
      return expected + ", not `" + std::string(name.substr(0, 64)) + "`";
    }
    const auto field = static_cast<std::size_t>(at - fields.begin());
    if (seen[field]) {
      return expected + ", and names " + std::string(name) + " twice";
    }
    // Seven distinct names at most, so cell stays below seven.
    seen[field] = true;
    column[field] = cell++;
    if (comma == std::string_view::npos) {
      break;
    }
    from = comma + 1;
  }
  for (std::size_t k = 0; k < fields.size(); ++k) {
    if (!seen[k]) {
      return expected + ", and lacks " + std::string(fields[k]);
    }
  }
  return std::nullopt;
}

/**
 * @brief Parses a whole bars file, strictly: the header names the seven columns `symbol`, `date`, `open`, `high`,
 *   `low`, `close` and `volume`, each once, in any order; each row exactly seven cells, unquoted and without spaces;
 *   a symbol by its grammar; a real date inside the supported interval; prices by `parse_price` with low <= open,
 *   close <= high; a volume by `parse_volume`; dates strictly increasing within a symbol; at most a million rows;
 *   `\r\n` accepted, a blank line refused.
 * @param text The file's bytes.
 * @param data Where the bars go; its hash is of `text`.
 * @return The first problem, or nothing when the file is sound.
 */
inline std::optional<BarsProblem> parse_bars(std::string_view text, MarketData& data) {
  // Each field's cell in a row, in the order symbol, date, open, high, low, close, volume, as the header names them.
  const std::array<std::string_view, 7> fields{"symbol", "date", "open", "high", "low", "close", "volume"};
  std::array<std::size_t, 7> column{};
  data.bars_.clear();
  data.hash_ = fnv1a(text);
  std::size_t line_number = 0;
  std::size_t rows = 0;
  std::size_t start = 0;
  std::vector<std::string_view> cells;
  cells.reserve(8);
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    auto line = text.substr(start, end - start);
    start = end + 1;
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      return BarsProblem{line_number, "a blank line"};
    }
    if (line_number == 1) {
      if (auto problem = read_header(line, fields, column)) {
        return BarsProblem{line_number, *problem};
      }
      continue;
    }
    if (++rows > MAX_BAR_ROWS) {
      return BarsProblem{line_number, "more than " + std::to_string(MAX_BAR_ROWS) + " rows"};
    }
    cells.clear();
    std::size_t from = 0;
    while (true) {
      const auto comma = line.find(',', from);
      cells.push_back(line.substr(from, comma == std::string_view::npos ? std::string_view::npos : comma - from));
      if (comma == std::string_view::npos || cells.size() > 7) {
        break;
      }
      from = comma + 1;
    }
    if (cells.size() != 7) {
      return BarsProblem{line_number, "a row needs exactly seven cells"};
    }
    const auto symbol = cells[column[0]];
    if (!valid_symbol(symbol)) {
      return BarsProblem{line_number, "the symbol is not 1 to 32 of letters, digits, '.', '_' or '-'"};
    }
    const auto day = parse_date(cells[column[1]]);
    if (!day) {
      return BarsProblem{line_number, "the date is not a real YYYY-MM-DD date from 1990-01-01 to 2100-12-31"};
    }
    DayBar bar;
    bar.day_ = *day;
    const std::string_view names[] = {"open", "high", "low", "close"};
    double* targets[] = {&bar.open_, &bar.high_, &bar.low_, &bar.close_};
    for (std::size_t k = 0; k < 4; ++k) {
      const auto price = parse_price(cells[column[2 + k]]);
      if (!price) {
        return BarsProblem{
          line_number, "the " + std::string(names[k]) + " is not a price from 1 to 10000000 with at most four decimals"
        };
      }
      *targets[k] = *price;
    }
    if (bar.low_ > bar.open_ || bar.open_ > bar.high_ || bar.low_ > bar.close_ || bar.close_ > bar.high_) {
      return BarsProblem{line_number, "the open and the close must lie between the low and the high"};
    }
    const auto volume = parse_volume(cells[column[6]]);
    if (!volume) {
      return BarsProblem{line_number, "the volume is not a whole number up to 1000000000000"};
    }
    bar.volume_ = *volume;
    auto& series = data.bars_[std::string(symbol)];
    if (!series.empty() && series.back().day_ >= bar.day_) {
      return BarsProblem{line_number, "the dates of " + std::string(symbol) + " are not strictly increasing"};
    }
    series.push_back(bar);
  }
  if (line_number == 0) {
    return BarsProblem{1, "the file is empty"};
  }
  return std::nullopt;
}

} // namespace paper
} // namespace cmdatl
