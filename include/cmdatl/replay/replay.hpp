#pragma once

#include "./settings.hpp"

#include <algotradelib/backtest>
#include <algotradelib/strategy>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(CMDATL_DUCKDB)
#include <duckdb.h>
#endif

/**
 * @file
 * @brief The replay of a strategy `atlc build` generated: the run settings as `key=value` arguments, the bars (wide
 *   tables, CSV files or a Parquet store), the library's backtester, its report as JSON and, optionally, every fill
 *   as CSV.
 *
 * Nothing here depends on a strategy's shape, so a change of settings or parameters never rebuilds. cmdatl checks
 * the settings before it passes them; the checks here repeat what a reader needs. Nothing here includes the rest of
 * cmdatl, so a project's own `main` can include it beside `atlc build`'s header.
 *
 * Fills: a market order fills against the bar after its decision, at that bar's open, or at the price of the run's
 * market-price rule (`replay.fill_header` and `replay.fill_function`, compiled into the driver); a wide table's row is
 * a bar with open = high = low = close, so on closes alone the open is the next close.
 *
 * Sessions: the replay's session window is the market's (`kr` 09:00-15:30 KST, `us` 09:30-16:00 at UTC-5).
 * `bars.open_point` replays each daily bar as two bars, a point at the session's open (its open alone) and the day's
 * bar stamped at the close, so a program's `on_open` runs at the open and its `on_close` at the close; intraday bars
 * are replayed as they are, and the session's first and last bars run those handlers.
 *
 * Sources (`bars.source`):
 * - `wide`: tables of one field each, a yyyymmdd date column then one column per symbol.
 * - `csv`: OHLCV rows with a column mapping, one symbol per file, a symbol column, or a wide layout of closes.
 * - `store`: a Parquet store by coordinates (`source/interval/symbol`), read with DuckDB's C library: each bar's
 *   latest version, or the latest fetched at or before `bars.as_of`. Compiled only with `CMDATL_DUCKDB`.
 */

namespace cmdatl {
namespace replay {

namespace atl = algotradelib;

/**
 * @brief Removes trailing carriage returns and spaces.
 * @param cell The text, changed in place.
 */
inline void trim(std::string& cell) {
  while (!cell.empty() && (cell.back() == '\r' || cell.back() == ' ')) {
    cell.pop_back();
  }
}

/**
 * @brief A whole number read from text, or an error naming where it was read.
 * @param text The text.
 * @param where The file and line, for the error.
 * @return The number.
 */
inline long long whole_of(const std::string& text, const std::string& where) {
  std::size_t used = 0;
  long long value = 0;
  try {
    value = std::stoll(text, &used);
  } catch (const std::exception&) {
    used = 0;
  }
  if (used == 0 || used != text.size()) {
    throw std::invalid_argument(where + ": '" + text + "' is not a whole number");
  }
  return value;
}

/**
 * @brief Whether a yyyymmdd number is a date of the years 1 to 9999.
 * @param yyyymmdd The number.
 * @return Whether the date exists.
 */
inline bool date_exists(long long yyyymmdd) {
  const long long y = yyyymmdd / 10000;
  const long long m = yyyymmdd / 100 % 100;
  const long long d = yyyymmdd % 100;
  if (yyyymmdd < 0 || y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) {
    return false;
  }
  static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return d <= days[m - 1] + (m == 2 && leap ? 1 : 0);
}

/**
 * @brief An instant a number of seconds after the Unix epoch: within the years 1 to 9999, and within what this
 *   platform's clock holds less two days, so that an offset or a bar's minutes added later stay inside it (with
 *   nanosecond ticks, as on Linux, about the years 1678 to 2262).
 * @param secs The seconds.
 * @param where What is read, for errors.
 * @return The instant.
 */
inline std::chrono::system_clock::time_point epoch_plus(long long secs, const std::string& where) {
  using std::chrono::duration_cast;
  using std::chrono::seconds;
  using std::chrono::system_clock;
  const long long margin = 2 * 86400LL;
  const long long clock_min = static_cast<long long>(duration_cast<seconds>(system_clock::duration::min()).count());
  const long long clock_max = static_cast<long long>(duration_cast<seconds>(system_clock::duration::max()).count());
  // 0001-01-01T00:00:00Z and 9999-12-31T23:59:59Z.
  const long long lo = std::max(-62135596800LL, clock_min + margin);
  const long long hi = std::min(253402300799LL, clock_max - margin);
  if (secs < lo || secs > hi) {
    throw std::invalid_argument(
      where + ": " + std::to_string(secs) +
      " seconds after 1970 is outside the years 1 to 9999 or this platform's clock"
    );
  }
  return system_clock::time_point{} + seconds(secs);
}

/**
 * @brief Seconds from the Unix epoch to midnight UTC of a yyyymmdd date; a date that does not exist is refused.
 *
 * The day count is Howard Hinnant's `days_from_civil`, from "chrono-Compatible Low-Level Date Algorithms"
 * (https://howardhinnant.github.io/date_algorithms.html), which he placed in the public domain.
 *
 * @param yyyymmdd The date.
 * @return The seconds.
 */
inline long long day_seconds(int yyyymmdd) {
  if (!date_exists(yyyymmdd)) {
    throw std::invalid_argument("not a date: " + std::to_string(yyyymmdd));
  }
  int y = yyyymmdd / 10000;
  const int m = yyyymmdd / 100 % 100;
  const int d = yyyymmdd % 100;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = era * 146097LL + static_cast<long long>(doe) - 719468LL;
  return days * 86400LL;
}

/**
 * @brief Midnight UTC of a yyyymmdd date (see day_seconds() and epoch_plus()).
 * @param yyyymmdd The date.
 * @return Its midnight.
 */
inline std::chrono::system_clock::time_point day_of(int yyyymmdd) {
  return epoch_plus(day_seconds(yyyymmdd), "the date " + std::to_string(yyyymmdd));
}

/**
 * @brief Whether every character of a text is a digit.
 * @param text The text.
 * @return Whether it is non-empty and all digits.
 */
inline bool all_digits(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return c >= '0' && c <= '9';
  });
}

/**
 * @brief A date written yyyymmdd or YYYY-MM-DD; anything else, or a date that does not exist, is refused.
 * @param text The text.
 * @param where What is read, for errors.
 * @return The date, yyyymmdd.
 */
inline int date_of(const std::string& text, const std::string& where) {
  std::string digits = text;
  if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
    digits = text.substr(0, 4) + text.substr(5, 2) + text.substr(8, 2);
  }
  if (digits.size() != 8 || !all_digits(digits) || !date_exists(std::stoll(digits))) {
    throw std::invalid_argument(where + ": not a date (yyyymmdd or YYYY-MM-DD): '" + text + "'");
  }
  return std::stoi(digits);
}

/** @brief A wide table: dates (yyyymmdd) and, per column, the cell text ("" when missing). */
struct Table {
  /** @brief Each row's date, yyyymmdd. */
  std::vector<int> dates_;
  /** @brief The column names. */
  std::vector<std::string> names_;
  /** @brief The cells, by column then row. */
  std::vector<std::vector<std::string>> cells_;
  /** @brief The file, for errors. */
  std::string path_;

  /**
   * @brief The column of a name.
   * @param name The name.
   * @return Its index; throws `std::invalid_argument` when there is none.
   */
  std::size_t col(const std::string& name) const {
    const auto it = std::find(names_.begin(), names_.end(), name);
    if (it == names_.end()) {
      throw std::invalid_argument(path_ + ": no column '" + name + "'");
    }
    return static_cast<std::size_t>(it - names_.begin());
  }

  /**
   * @brief Keeps the rows from one date to another, inclusive.
   * @param from The first date, yyyymmdd.
   * @param to The last date, yyyymmdd.
   */
  void slice(int from, int to) {
    const auto first = std::lower_bound(dates_.begin(), dates_.end(), from);
    const auto last = std::upper_bound(first, dates_.end(), to);
    const auto begin = first - dates_.begin();
    const auto end = last - dates_.begin();
    for (auto& column : cells_) {
      column.erase(column.begin() + end, column.end());
      column.erase(column.begin(), column.begin() + begin);
    }
    dates_.erase(last, dates_.end());
    dates_.erase(dates_.begin(), dates_.begin() + begin);
  }
};

/**
 * @brief Reads a wide table: a header (its first cell ignored), then rows of a yyyymmdd date and one cell per
 *   column; an empty cell, `NaN` or `nan` is missing.
 * @param path The file.
 * @return The table.
 */
inline Table load_wide(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("cannot open " + path);
  }
  Table table;
  table.path_ = path;
  std::string line;
  std::string cell;
  if (std::getline(file, line)) {
    std::stringstream header(line);
    std::getline(header, cell, ',');
    while (std::getline(header, cell, ',')) {
      trim(cell);
      table.names_.push_back(cell);
    }
  }
  table.cells_.resize(table.names_.size());
  std::size_t number = 1;
  while (std::getline(file, line)) {
    ++number;
    trim(line);
    if (line.empty()) {
      continue;
    }
    std::stringstream row(line);
    std::getline(row, cell, ',');
    trim(cell);
    const auto date = date_of(cell, path + ":" + std::to_string(number));
    if (!table.dates_.empty() && date <= table.dates_.back()) {
      throw std::invalid_argument(path + ":" + std::to_string(number) + ": dates must increase");
    }
    table.dates_.push_back(static_cast<int>(date));
    for (auto& column : table.cells_) {
      cell.clear();
      std::getline(row, cell, ',');
      trim(cell);
      const bool missing = cell.empty() || cell == "NaN" || cell == "nan";
      column.push_back(missing ? std::string() : cell);
    }
  }
  return table;
}

/**
 * @brief The symbols in the second line of a file (as Chan's `*_stocks.csv` holds them).
 * @param path The file.
 * @return The symbols.
 */
inline std::vector<std::string> load_names(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("cannot open " + path);
  }
  std::string line;
  std::getline(file, line);
  std::getline(file, line);
  std::vector<std::string> names;
  std::stringstream row(line);
  std::string cell;
  while (std::getline(row, cell, ',')) {
    trim(cell);
    names.push_back(cell);
  }
  return names;
}

/**
 * @brief A date setting.
 * @param text yyyymmdd or YYYY-MM-DD.
 * @return The date, yyyymmdd.
 */
inline int parse_date(const std::string& text) {
  return date_of(text, "a date setting");
}

/**
 * @brief An instrument of a market preset.
 * @param symbol The symbol.
 * @param market `us` (USD, no tick rule) or `kr` (KRW, KRX ticks).
 * @return The instrument.
 */
inline atl::Instrument instrument_of(const std::string& symbol, const std::string& market) {
  atl::Instrument instrument;
  const bool kr = market == "kr";
  instrument.id_ = atl::InstrumentId{kr ? atl::Country::KR : atl::Country::US, atl::Venue::COMBINED, symbol};
  instrument.name_ = symbol;
  instrument.type_ = atl::SecurityType::STOCK;
  instrument.currency_ = atl::Currency{kr ? "KRW" : "USD"};
  instrument.lot_size_ = atl::Decimal64::parse("1");
  instrument.price_rule_ = kr ? atl::PriceRule::krx_equity() : atl::PriceRule::unconstrained();
  return instrument;
}

/**
 * @brief The market: `replay.market`, or by default `kr` for a store coordinate whose source starts with `kis`,
 *   `kiwoom` or `toss` or contains `krx`, and `us` otherwise.
 * @param s The settings.
 * @return `us` or `kr`.
 */
inline std::string market_of(const Settings& s) {
  if (const auto m = s.get("replay.market")) {
    return *m;
  }
  if (const auto store = s.get("bars.store")) {
    const auto& c = *store;
    const bool korean = c.rfind("kis", 0) == 0 || c.rfind("kiwoom", 0) == 0 || c.rfind("toss", 0) == 0 ||
      c.substr(0, c.find('/')).find("krx") != std::string::npos;
    if (korean) {
      return "kr";
    }
  }
  return "us";
}

/** @brief One symbol's bars from an OHLCV source, in time order. */
struct BarSeries {
  /** @brief The symbol. */
  std::string symbol_;
  /** @brief The bars' interval. */
  atl::BarInterval interval_{atl::BarInterval::ONE_DAY};
  /** @brief Whether a bar is stamped at its start or its end. */
  atl::BarStamp stamp_{atl::BarStamp::CLOSE};
  /** @brief The bars. */
  std::vector<atl::Bar> bars_;
};

/** @brief The loaded data and the instruments to trade, in feed order. */
struct Data {
  /** @brief Wide tables: the closes. */
  Table close_;
  /** @brief Wide tables: the opens, highs and lows, when given. */
  std::optional<Table> open_, high_, low_;
  /** @brief The symbols traded. */
  std::vector<std::string> symbols_;
  /** @brief Wide tables: each symbol's column. */
  std::vector<std::size_t> columns_;
  /** @brief OHLCV sources (`csv`, `store`): one series per symbol, in `symbols_` order. */
  std::vector<BarSeries> series_;
};

/**
 * @brief A stamp cell.
 * @param cell `open` or `close`.
 * @param where The file and line, for errors.
 * @return The stamp.
 */
inline atl::BarStamp stamp_of(const std::string& cell, const std::string& where) {
  if (cell == "open") {
    return atl::BarStamp::OPEN;
  }
  if (cell == "close") {
    return atl::BarStamp::CLOSE;
  }
  throw std::invalid_argument(where + ": a stamp is 'open' or 'close', not '" + cell + "'");
}

/**
 * @brief Sets the stamp of a series before its next bar; a symbol's bars share one stamp.
 * @param series The series.
 * @param stamp The next bar's stamp.
 * @param where The file and line, for errors.
 */
inline void set_stamp(BarSeries& series, atl::BarStamp stamp, const std::string& where) {
  if (!series.bars_.empty() && series.stamp_ != stamp) {
    throw std::invalid_argument(
      where + ": the stamp of " + series.symbol_ + " changes; a symbol's bars share one stamp"
    );
  }
  series.stamp_ = stamp;
}

/**
 * @brief Splits one CSV line on commas (no quoting).
 * @param line The line.
 * @return The cells, trimmed.
 */
inline std::vector<std::string> split_line(const std::string& line) {
  std::vector<std::string> out;
  std::string cell;
  std::stringstream row(line);
  while (std::getline(row, cell, ',')) {
    trim(cell);
    out.push_back(cell);
  }
  if (!line.empty() && line.back() == ',') {
    out.emplace_back();
  }
  return out;
}

/**
 * @brief A bar interval by its name.
 * @param text 1m, 5m, 15m, 30m, 1h, 1d, 1w or 1mo.
 * @return The interval.
 */
inline atl::BarInterval interval_of(const std::string& text) {
  const std::pair<const char*, atl::BarInterval> known[] = {
    {"1m", atl::BarInterval::ONE_MINUTE},
    {"5m", atl::BarInterval::FIVE_MINUTES},
    {"15m", atl::BarInterval::FIFTEEN_MINUTES},
    {"30m", atl::BarInterval::THIRTY_MINUTES},
    {"1h", atl::BarInterval::ONE_HOUR},
    {"1d", atl::BarInterval::ONE_DAY},
    {"1w", atl::BarInterval::ONE_WEEK},
    {"1mo", atl::BarInterval::ONE_MONTH},
  };
  for (const auto& k : known) {
    if (text == k.first) {
      return k.second;
    }
  }
  throw std::invalid_argument("unknown interval '" + text + "' (1m, 5m, 15m, 30m, 1h, 1d, 1w, 1mo)");
}

/**
 * @brief Seconds east of UTC of a fixed offset.
 * @param text `UTC`, `Z` or `+HH:MM`.
 * @return The seconds.
 */
inline long offset_of(const std::string& text) {
  if (text.empty() || text == "UTC" || text == "Z") {
    return 0;
  }
  if ((text[0] != '+' && text[0] != '-') || text.size() != 6 || text[3] != ':') {
    throw std::invalid_argument("bars.timezone is a fixed offset such as \"+09:00\" or \"UTC\", not '" + text + "'");
  }
  const long seconds = static_cast<long>(
    whole_of(text.substr(1, 2), "bars.timezone") * 3600 + whole_of(text.substr(4, 2), "bars.timezone") * 60
  );
  return text[0] == '-' ? -seconds : seconds;
}

/**
 * @brief A two-digit field of a time, or -1 when the text there is not two digits.
 * @param text The text.
 * @param at Where the field starts.
 * @return The field's value.
 */
inline int two_digits(const std::string& text, std::size_t at) {
  if (at + 2 > text.size() || !all_digits(text.substr(at, 2))) {
    return -1;
  }
  return (text[at] - '0') * 10 + (text[at + 1] - '0');
}

/**
 * @brief An ISO time cell: YYYY-MM-DD, optionally `T` or a space then HH:MM or HH:MM:SS, optionally `Z` or
 *   `+HH:MM`/`-HH:MM`; anything after it, or a field out of range, is refused.
 * @param cell The cell.
 * @param offset Seconds east of UTC, for a cell that names no offset.
 * @param where The file and line, for errors.
 * @return The instant.
 */
inline std::chrono::system_clock::time_point iso_of(const std::string& cell, long offset, const std::string& where) {
  const auto bad = [&cell, &where]() {
    return std::invalid_argument(
      where + ": not an ISO time (YYYY-MM-DD, then optionally THH:MM[:SS] and Z or +HH:MM): '" + cell + "'"
    );
  };
  if (cell.size() < 10) {
    throw bad();
  }
  const long long day = day_seconds(date_of(cell.substr(0, 10), where));
  std::size_t at = 10;
  long secs = 0;
  if (at < cell.size() && (cell[at] == 'T' || cell[at] == ' ')) {
    const int hh = two_digits(cell, at + 1);
    const int mm = at + 3 < cell.size() && cell[at + 3] == ':' ? two_digits(cell, at + 4) : -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
      throw bad();
    }
    at += 6;
    int ss = 0;
    if (at < cell.size() && cell[at] == ':') {
      ss = two_digits(cell, at + 1);
      if (ss < 0 || ss > 59) {
        throw bad();
      }
      at += 3;
    }
    secs = hh * 3600L + mm * 60L + ss;
    if (at < cell.size() && cell[at] == 'Z') {
      offset = 0;
      at += 1;
    } else if (at < cell.size() && (cell[at] == '+' || cell[at] == '-')) {
      const int oh = two_digits(cell, at + 1);
      const int om = at + 3 < cell.size() && cell[at + 3] == ':' ? two_digits(cell, at + 4) : -1;
      if (oh < 0 || oh > 14 || om < 0 || om > 59) {
        throw bad();
      }
      offset = (cell[at] == '-' ? -1L : 1L) * (oh * 3600L + om * 60L);
      at += 6;
    }
  }
  if (at != cell.size()) {
    throw bad();
  }
  return epoch_plus(day + secs - offset, where);
}

/**
 * @brief A time cell as an instant.
 * @param cell The cell.
 * @param format `unix` (seconds), `unix_ms`, `yyyymmdd`, or `iso` (see iso_of()).
 * @param offset Seconds east of UTC of the local times (`yyyymmdd`, and `iso` cells that name no offset).
 * @param where The file and line, for errors.
 * @return The instant.
 */
inline std::chrono::system_clock::time_point time_of(
  const std::string& cell, const std::string& format, long offset, const std::string& where
) {
  if (format == "unix") {
    return epoch_plus(whole_of(cell, where), where);
  }
  if (format == "unix_ms") {
    const long long ms = whole_of(cell, where);
    // Floor division keeps the range check exact for times before 1970.
    const long long secs = ms / 1000 - (ms % 1000 < 0 ? 1 : 0);
    return epoch_plus(secs, where) +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(ms - secs * 1000));
  }
  if (format == "yyyymmdd") {
    return epoch_plus(day_seconds(date_of(cell, where)) - offset, where);
  }
  if (format == "iso") {
    return iso_of(cell, offset, where);
  }
  throw std::invalid_argument("bars.time_format is unix, unix_ms, yyyymmdd or iso, not '" + format + "'");
}

/**
 * @brief Whether a bar is inside `replay.from` and `replay.to` (dates, inclusive, in UTC).
 * @param s The settings.
 * @param at The bar's time.
 * @return Whether it is replayed.
 */
inline bool in_range(const Settings& s, std::chrono::system_clock::time_point at) {
  // In seconds, so that a far `replay.to` needs no instant of its own.
  const long long t = static_cast<long long>(std::chrono::floor<std::chrono::seconds>(at.time_since_epoch()).count());
  if (const auto from = s.get("replay.from")) {
    if (t < day_seconds(parse_date(*from))) {
      return false;
    }
  }
  if (const auto to = s.get("replay.to")) {
    if (t >= day_seconds(parse_date(*to)) + 86400LL) {
      return false;
    }
  }
  return true;
}

/**
 * @brief A price cell as a decimal; a price that is not positive is refused.
 * @param cell The cell.
 * @param where The file and line, for errors.
 * @return The price.
 */
inline atl::Decimal64 price_of(const std::string& cell, const std::string& where) {
  const auto value = decimal_of(cell, where);
  if (value.coefficient() <= 0) {
    throw std::invalid_argument(where + ": prices must be positive, got " + cell);
  }
  return value;
}

/**
 * @brief The items of a list setting.
 * @param text `a,b,c`, as cmdatl passes an array.
 * @return The items.
 */
inline std::vector<std::string> list_of(const std::string& text) {
  std::vector<std::string> out;
  std::string cell;
  std::stringstream row(text);
  while (std::getline(row, cell, ',')) {
    trim(cell);
    if (!cell.empty()) {
      out.push_back(cell);
    }
  }
  return out;
}

/**
 * @brief `bars.source = "csv"`: one file per symbol (`bars.file` with `bars.symbol`, or `bars.files` with
 *   `bars.symbols`), one long file with a symbol column (`bars.symbol_column`), or a wide file of closes
 *   (`bars.layout = "wide"`). Columns default to ts, stamp, open, high, low, close and volume.
 * @param s The settings.
 * @return One series per symbol, in order of first appearance.
 */
inline std::vector<BarSeries> load_csv_source(const Settings& s) {
  const auto layout = s.get_or("bars.layout", "long");
  const auto time_col = s.get_or("bars.time", "ts");
  const auto format = s.get_or("bars.time_format", "unix");
  const long offset = offset_of(s.get_or("bars.timezone", "UTC"));
  const auto interval = interval_of(s.get_or("bars.interval", "1d"));
  const auto stamp_setting = s.get("bars.stamp");
  std::vector<std::string> files;
  std::vector<std::string> file_symbols;
  if (const auto list = s.get("bars.files")) {
    files = list_of(*list);
    file_symbols = list_of(s.get_or("bars.symbols", ""));
  } else {
    files.push_back(s.need("bars.file"));
    if (const auto one = s.get("bars.symbol")) {
      file_symbols.push_back(*one);
    }
  }
  const auto symbol_col = s.get("bars.symbol_column");
  if (layout == "long" && !symbol_col && file_symbols.size() != files.size()) {
    throw std::invalid_argument(
      "a long CSV without bars.symbol_column needs one symbol per file (bars.symbol, or bars.symbols beside bars.files)"
    );
  }
  std::vector<BarSeries> out;
  auto series_for = [&out, interval](const std::string& symbol) -> BarSeries& {
    for (auto& series : out) {
      if (series.symbol_ == symbol) {
        return series;
      }
    }
    BarSeries series;
    series.symbol_ = symbol;
    series.interval_ = interval;
    out.push_back(std::move(series));
    return out.back();
  };
  for (std::size_t f = 0; f < files.size(); ++f) {
    std::ifstream file(files[f]);
    if (!file.is_open()) {
      throw std::runtime_error("cannot open " + files[f]);
    }
    std::string line;
    std::getline(file, line);
    trim(line);
    const auto header = split_line(line);
    auto col = [&header, &files, f](const std::string& name) -> std::size_t {
      const auto it = std::find(header.begin(), header.end(), name);
      if (it == header.end()) {
        throw std::invalid_argument(
          files[f] + ": no column '" + name + "' (bars.columns.* or bars.time name the columns)"
        );
      }
      return static_cast<std::size_t>(it - header.begin());
    };
    const std::size_t t = col(time_col);
    std::optional<std::size_t> stamp_col;
    atl::BarStamp fixed_stamp = atl::BarStamp::CLOSE;
    if (stamp_setting && (*stamp_setting == "open" || *stamp_setting == "close")) {
      fixed_stamp = *stamp_setting == "open" ? atl::BarStamp::OPEN : atl::BarStamp::CLOSE;
    } else if (stamp_setting) {
      stamp_col = col(*stamp_setting);
    } else if (std::find(header.begin(), header.end(), "stamp") != header.end()) {
      stamp_col = col("stamp");
    }
    std::size_t number = 1;
    if (layout == "wide") {
      // A time column, then one column of closes per symbol.
      while (std::getline(file, line)) {
        ++number;
        trim(line);
        if (line.empty()) {
          continue;
        }
        const auto where = files[f] + ":" + std::to_string(number);
        const auto cells = split_line(line);
        if (cells.size() != header.size()) {
          throw std::invalid_argument(
            where + ": expected " + std::to_string(header.size()) + " cells, found " + std::to_string(cells.size())
          );
        }
        const auto at = time_of(cells[t], format, offset, where);
        if (!in_range(s, at)) {
          continue;
        }
        const auto stamp = stamp_col ? stamp_of(cells[*stamp_col], where) : fixed_stamp;
        for (std::size_t c = 0; c < header.size(); ++c) {
          if (c == t || (stamp_col && c == *stamp_col) || cells[c].empty() || cells[c] == "NaN" || cells[c] == "nan") {
            continue;
          }
          atl::Bar bar;
          bar.timestamp_ = at;
          bar.close_ = price_of(cells[c], where);
          bar.open_ = bar.close_;
          bar.high_ = bar.close_;
          bar.low_ = bar.close_;
          bar.volume_ = atl::Decimal64::parse("100000");
          auto& series = series_for(header[c]);
          set_stamp(series, stamp, where);
          if (!series.bars_.empty() && !(series.bars_.back().timestamp_ < bar.timestamp_)) {
            throw std::invalid_argument(where + ": times must increase");
          }
          series.bars_.push_back(bar);
        }
      }
      continue;
    }
    const std::size_t o = col(s.get_or("bars.columns.open", "open"));
    const std::size_t h = col(s.get_or("bars.columns.high", "high"));
    const std::size_t l = col(s.get_or("bars.columns.low", "low"));
    const std::size_t c = col(s.get_or("bars.columns.close", "close"));
    const std::size_t v = col(s.get_or("bars.columns.volume", "volume"));
    const std::optional<std::size_t> sym = symbol_col ? std::optional<std::size_t>(col(*symbol_col)) : std::nullopt;
    while (std::getline(file, line)) {
      ++number;
      trim(line);
      if (line.empty()) {
        continue;
      }
      const auto cells = split_line(line);
      const auto where = files[f] + ":" + std::to_string(number);
      if (cells.size() != header.size()) {
        throw std::invalid_argument(
          where + ": expected " + std::to_string(header.size()) + " cells, found " + std::to_string(cells.size())
        );
      }
      if (cells[c].empty() || cells[c] == "NaN" || cells[c] == "nan") {
        continue;
      }
      const auto at = time_of(cells[t], format, offset, where);
      if (!in_range(s, at)) {
        continue;
      }
      atl::Bar bar;
      bar.timestamp_ = at;
      bar.open_ = price_of(cells[o], where);
      bar.high_ = price_of(cells[h], where);
      bar.low_ = price_of(cells[l], where);
      bar.close_ = price_of(cells[c], where);
      bar.volume_ = decimal_of(cells[v].empty() ? "0" : cells[v], where);
      auto& series = series_for(sym ? cells[*sym] : file_symbols[f]);
      set_stamp(series, stamp_col ? stamp_of(cells[*stamp_col], where) : fixed_stamp, where);
      if (!series.bars_.empty() && !(series.bars_.back().timestamp_ < bar.timestamp_)) {
        throw std::invalid_argument(where + ": times must increase within a symbol");
      }
      series.bars_.push_back(bar);
    }
  }
  return out;
}

#if defined(CMDATL_DUCKDB)

namespace duck {

/**
 * @brief A SQL string literal.
 * @param text The text.
 * @return The text in single quotes, a quote inside doubled.
 */
inline std::string literal(const std::string& text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out.push_back('\'');
    }
    out.push_back(c);
  }
  return out + "'";
}

/** @brief An in-memory DuckDB database and one connection to it, closed when destroyed. */
class Connection {
public:
  /** @brief Opens the database; throws `std::runtime_error` when DuckDB cannot. */
  Connection() {
    if (duckdb_open(nullptr, &database_) == DuckDBError) {
      throw std::runtime_error("duckdb: cannot open an in-memory database");
    }
    if (duckdb_connect(database_, &connection_) == DuckDBError) {
      duckdb_close(&database_);
      throw std::runtime_error("duckdb: cannot connect");
    }
  }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  /** @brief Closes the connection and the database. */
  ~Connection() {
    duckdb_disconnect(&connection_);
    duckdb_close(&database_);
  }

  /**
   * @brief Runs a query and reads every cell as text.
   * @param sql The query; every column it selects must be VARCHAR.
   * @return The rows; a NULL cell is empty.
   */
  std::vector<std::vector<std::string>> rows(const std::string& sql) {
    duckdb_result result;
    if (duckdb_query(connection_, sql.c_str(), &result) == DuckDBError) {
      const std::string message = duckdb_result_error(&result);
      duckdb_destroy_result(&result);
      throw std::runtime_error("duckdb: " + message);
    }
    const std::unique_ptr<duckdb_result, void (*)(duckdb_result*)> owned(&result, duckdb_destroy_result);
    const idx_t columns = duckdb_column_count(&result);
    std::vector<std::vector<std::string>> out;
    for (;;) {
      duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
      if (!chunk) {
        break;
      }
      const std::unique_ptr<duckdb_data_chunk, void (*)(duckdb_data_chunk*)> chunk_owned(
        &chunk, duckdb_destroy_data_chunk
      );
      const idx_t size = duckdb_data_chunk_get_size(chunk);
      std::vector<duckdb_string_t*> data(columns);
      std::vector<uint64_t*> validity(columns);
      for (idx_t c = 0; c < columns; ++c) {
        duckdb_vector vector = duckdb_data_chunk_get_vector(chunk, c);
        data[c] = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vector));
        validity[c] = duckdb_vector_get_validity(vector);
      }
      for (idx_t r = 0; r < size; ++r) {
        std::vector<std::string> row(columns);
        for (idx_t c = 0; c < columns; ++c) {
          if (validity[c] && !duckdb_validity_row_is_valid(validity[c], r)) {
            continue;
          }
          row[c].assign(duckdb_string_t_data(&data[c][r]), duckdb_string_t_length(data[c][r]));
        }
        out.push_back(std::move(row));
      }
    }
    return out;
  }

private:
  duckdb_database database_{nullptr};
  duckdb_connection connection_{nullptr};
};

} // namespace duck

#endif

/**
 * @brief `bars.source = "store"`: each coordinate of `bars.store` (`source/interval/symbol`) from the Parquet store
 *   under `bars.store_root`, `bars/source=SOURCE/interval=INTERVAL/symbol=SYMBOL/bars.parquet`; of each bar, the
 *   latest version fetched at or before `bars.as_of` (the latest of all when unset). An in-memory DuckDB reads the
 *   files and writes nothing.
 * @param s The settings.
 * @return One series per coordinate.
 */
inline std::vector<BarSeries> load_store_source(const Settings& s) {
#if defined(CMDATL_DUCKDB)
  const auto root = s.need("bars.store_root");
  duck::Connection con;
  con.rows("SET TimeZone = 'UTC'");
  con.rows("SET autoinstall_known_extensions = false");
  con.rows("SET autoload_known_extensions = false");
  std::vector<BarSeries> out;
  for (const auto& coordinate : list_of(s.need("bars.store"))) {
    const auto a = coordinate.find('/');
    const auto b = a == std::string::npos ? a : coordinate.find('/', a + 1);
    if (a == std::string::npos || b == std::string::npos) {
      throw std::invalid_argument("a store coordinate is source/interval/symbol, not '" + coordinate + "'");
    }
    const auto source = coordinate.substr(0, a);
    const auto interval = coordinate.substr(a + 1, b - a - 1);
    const auto symbol = coordinate.substr(b + 1);
    const auto path = root + "/bars/source=" + source + "/interval=" + interval + "/symbol=" + symbol + "/bars.parquet";
    if (!std::ifstream(path).good()) {
      throw std::invalid_argument("the store has no " + coordinate + " (" + path + ")");
    }
    const auto as_of = s.get("bars.as_of");
    const std::string cutoff =
      as_of ? " WHERE fetched_at <= " + duck::literal(*as_of) + "::TIMESTAMPTZ" : std::string();
    const auto rows = con.rows(
      "SELECT epoch(ts)::BIGINT::VARCHAR, stamp::VARCHAR, open::VARCHAR, high::VARCHAR, low::VARCHAR, close::VARCHAR, "
      "volume::VARCHAR FROM (SELECT *, row_number() OVER (PARTITION BY ts ORDER BY fetched_at DESC) AS rn FROM read_parquet(" +
      duck::literal(path) + ", hive_partitioning = false)" + cutoff + ") WHERE rn = 1 ORDER BY ts"
    );
    BarSeries series;
    series.symbol_ = symbol;
    series.interval_ = interval_of(interval);
    for (std::size_t row = 0; row < rows.size(); ++row) {
      const auto& cells = rows[row];
      const auto where = coordinate + " row " + std::to_string(row + 1);
      atl::Bar bar;
      bar.timestamp_ = epoch_plus(whole_of(cells[0], where), where);
      if (!in_range(s, bar.timestamp_)) {
        continue;
      }
      set_stamp(series, stamp_of(cells[1], where), where);
      bar.open_ = price_of(cells[2], where);
      bar.high_ = price_of(cells[3], where);
      bar.low_ = price_of(cells[4], where);
      bar.close_ = price_of(cells[5], where);
      bar.volume_ = decimal_of(cells[6].empty() ? "0" : cells[6], where);
      series.bars_.push_back(bar);
    }
    out.push_back(std::move(series));
  }
  return out;
#else
  (void)s;
  throw std::invalid_argument("this binary was built without DuckDB (cmdatl builds with it when a spec reads a store)");
#endif
}

/**
 * @brief The session window of a market preset: phases, `DAY` orders and a program's `on_open`/`on_close` follow it.
 * @param market `kr` or `us`.
 * @return KRX's regular session, or the US regular session at a fixed UTC-5 (daylight saving is not modelled).
 */
inline atl::strategy::SessionWindow session_of(const std::string& market) {
  if (market == "kr") {
    return atl::strategy::SessionWindow::krx();
  }
  atl::strategy::SessionWindow us;
  us.open_ = std::chrono::hours(9) + std::chrono::minutes(30);
  us.close_ = std::chrono::hours(16);
  us.utc_offset_ = std::chrono::hours(-5);
  us.source_ = "US regular session, 09:30-16:00 at UTC-5 (cmdatl's us preset; daylight saving is not modelled)";
  return us;
}

/**
 * @brief Noon, market time, of the date a daily bar states.
 *
 * A stamp whose market-local time of day is in the daytime (06:00 to 18:00) is a session time, such as a US bar
 * stamped at its 09:30 open (14:30 UTC in winter, 13:30 UTC in summer) or its close, and dates by its market-local
 * date. Any other stamp is some time zone's midnight (UTC, the market's, or another offset, such as an Eastern
 * Daylight midnight at 04:00 UTC, which a fixed UTC-5 window reads as 23:00), so twelve hours later it is still that
 * date in UTC.
 *
 * @param stamp The daily bar's timestamp.
 * @param session The market's session window.
 * @return An instant on that date in the market's time zone.
 */
inline std::chrono::system_clock::time_point market_noon(
  std::chrono::system_clock::time_point stamp, const atl::strategy::SessionWindow& session
) {
  using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
  const auto local = stamp + session.utc_offset_;
  const auto time_of_day = local - std::chrono::floor<Days>(local);
  const bool daytime = !(time_of_day < std::chrono::hours(6)) && time_of_day < std::chrono::hours(18);
  const auto date =
    daytime ? std::chrono::floor<Days>(local) : std::chrono::floor<Days>(stamp + std::chrono::hours(12));
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(date) + std::chrono::hours(12) -
    session.utc_offset_;
}

/**
 * @brief A daily bar as two bars (`bars.open_point`): a point at the session's open, its open alone, and the day's
 *   bar stamped at the session's close; both daily and close-stamped, so the first is an empty period.
 * @param id The instrument.
 * @param bar The daily bar.
 * @param session The market's session window.
 * @param out Where the two bars go.
 */
inline void split_day(
  const atl::InstrumentId& id, const atl::Bar& bar, const atl::strategy::SessionWindow& session,
  std::vector<atl::strategy::BarEvent>& out
) {
  const auto noon = market_noon(bar.timestamp_, session);
  const auto open = bar.open_ ? *bar.open_ : bar.close_;
  atl::Bar point;
  point.timestamp_ = session.opens_at(noon);
  point.open_ = open;
  point.high_ = open;
  point.low_ = open;
  point.close_ = open;
  point.volume_ = atl::Decimal64::parse("0");
  out.push_back(atl::strategy::make_bar_event(id, atl::BarInterval::ONE_DAY, atl::BarStamp::CLOSE, point));
  atl::Bar day = bar;
  day.timestamp_ = session.closes_at(noon);
  out.push_back(atl::strategy::make_bar_event(id, atl::BarInterval::ONE_DAY, atl::BarStamp::CLOSE, day));
}

/**
 * @brief The bar events of OHLCV series: each bar as it is, or each daily bar split at the open.
 * @param data The series.
 * @param market The market preset.
 * @param open_point Whether each daily bar is replayed as a point at the open and the day's bar (`bars.open_point`).
 * @return The events, in time order (then series order).
 */
inline std::vector<atl::strategy::BarEvent> bar_events(const Data& data, const std::string& market, bool open_point) {
  struct Tagged {
    std::chrono::system_clock::time_point at_;
    std::size_t series_;
    std::size_t bar_;
  };
  std::vector<Tagged> order;
  for (std::size_t k = 0; k < data.series_.size(); ++k) {
    const auto& series = data.series_[k];
    if (open_point && series.interval_ != atl::BarInterval::ONE_DAY) {
      throw std::invalid_argument("bars.open_point splits daily bars; intraday bars are replayed as they are");
    }
    for (std::size_t i = 0; i < series.bars_.size(); ++i) {
      order.push_back({series.bars_[i].timestamp_, k, i});
    }
  }
  std::stable_sort(order.begin(), order.end(), [](const Tagged& a, const Tagged& b) {
    return a.at_ < b.at_ || (a.at_ == b.at_ && a.series_ < b.series_);
  });
  std::vector<atl::InstrumentId> ids;
  for (const auto& series : data.series_) {
    ids.push_back(instrument_of(series.symbol_, market).id_);
  }
  std::vector<atl::strategy::BarEvent> out;
  out.reserve(order.size() * (open_point ? 2 : 1));
  if (!open_point) {
    for (const auto& t : order) {
      const auto& series = data.series_[t.series_];
      out.push_back(
        atl::strategy::make_bar_event(ids[t.series_], series.interval_, series.stamp_, series.bars_[t.bar_])
      );
    }
    return out;
  }
  // Every instrument's open point of a date before any of its day bars.
  const auto session = session_of(market);
  for (std::size_t a = 0; a < order.size();) {
    std::size_t b = a;
    const auto date = market_noon(data.series_[order[a].series_].bars_[order[a].bar_].timestamp_, session);
    while (
      b < order.size() && market_noon(data.series_[order[b].series_].bars_[order[b].bar_].timestamp_, session) == date
    ) {
      ++b;
    }
    std::vector<atl::strategy::BarEvent> two;
    for (std::size_t i = a; i < b; ++i) {
      split_day(ids[order[i].series_], data.series_[order[i].series_].bars_[order[i].bar_], session, two);
    }
    for (std::size_t i = 0; i < two.size(); i += 2) {
      out.push_back(two[i]);
    }
    for (std::size_t i = 1; i < two.size(); i += 2) {
      out.push_back(two[i]);
    }
    a = b;
  }
  return out;
}

/**
 * @brief Reads the data settings.
 * @param s The settings.
 * @param symbols The symbols to trade, in order; empty for every symbol of the data.
 * @return The data.
 */
inline Data load(const Settings& s, const std::vector<std::string>& symbols) {
  Data data;
  const auto source = s.get_or("bars.source", s.get("bars.store") ? "store" : "wide");
  if (source == "csv" || source == "store") {
    auto all = source == "csv" ? load_csv_source(s) : load_store_source(s);
    if (symbols.empty()) {
      data.series_ = std::move(all);
    } else {
      for (const auto& symbol : symbols) {
        const auto it = std::find_if(all.begin(), all.end(), [&symbol](const BarSeries& x) {
          return x.symbol_ == symbol;
        });
        if (it == all.end()) {
          throw std::invalid_argument("the data has no symbol '" + symbol + "'");
        }
        data.series_.push_back(*it);
      }
    }
    for (const auto& series : data.series_) {
      data.symbols_.push_back(series.symbol_);
    }
    return data;
  }
  if (source != "wide") {
    throw std::invalid_argument("bars.source is wide, csv or store, not '" + source + "'");
  }
  const auto names = s.get("bars.names");
  auto read = [&s, &names](const std::string& path) {
    auto t = load_wide(path);
    if (names) {
      t.names_ = load_names(*names);
      if (t.names_.size() != t.cells_.size()) {
        throw std::invalid_argument(
          *names + " names " + std::to_string(t.names_.size()) + " columns and " + path + " has " +
          std::to_string(t.cells_.size())
        );
      }
    }
    const auto from = s.get("replay.from");
    const auto to = s.get("replay.to");
    if (from || to) {
      t.slice(from ? parse_date(*from) : 0, to ? parse_date(*to) : 99991231);
    }
    return t;
  };
  if (const auto file = s.get("bars.file")) {
    data.close_ = read(*file);
  } else {
    data.close_ = read(s.need("bars.close"));
  }
  if (const auto f = s.get("bars.open")) {
    data.open_ = read(*f);
  }
  if (const auto f = s.get("bars.high")) {
    data.high_ = read(*f);
  }
  if (const auto f = s.get("bars.low")) {
    data.low_ = read(*f);
  }
  for (const auto* other : {&data.open_, &data.high_, &data.low_}) {
    if (*other && ((*other)->dates_ != data.close_.dates_ || (*other)->names_ != data.close_.names_)) {
      throw std::invalid_argument((*other)->path_ + " has other dates or columns than " + data.close_.path_);
    }
  }
  data.symbols_ = symbols.empty() ? data.close_.names_ : symbols;
  for (const auto& symbol : data.symbols_) {
    data.columns_.push_back(data.close_.col(symbol));
  }
  return data;
}

/**
 * @brief The bar events of wide tables: a row a day, or each row split at the open.
 * @param data The tables.
 * @param market The market preset.
 * @param open_point Whether each day is replayed as a point at the open and the day's bar (`bars.open_point`).
 * @return The events, in time order (then symbol order).
 */
inline std::vector<atl::strategy::BarEvent> events(const Data& data, const std::string& market, bool open_point) {
  std::vector<atl::strategy::BarEvent> out;
  const auto& close = data.close_;
  std::vector<atl::InstrumentId> ids;
  for (const auto& symbol : data.symbols_) {
    ids.push_back(instrument_of(symbol, market).id_);
  }
  const auto where = [&close](std::size_t day) {
    return close.path_ + " " + std::to_string(close.dates_[day]);
  };
  if (open_point) {
    if (!data.open_ || !data.high_ || !data.low_) {
      throw std::invalid_argument("bars.open_point on wide tables needs bars.open, bars.high, bars.low and bars.close");
    }
    const auto session = session_of(market);
    const auto volume = atl::Decimal64::parse("1000000000");
    for (std::size_t day = 0; day < close.dates_.size(); ++day) {
      std::vector<atl::strategy::BarEvent> two;
      for (std::size_t at = 0; at < ids.size(); ++at) {
        const auto c = data.columns_[at];
        const auto& op = data.open_->cells_[c][day];
        const auto& cl = close.cells_[c][day];
        if (op.empty() || cl.empty()) {
          continue;
        }
        atl::Bar bar;
        bar.timestamp_ = day_of(close.dates_[day]);
        bar.open_ = decimal_of(op, where(day));
        bar.high_ = decimal_of(data.high_->cells_[c][day], where(day));
        bar.low_ = decimal_of(data.low_->cells_[c][day], where(day));
        bar.close_ = decimal_of(cl, where(day));
        bar.volume_ = volume;
        split_day(ids[at], bar, session, two);
      }
      for (std::size_t i = 0; i < two.size(); i += 2) {
        out.push_back(two[i]);
      }
      for (std::size_t i = 1; i < two.size(); i += 2) {
        out.push_back(two[i]);
      }
    }
    return out;
  }
  const auto volume = atl::Decimal64::parse("100000");
  for (std::size_t day = 0; day < close.dates_.size(); ++day) {
    for (std::size_t at = 0; at < ids.size(); ++at) {
      const auto& text = close.cells_[data.columns_[at]][day];
      if (text.empty()) {
        continue;
      }
      atl::Bar bar;
      bar.timestamp_ = day_of(close.dates_[day]);
      bar.close_ = decimal_of(text, where(day));
      bar.open_ = bar.close_;
      bar.high_ = bar.close_;
      bar.low_ = bar.close_;
      bar.volume_ = volume;
      out.push_back(atl::strategy::make_bar_event(ids[at], atl::BarInterval::ONE_DAY, atl::BarStamp::CLOSE, bar));
    }
  }
  return out;
}

/**
 * @brief The replay settings: cash, currency, instruments, and shorts (the sellable check off); any phase.
 * @param s The settings.
 * @param data The data, for its symbols.
 * @return The configuration.
 */
/**
 * @brief Whether `Strategy` reads data (`atls_reads_data`, which atlc states for every program); a strategy that
 *   does not state it has no `data {}`.
 */
template <typename Strategy, typename = void>
struct reads_data : std::false_type {};

/** @brief A strategy that states `atls_reads_data`. */
template <typename Strategy>
struct reads_data<Strategy, std::void_t<decltype(Strategy::atls_reads_data)>>
: std::bool_constant<Strategy::atls_reads_data> {};

/** @brief One value of a program's `data {}`, read from its `[data.NAME]` file. */
struct DataRow {
  /** @brief When it became known. */
  std::chrono::system_clock::time_point known_at_;
  /** @brief The value's index in the program's `data {}`. */
  std::size_t name_{0};
  /** @brief The symbol it is for; empty for every instrument. */
  std::optional<std::string> symbol_;
  /** @brief The value; NaN is undefined. */
  double value_{0};
};

/**
 * @brief A data value's cell: a number, or undefined when empty or `NaN`.
 * @param cell The cell.
 * @param where The file and line, for errors.
 * @return The value.
 */
inline double data_value_of(std::string cell, const std::string& where) {
  trim(cell);
  if (cell.empty() || cell == "NaN" || cell == "nan" || cell == "NA") {
    return std::numeric_limits<double>::quiet_NaN();
  }
  char* end = nullptr;
  const double value = std::strtod(cell.c_str(), &end);
  if (end != cell.c_str() + cell.size() || !std::isfinite(value)) {
    throw std::invalid_argument(where + ": '" + cell + "' is not a number");
  }
  return value;
}

/**
 * @brief Reads every value of the program's `data {}` from its `[data.NAME]` file, in time order.
 *
 * A long file has a time column and a value column (`column`, the name by default), and per instrument a symbol
 * column; a wide file has a time column and one column per symbol. Rows at one instant keep their file's order.
 * Symbols the run does not trade are skipped; a symbol the run trades that a per-instrument file lacks is refused.
 * Values after `replay.to`'s day are dropped; values before `replay.from` are kept, as the latest known at the first bar.
 *
 * @param s The settings.
 * @param names The program's data names, in order.
 * @param symbols The run's symbols.
 * @return The values.
 */
inline std::vector<DataRow> load_data(
  const Settings& s, const std::vector<std::string>& names, const std::vector<std::string>& symbols
) {
  std::vector<DataRow> out;
  for (std::size_t k = 0; k < names.size(); ++k) {
    const auto prefix = "data." + names[k] + ".";
    const auto path = s.need(prefix + "file");
    const bool wide = s.get_or(prefix + "layout", "long") == "wide";
    const auto time_col = s.get_or(prefix + "time", "ts");
    const auto format = s.get_or(prefix + "time_format", "unix");
    const long offset = offset_of(s.get_or(prefix + "timezone", "UTC"));
    const auto symbol_col = s.get(prefix + "symbol_column");
    std::ifstream file(path);
    if (!file.is_open()) {
      throw std::runtime_error("cannot open " + path);
    }
    std::string line;
    std::getline(file, line);
    trim(line);
    const auto header = split_line(line);
    auto col = [&header, &path](const std::string& name) -> std::size_t {
      const auto it = std::find(header.begin(), header.end(), name);
      if (it == header.end()) {
        throw std::invalid_argument(path + ": no column '" + name + "'");
      }
      return static_cast<std::size_t>(it - header.begin());
    };
    const std::size_t t = col(time_col);
    // The columns read, each with its symbol (empty for a value for every instrument).
    std::vector<std::pair<std::size_t, std::optional<std::string>>> columns;
    std::optional<std::size_t> symbol_at;
    if (wide) {
      for (std::size_t c = 0; c < header.size(); ++c) {
        if (c != t && std::find(symbols.begin(), symbols.end(), header[c]) != symbols.end()) {
          columns.emplace_back(c, header[c]);
        }
      }
    } else {
      columns.emplace_back(col(s.get_or(prefix + "column", names[k])), std::nullopt);
      if (symbol_col) {
        symbol_at = col(*symbol_col);
      }
    }
    std::vector<std::string> seen;
    for (const auto& [c, symbol] : columns) {
      if (symbol) {
        seen.push_back(*symbol);
      }
    }
    std::size_t row = 1;
    while (std::getline(file, line)) {
      ++row;
      trim(line);
      if (line.empty()) {
        continue;
      }
      const auto cells = split_line(line);
      const auto where = path + ":" + std::to_string(row);
      if (cells.size() != header.size()) {
        throw std::invalid_argument(
          where + ": " + std::to_string(cells.size()) + " cells, the header has " + std::to_string(header.size())
        );
      }
      const auto at = time_of(cells[t], format, offset, where);
      std::optional<std::string> row_symbol;
      if (symbol_at) {
        auto cell = cells[*symbol_at];
        trim(cell);
        if (cell.empty()) {
          throw std::invalid_argument(where + ": no symbol");
        }
        if (std::find(symbols.begin(), symbols.end(), cell) == symbols.end()) {
          continue;
        }
        if (std::find(seen.begin(), seen.end(), cell) == seen.end()) {
          seen.push_back(cell);
        }
        row_symbol = cell;
      }
      for (const auto& [c, symbol] : columns) {
        out.push_back(DataRow{at, k, symbol ? symbol : row_symbol, data_value_of(cells[c], where)});
      }
    }
    if (wide || symbol_at) {
      for (const auto& symbol : symbols) {
        if (std::find(seen.begin(), seen.end(), symbol) == seen.end()) {
          throw std::invalid_argument(path + " has no value for " + symbol + ", which the run trades");
        }
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const DataRow& a, const DataRow& b) {
    return a.known_at_ < b.known_at_;
  });
  if (const auto to = s.get("replay.to")) {
    const auto end = epoch_plus(day_seconds(parse_date(*to)) + 86400LL, "replay.to");
    out.erase(
      std::remove_if(
        out.begin(), out.end(),
        [&end](const DataRow& r) {
          return !(r.known_at_ < end);
        }
      ),
      out.end()
    );
  }
  return out;
}

inline atl::backtest::ReplayConfig replay_config(const Settings& s, const Data& data) {
  const auto market = market_of(s);
  atl::backtest::ReplayConfig replay;
  replay.cash_ = decimal_of(s.get_or("replay.cash", market == "kr" ? "100000000" : "100000"), "replay.cash");
  replay.currency_ = atl::Currency{market == "kr" ? "KRW" : "USD"};
  for (const auto& symbol : data.symbols_) {
    replay.instruments_.push_back(instrument_of(symbol, market));
  }
  replay.session_ = session_of(market);
  replay.series_capacity_ = 512;
  replay.limits_.permitted_phases_ = std::nullopt;
  if (s.get_or("replay.shorts", "false") == "true") {
    replay.limits_.enforce_sellable_quantity_ = false;
  }
  return replay;
}

/**
 * @brief Commission, slippage and sell tax; zero costs are the library's `none()` models (a market-price rule is the
 *   driver's, `program_fills`).
 * @param s The settings.
 * @return The fill model.
 */
inline atl::backtest::NextBarFillModel fill_model(const Settings& s) {
  const auto rate = decimal_of(s.get_or("replay.commission", "0"), "replay.commission");
  const auto minimum = decimal_of(s.get_or("replay.min_commission", "0"), "replay.min_commission");
  const int ticks = static_cast<int>(whole_of(s.get_or("replay.slippage_ticks", "0"), "replay.slippage_ticks"));
  const auto zero = atl::Decimal64::parse("0");
  auto commission = rate == zero && minimum == zero ?
    atl::backtest::CommissionModel::none() :
    atl::backtest::CommissionModel::of(rate, minimum);
  auto slippage = ticks == 0 ? atl::backtest::SlippageModel::none() : atl::backtest::SlippageModel::of_ticks(ticks);
  atl::backtest::NextBarFillModel model(commission, slippage);
  const auto tax = decimal_of(s.get_or("replay.sell_tax", "0"), "replay.sell_tax");
  if (!(tax == zero)) {
    model =
      model.with_sell_tax(atl::backtest::SellTax{tax, market_of(s) == "kr" ? atl::Country::KR : atl::Country::US});
  }
  return model;
}

/**
 * @brief An instant in ISO 8601, UTC.
 * @param at The instant.
 * @return `YYYY-MM-DDTHH:MM:SSZ`.
 */
inline std::string iso(std::chrono::system_clock::time_point at) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

/**
 * @brief A number for JSON.
 * @param x The number.
 * @return Ten significant digits, or `null` when it is not finite.
 */
inline std::string number(double x) {
  if (!std::isfinite(x)) {
    return "null";
  }
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%.10g", x);
  return buffer;
}

/**
 * @brief A JSON string.
 * @param text The text.
 * @return The text quoted, with quotes, backslashes and control characters escaped.
 */
inline std::string quoted(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
      out += buffer;
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

/**
 * @brief Writes every fill as CSV: time, symbol, side, quantity, price, commission, tax.
 * @param result The replay's result.
 * @param path The file.
 */
inline void write_fills(const atl::backtest::ReplayResult& result, const std::string& path) {
  std::ofstream out(path);
  if (!out.is_open()) {
    throw std::runtime_error("cannot write " + path);
  }
  out << "at,symbol,side,quantity,price,commission,tax\n";
  for (const auto& e : result.executions_) {
    out
      << iso(e.fill_.at_) << ',' << e.fill_.instrument_.symbol_ << ','
      << (e.fill_.side_ == atl::OrderSide::BUY ? "buy" : "sell") << ',' << e.fill_.quantity_.str() << ','
      << e.fill_.price_.str() << ',' << e.commission_.str() << ',' << e.tax_.str() << '\n';
  }
  if (!out.flush()) {
    throw std::runtime_error("cannot write " + path);
  }
}

/** @brief What a program with `on_open` or `on_close` met, for the report. */
struct SessionFacts {
  /** @brief Whether the program has session handlers (nothing below is reported otherwise). */
  bool session_{false};
  /** @brief Sessions a bar opened. */
  std::size_t opened_{0};
  /** @brief Sessions a bar closed. */
  std::size_t closed_{0};
  /** @brief Bars wholly outside their session (pre-market or after-hours). */
  std::size_t outside_{0};
  /** @brief Whether each daily bar was replayed as an open point and the day (`bars.open_point`). */
  bool open_points_{false};
};

/**
 * @brief The library's report, and what the replay's result and the strategy add, as one JSON object.
 * @tparam Refusals The strategy's pricing refusals (`pricing_refusals()`'s type): the counters `no_basis_price_`,
 *   `no_price_grid_`, `price_not_representable_`, `price_not_positive_`, `net_not_representable_`, and `total()`.
 * @param result The replay's result.
 * @param r The report.
 * @param pricing The targets limit pricing did not submit.
 * @param session What a session program met; empty for any other.
 * @param reads_data Whether the program has `data {}`: the report then counts the records fed.
 * @return The JSON text, one field per line.
 */
template <typename Refusals>
std::string report_json(
  const atl::backtest::ReplayResult& result, const atl::backtest::Report& r, const Refusals& pricing,
  const SessionFacts& session = {}, bool reads_data = false
) {
  std::ostringstream o;
  auto opt = [](const auto& value) {
    return value ? quoted(value->str()) : std::string("null");
  };
  o << "{\n";
  o << "  \"bars\": " << r.bars_ << ",\n";
  if (reads_data) {
    o << "  \"records\": " << result.records_ << ",\n";
  }
  o << "  \"executions\": " << r.executions_ << ",\n";
  o << "  \"opening_equity\": " << quoted(r.opening_equity_.str()) << ",\n";
  o << "  \"closing_equity\": " << quoted(r.closing_equity_.str()) << ",\n";
  o << "  \"total_return\": " << number(r.total_return_) << ",\n";
  o << "  \"sharpe_per_period\": " << number(r.sharpe_) << ",\n";
  o << "  \"sharpe_annualized\": " << number(r.annualized_sharpe_) << ",\n";
  o << "  \"sortino_per_period\": " << number(r.sortino_) << ",\n";
  o << "  \"max_drawdown\": " << number(r.max_drawdown_) << ",\n";
  o << "  \"turnover\": " << number(r.total_turnover_) << ",\n";
  o << "  \"commission_paid\": " << opt(r.commission_paid_) << ",\n";
  o << "  \"slippage_paid\": " << opt(r.slippage_paid_) << ",\n";
  o << "  \"tax_paid\": " << opt(r.tax_paid_) << ",\n";
  o << "  \"rejected\": " << r.rejected_ << ",\n";
  o << "  \"refused\": " << r.refused_ << ",\n";
  o << "  \"halted\": " << (r.halted_ ? quoted(r.halted_->broker_code_ + ": " + r.halted_->message_) : "null") << ",\n";
  o << "  \"aborted\": " << (r.aborted_ ? "true" : "false") << ",\n";
  std::map<std::string, std::size_t> reasons;
  for (const auto& rejection : result.rejected_) {
    ++reasons[std::string(atl::strategy::reject_reason_name(rejection.reason_))];
  }
  for (const auto& refusal : result.refused_) {
    ++reasons["guard:" + std::string(atl::strategy::guard_reason_name(refusal.reason_))];
  }
  o << "  \"rejection_reasons\": {";
  bool first = true;
  for (const auto& reason : reasons) {
    o << (first ? "" : ", ") << quoted(reason.first) << ": " << reason.second;
    first = false;
  }
  o << "},\n";
  o << "  \"pricing_refused\": " << pricing.total() << ",\n";
  o << "  \"pricing_refusal_reasons\": {";
  first = true;
  for (
    const auto& [name, count] : {
      std::pair<const char*, std::size_t>{"no_basis_price", pricing.no_basis_price_},
      std::pair<const char*, std::size_t>{"no_price_grid", pricing.no_price_grid_},
      std::pair<const char*, std::size_t>{"price_not_representable", pricing.price_not_representable_},
      std::pair<const char*, std::size_t>{"price_not_positive", pricing.price_not_positive_},
      std::pair<const char*, std::size_t>{"net_not_representable", pricing.net_not_representable_},
    }
  ) {
    if (count != 0) {
      o << (first ? "" : ", ") << quoted(name) << ": " << count;
      first = false;
    }
  }
  o << "},\n";
  const auto& c = r.caveats_;
  o << "  \"caveats\": {\"gaps\": " << c.gaps_ << ", \"unfilled\": " << c.unfilled_
    << ", \"expired\": " << c.expired_day_orders_ << ", \"unvalued_bars\": " << c.unvalued_bars_
    << ", \"multiple_instruments\": " << (c.multiple_instruments_ ? "true" : "false")
    << ", \"cost_totals_incomplete\": " << (c.cost_totals_incomplete_ ? "true" : "false")
    << ", \"volume_ignored\": " << (atl::backtest::ReportCaveats::volume_ignored ? "true" : "false")
    << ", \"lowest_cash\": " << (result.lowest_cash_ ? quoted(result.lowest_cash_->amount_.str()) : "null")
    << ", \"shorts_borrow_nothing\": true";
  if (!c.market_price_rule_.empty()) {
    o << ", \"market_price_rule\": " << quoted(c.market_price_rule_)
      << ", \"market_fills_by_rule\": " << c.market_fills_by_rule_;
  }
  if (session.session_) {
    o << ", \"sessions_opened\": " << session.opened_ << ", \"sessions_closed\": " << session.closed_
      << ", \"bars_outside_session\": " << session.outside_;
  }
  if (session.open_points_) {
    o << ", \"open_points\": true";
  }
  o << "}";
  if (!result.equity_.empty()) {
    o << ",\n  \"first_bar\": " << quoted(iso(result.equity_.front().at_));
    o << ",\n  \"last_bar\": " << quoted(iso(result.equity_.back().at_));
  }
  o << "\n}\n";
  return o.str();
}

/**
 * @brief Runs a compiled strategy from `key=value` settings: loads the data, sets the parameters
 *   (`param.NAME=VALUE`) and then the order pricing (`orders.type`, `orders.offset`), replays, prints the report as
 *   JSON on standard output and writes the fills to `output.fills` when given.
 * @tparam Strategy A strategy generated by `atlc`; of its interface the replay uses `atls_mode`, `atls_session`,
 *   `atls_instruments()`, the constructor from the instruments, `set`, `set_order_pricing` and `pricing_refusals()`.
 * @tparam Fills A callable taking the settings' fill model and returning the one the replay uses.
 * @param argc The argument count.
 * @param argv The settings.
 * @param fills Gives the fill model the run's market-price rule, if it has one (the driver's `program_fills`).
 * @return 0, or 1 after printing the error on standard error.
 */
template <typename Strategy, typename Fills>
int main(int argc, char** argv, Fills fills_of) {
  try {
    const auto s = Settings::from_args(argc, argv);
    const std::string mode = Strategy::atls_mode;
    const auto market = market_of(s);
    const auto symbols = symbols_of<Strategy>(s);
    const bool open_point = s.get_or("bars.open_point", "false") == "true";
    if (open_point && !Strategy::atls_session) {
      throw std::invalid_argument("bars.open_point is for a program with on_open/on_close");
    }
    const auto data = load(s, symbols);
    if (mode == "single" && data.symbols_.size() != 1) {
      throw std::invalid_argument(
        "a single-instrument program trades one symbol, and the data holds " + std::to_string(data.symbols_.size()) +
        ": name it in [instruments]"
      );
    }
    const bool wide = data.series_.empty() && !data.close_.names_.empty();
    const bool daily = wide || std::all_of(data.series_.begin(), data.series_.end(), [](const BarSeries& x) {
      return x.interval_ == atl::BarInterval::ONE_DAY;
    });
    if (Strategy::atls_session && daily && !open_point) {
      throw std::invalid_argument(
        "the program has on_open/on_close and a daily bar is a whole session: set bars.open_point = true, or give "
        "intraday bars"
      );
    }
    const auto feed = wide ? events(data, market, open_point) : bar_events(data, market, open_point);
    const auto config = replay_config(s, data);
    const auto fills = fills_of(fill_model(s));
    std::vector<atl::InstrumentId> ids;
    for (const auto& symbol : data.symbols_) {
      ids.push_back(instrument_of(symbol, market).id_);
    }
    Strategy strategy(ids);
    set_params(strategy, s);
    // After the final parameter values, though `set` keeps the pricing.
    set_pricing(strategy, s);
    double periods = 252;
    if (const auto p = s.get("replay.periods_per_year")) {
      periods = std::stod(*p);
    }
    atl::backtest::Backtester<Strategy> engine(std::move(strategy), config, fills);
    if constexpr (reads_data<Strategy>::value) {
      // Records and bars in time order, a record before the bars of its instant.
      const auto rows = load_data(s, Strategy::atls_data(), data.symbols_);
      std::size_t next = 0;
      const auto feed_rows_until = [&](std::optional<std::chrono::system_clock::time_point> until) {
        for (; next < rows.size() && (!until || !(*until < rows[next].known_at_)); ++next) {
          typename Strategy::data_type record;
          record.name_ = rows[next].name_;
          if (rows[next].symbol_) {
            record.instrument_ = instrument_of(*rows[next].symbol_, market).id_;
          }
          record.value_ = rows[next].value_;
          engine.feed(atl::strategy::DataEvent<typename Strategy::data_type>{rows[next].known_at_, record});
        }
      };
      for (const auto& event : feed) {
        feed_rows_until(event.period_end_);
        engine.feed(event);
      }
      // Values after the last bar but inside the replay are fed too: nothing decides on them, and the count says so.
      feed_rows_until(std::nullopt);
    } else {
      for (const auto& event : feed) {
        engine.feed(event);
      }
    }
    engine.finish();
    const auto result = engine.result();
    const auto report = atl::backtest::summarize(result, engine.fills(), periods);
    SessionFacts session;
    session.open_points_ = open_point;
    if constexpr (Strategy::atls_session) {
      session.session_ = true;
      session.opened_ = engine.strategy().session_opens();
      session.closed_ = engine.strategy().session_closes();
      session.outside_ = engine.strategy().session_outside_bars();
      // A session program that never met the edge its handlers run on traded on nothing it was written for.
      const bool no_open = Strategy::atls_on_open && session.opened_ == 0;
      const bool no_close = Strategy::atls_on_close && session.closed_ == 0;
      if (!engine.result().halted_ && (no_open || no_close)) {
        throw std::invalid_argument(
          std::string("the program's ") + (no_open ? "on_open" : "on_close") + " never ran: no bar " +
          (no_open ? "opened" : "closed") + " a session of the market `" + market +
          "` (check replay.market and the bars' interval and times)"
        );
      }
    }
    // After the session check, so a failed replay leaves no fills file behind.
    if (const auto path = s.get("output.fills")) {
      write_fills(result, *path);
    }
    std::cout << report_json(
      result, report, engine.strategy().pricing_refusals(), session, reads_data<Strategy>::value
    );
    std::cout.flush();
    return std::cout ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}

/**
 * @brief Runs a compiled strategy from `key=value` settings with the settings' fill model as it is (market orders at the
 *   next bar's open); see the overload taking `fills`.
 * @tparam Strategy A strategy generated by `atlc`.
 * @param argc The argument count.
 * @param argv The settings.
 * @return 0, or 1 after printing the error on standard error.
 */
template <typename Strategy>
int main(int argc, char** argv) {
  return main<Strategy>(argc, argv, [](atl::backtest::NextBarFillModel model) {
    return model;
  });
}

} // namespace replay
} // namespace cmdatl
