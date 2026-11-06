#pragma once

// Run settings: what a program is replayed or run on, read from an optional TOML file (the run spec) and from
// `KEY=VALUE` words on the command line, checked key by key and then as a whole against the program's facts
// (`atlc check FILE --info`), and flattened to the `key=value` arguments the replay module (`replay/`) reads.
//
// The file is read by toml++; every number keeps the text it was written with (re-read from the file at the value's
// position), so a rate such as 0.00015 reaches the replay as an exact decimal. A command-line value is typed by its
// key, never read as TOML: `instruments.stock=005930` is the string 005930.

#include "../account/operator.hpp"
#include "../core/reply.hpp"
#include "../core/table.hpp"
#include "../system/files.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#endif

namespace cmdatl {
namespace spec {

/** @brief The largest run spec read, in bytes. */
inline constexpr std::size_t MAX_SPEC_BYTES = 1u << 20;
/** @brief The most `key=value` arguments a run or a backtest passes to its driver. */
inline constexpr std::size_t MAX_ARGS = 1000;
/** @brief The longest argument value passed to a driver. */
inline constexpr std::size_t MAX_VALUE = 4096;

/** @brief What a key holds. */
enum class Kind {
  /** @brief A file or directory, relative to the file that sets it, or to the working directory. */
  PATH,
  /** @brief A list of paths (a single string is a list of one). */
  PATHS,
  /** @brief A string. */
  TEXT,
  /** @brief A list of strings (a single string is a list of one). */
  TEXTS,
  /** @brief One of the key's choices. */
  CHOICE,
  /** @brief A number at least the key's minimum (above it when strict). */
  NUMBER,
  /** @brief A whole number, 0 to 1e9. */
  COUNT,
  /** @brief `true` or `false`. */
  BOOL,
  /** @brief A date: `"YYYY-MM-DD"`, or yyyymmdd as a number or a string. */
  DATE,
  /** @brief An instant: `"YYYY-MM-DD"`, optionally with `THH:MM[:SS]` and an offset. */
  INSTANT,
  /** @brief A fixed offset from UTC: `"UTC"`, `"Z"` or `"+HH:MM"`. */
  OFFSET,
  /** @brief Store coordinates, `"SOURCE/INTERVAL/SYMBOL"`, one or a list. */
  COORDINATES,
  /** @brief A limit's offset: a plain decimal at least 0 and below 0.3, of at most 18 fractional digits. */
  FRACTION,
};

/** @brief One documented key. */
struct Key {
  /** @brief The flattened name, `table.key`. */
  std::string name_;
  /** @brief What it holds. */
  Kind kind_;
  /** @brief The data sources it applies to; empty for any. */
  std::vector<std::string> sources_;
  /** @brief A choice key's choices. */
  std::vector<std::string> choices_;
  /** @brief A number's least value. */
  double min_{0};
  /** @brief Whether a number must be above `min_`. */
  bool strict_{false};
};

/** @brief The bar intervals the readers know. */
inline const std::vector<std::string> INTERVALS{"1m", "5m", "15m", "30m", "1h", "1d", "1w", "1mo"};

/**
 * @brief Every key the settings take besides `instruments.NAME` and `params.NAME`; anything else is refused.
 * @return The keys.
 */
inline const std::vector<Key>& keys() {
  static const std::vector<std::string> csv{"csv"};
  static const std::vector<std::string> wide{"wide"};
  static const std::vector<std::string> store{"store"};
  static const std::vector<Key> table{
    {"strategy", Kind::PATH, {}, {}},
    {"bars.source", Kind::CHOICE, {}, {"wide", "csv", "store"}},
    {"bars.file", Kind::PATH, {"wide", "csv"}, {}},
    {"bars.close", Kind::PATH, wide, {}},
    {"bars.open", Kind::PATH, wide, {}},
    {"bars.high", Kind::PATH, wide, {}},
    {"bars.low", Kind::PATH, wide, {}},
    {"bars.names", Kind::PATH, wide, {}},
    {"bars.files", Kind::PATHS, csv, {}},
    {"bars.symbol", Kind::TEXT, csv, {}},
    {"bars.symbols", Kind::TEXTS, csv, {}},
    {"bars.symbol_column", Kind::TEXT, csv, {}},
    {"bars.layout", Kind::CHOICE, csv, {"long", "wide"}},
    {"bars.time", Kind::TEXT, csv, {}},
    {"bars.time_format", Kind::CHOICE, csv, {"unix", "unix_ms", "yyyymmdd", "iso"}},
    {"bars.timezone", Kind::OFFSET, csv, {}},
    {"bars.stamp", Kind::TEXT, csv, {}},
    {"bars.interval", Kind::CHOICE, csv, INTERVALS},
    {"bars.columns.open", Kind::TEXT, csv, {}},
    {"bars.columns.high", Kind::TEXT, csv, {}},
    {"bars.columns.low", Kind::TEXT, csv, {}},
    {"bars.columns.close", Kind::TEXT, csv, {}},
    {"bars.columns.volume", Kind::TEXT, csv, {}},
    {"bars.store", Kind::COORDINATES, store, {}},
    {"bars.store_root", Kind::PATH, store, {}},
    {"bars.as_of", Kind::INSTANT, store, {}},
    {"bars.open_point", Kind::BOOL, {}, {}},
    {"replay.from", Kind::DATE, {}, {}},
    {"replay.to", Kind::DATE, {}, {}},
    {"replay.market", Kind::CHOICE, {}, {"us", "kr"}},
    {"replay.cash", Kind::NUMBER, {}, {}, 0, true},
    {"replay.commission", Kind::NUMBER, {}, {}, 0, false},
    {"replay.min_commission", Kind::NUMBER, {}, {}, 0, false},
    {"replay.sell_tax", Kind::NUMBER, {}, {}, 0, false},
    {"replay.slippage_ticks", Kind::COUNT, {}, {}},
    {"replay.fill", Kind::CHOICE, {}, {"next-open", "decision-close"}},
    {"replay.fill_header", Kind::PATH, {}, {}},
    {"replay.fill_function", Kind::TEXT, {}, {}},
    {"replay.shorts", Kind::BOOL, {}, {}},
    {"replay.periods_per_year", Kind::NUMBER, {}, {}, 0, true},
    {"orders.type", Kind::CHOICE, {}, {"market", "limit"}},
    {"orders.offset", Kind::FRACTION, {}, {}},
    {"build.duckdb", Kind::PATH, {}, {}},
    {"build.indicators", Kind::CHOICE, {}, {"incremental", "naive", "exact"}},
  };
  return table;
}

/**
 * @brief The keys of one `[data.NAME]` table, which maps a value of the program's `data {}` to a file.
 * @return The keys, named without their `data.NAME.` prefix.
 */
inline const std::vector<Key>& data_keys() {
  static const std::vector<Key> table{
    {"file", Kind::PATH, {}, {}},
    {"layout", Kind::CHOICE, {}, {"long", "wide"}},
    {"column", Kind::TEXT, {}, {}},
    {"symbol_column", Kind::TEXT, {}, {}},
    {"time", Kind::TEXT, {}, {}},
    {"time_format", Kind::CHOICE, {}, {"unix", "unix_ms", "yyyymmdd", "iso"}},
    {"timezone", Kind::OFFSET, {}, {}},
  };
  return table;
}

inline bool bare(std::string_view text);

/**
 * @brief Splits `data.NAME.KEY` into its data name and key.
 * @param name The flattened name.
 * @return NAME and KEY, or nothing when `name` has another shape or NAME is not bare.
 */
inline std::optional<std::pair<std::string, std::string>> data_name_key(const std::string& name) {
  if (name.rfind("data.", 0) != 0) {
    return std::nullopt;
  }
  const auto rest = name.substr(5);
  const auto dot = rest.find('.');
  if (dot == std::string::npos || !bare(rest.substr(0, dot)) || rest.find('.', dot + 1) != std::string::npos) {
    return std::nullopt;
  }
  return std::make_pair(rest.substr(0, dot), rest.substr(dot + 1));
}

/**
 * @brief The documented key named `name`: one of `keys()`, or `data.NAME.KEY` with KEY one of `data_keys()`.
 * @param name The name.
 * @return The key, or null.
 */
inline const Key* known(const std::string& name) {
  if (const auto split = data_name_key(name)) {
    for (const auto& key : data_keys()) {
      if (key.name_ == split->second) {
        return &key;
      }
    }
    return nullptr;
  }
  for (const auto& key : keys()) {
    if (key.name_ == name) {
      return &key;
    }
  }
  return nullptr;
}

/** @brief A value as written. */
struct Value {
  /** @brief What it is. */
  enum class Type {
    STR,
    NUM,
    BOOL,
    LIST,
  };
  /** @brief What it is. */
  Type type_{Type::STR};
  /** @brief A string, or a number's text (without underscores or a leading `+`): the text is what the replay reads. */
  std::string text_;
  /** @brief A number's value. */
  double number_{0};
  /** @brief A boolean. */
  bool boolean_{false};
  /** @brief A list's items, strings or numbers. */
  std::vector<Value> items_;

  /**
   * @brief A string.
   * @param text The string.
   * @return The value.
   */
  static Value str(std::string text) {
    Value v;
    v.text_ = std::move(text);
    return v;
  }

  /**
   * @brief A number.
   * @param text Its text.
   * @param number Its value.
   * @return The value.
   */
  static Value num(std::string text, double number) {
    Value v;
    v.type_ = Type::NUM;
    v.text_ = std::move(text);
    v.number_ = number;
    return v;
  }

  /**
   * @brief A boolean.
   * @param b The boolean.
   * @return The value.
   */
  static Value boolean(bool b) {
    Value v;
    v.type_ = Type::BOOL;
    v.boolean_ = b;
    return v;
  }

  /**
   * @brief A list.
   * @param items The items.
   * @return The value.
   */
  static Value list(std::vector<Value> items) {
    Value v;
    v.type_ = Type::LIST;
    v.items_ = std::move(items);
    return v;
  }

  /**
   * @brief The value as the replay reads it: a list's items joined by commas.
   * @return The text.
   */
  std::string text() const {
    switch (type_) {
      case Type::STR:
      case Type::NUM:
        return text_;
      case Type::BOOL:
        return boolean_ ? "true" : "false";
      case Type::LIST:
        break;
    }
    std::string out;
    for (const auto& item : items_) {
      out += (out.empty() ? "" : ",") + item.text();
    }
    return out;
  }

  /**
   * @brief The value for a message: a string in quotes, a list in brackets.
   * @return The text.
   */
  std::string describe() const {
    if (type_ == Type::STR) {
      return "\"" + text_ + "\"";
    }
    if (type_ == Type::LIST) {
      return "[" + text() + "]";
    }
    return text();
  }
};

/** @brief Where a setting came from: a file and a line, or the command line (`file_` empty). */
struct Origin {
  /** @brief The file, absolute, or empty for the command line. */
  std::string file_;
  /** @brief The line, from 1. */
  std::size_t line_{0};

  /**
   * @brief Whether the setting came from the command line.
   * @return Whether it did.
   */
  bool command_line() const {
    return file_.empty();
  }

  /**
   * @brief The place, for a message: `FILE:LINE` or `the command line`.
   * @return The text.
   */
  std::string text() const {
    return command_line() ? std::string("the command line") : file_ + ":" + std::to_string(line_);
  }
};

/** @brief One setting. */
struct Entry {
  /** @brief The flattened key, `table.key`. */
  std::string key_;
  /** @brief The value. */
  Value value_;
  /** @brief Where it was given. */
  Origin origin_;
};

/** @brief A settings error, with its place kept as data. */
struct Error {
  /** @brief Where it is. */
  enum class At {
    /** @brief A line of a file. */
    LINE,
    /** @brief A file as a whole. */
    FILE,
    /** @brief A setting on the command line. */
    COMMAND_LINE,
    /** @brief No one place: something no setting gives. */
    NOWHERE,
  };
  /** @brief `spec` or `unreadable`. */
  std::string code_{"spec"};
  /** @brief Where it is. */
  At at_{At::NOWHERE};
  /** @brief The file, for `LINE` and `FILE`. */
  std::string file_;
  /** @brief The line, for `LINE`. */
  std::size_t line_{0};
  /** @brief What is wrong. */
  std::string message_;

  /**
   * @brief An error about a setting, at the place it was given.
   * @param origin Where it was given.
   * @param message What is wrong.
   * @return The error.
   */
  static Error at(const Origin& origin, std::string message) {
    Error e;
    e.at_ = origin.command_line() ? At::COMMAND_LINE : At::LINE;
    e.file_ = origin.file_;
    e.line_ = origin.line_;
    e.message_ = std::move(message);
    return e;
  }

  /**
   * @brief An error with no one place.
   * @param message What is wrong.
   * @return The error.
   */
  static Error nowhere(std::string message) {
    Error e;
    e.message_ = std::move(message);
    return e;
  }

  /**
   * @brief An error about a file as a whole.
   * @param file The file.
   * @param message What is wrong.
   * @param code `spec` or `unreadable`.
   * @return The error.
   */
  static Error file(std::string file, std::string message, std::string code = "spec") {
    Error e;
    e.code_ = std::move(code);
    e.at_ = At::FILE;
    e.file_ = std::move(file);
    e.message_ = std::move(message);
    return e;
  }

  /**
   * @brief The error as a person reads it: the place, then the message.
   * @return The text.
   */
  std::string text() const {
    switch (at_) {
      case At::LINE:
        return file_ + ":" + std::to_string(line_) + ": " + message_;
      case At::FILE:
        return file_ + ": " + message_;
      case At::COMMAND_LINE:
        return "the command line: " + message_;
      case At::NOWHERE:
        break;
    }
    return message_;
  }

  /**
   * @brief The error as one diagnostic: `severity`, `code`, `message`, `file` and `line` (null without a place), and
   *   `origin` (`file` or `command_line`).
   * @return The object.
   */
  json::object to_json() const {
    json::object out{{"severity", "error"}, {"code", code_}, {"message", message_}};
    out["file"] = at_ == At::LINE || at_ == At::FILE ? json::value(file_) : json::value(nullptr);
    out["line"] = at_ == At::LINE ? json::value(static_cast<std::int64_t>(line_)) : json::value(nullptr);
    out["origin"] = at_ == At::COMMAND_LINE ? "command_line" : "file";
    return out;
  }
};

/**
 * @brief Whether a text is a bare key's part: 1 or more ASCII letters, digits, `_` or `-`.
 * @param text The text.
 * @return Whether it is.
 */
inline bool bare(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

/**
 * @brief Whether a command-line word is a setting: the text before its first `=` is a dotted key (bare parts joined by
 *   dots, at least one dot).
 * @param word The word.
 * @return Whether it is.
 */
inline bool is_setting(const std::string& word) {
  const auto eq = word.find('=');
  if (eq == std::string::npos) {
    return false;
  }
  const auto key = std::string_view(word).substr(0, eq);
  if (key.find('.') == std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (true) {
    const auto dot = key.find('.', start);
    if (!bare(key.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start))) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    start = dot + 1;
  }
}

/**
 * @brief Whether a text is an argument key a driver takes: 1 to 128 of `A-Z a-z 0-9 _ . -`.
 * @param key The text.
 * @return Whether it is.
 */
inline bool valid_key(const std::string& key) {
  if (key.empty() || key.size() > 128) {
    return false;
  }
  return std::all_of(key.begin(), key.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
      c == '-';
  });
}

/**
 * @brief Whether a text is a struct atlc can have named a program: `[A-Z][A-Za-z0-9]{0,63}`, neither `Params` nor `Lane`.
 *   It is pasted into the C++ cmdatl writes, so nothing else passes.
 * @param name The text.
 * @return Whether it is.
 */
inline bool valid_struct(const std::string& name) {
  if (name.empty() || name.size() > 64 || name[0] < 'A' || name[0] > 'Z' || name == "Params" || name == "Lane") {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  });
}

/**
 * @brief The first control character of a text (U+0000 to U+001F, U+007F), if it holds one.
 * @param text The text, UTF-8.
 * @return Its code.
 */
inline std::optional<unsigned> control_character(std::string_view text) {
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return byte;
    }
  }
  return std::nullopt;
}

/**
 * @brief Whether a text is valid UTF-8: no stray continuation byte, no truncated or overlong sequence, no surrogate,
 *   nothing past U+10FFFF.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_utf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto byte = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    std::uint32_t code = 0;
    if (byte < 0x80) {
      ++i;
      continue;
    } else if ((byte & 0xE0u) == 0xC0u) {
      length = 2;
      code = byte & 0x1Fu;
    } else if ((byte & 0xF0u) == 0xE0u) {
      length = 3;
      code = byte & 0x0Fu;
    } else if ((byte & 0xF8u) == 0xF0u) {
      length = 4;
      code = byte & 0x07u;
    } else {
      return false;
    }
    if (i + length > text.size()) {
      return false;
    }
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0u) != 0x80u) {
        return false;
      }
      code = (code << 6) | (next & 0x3Fu);
    }
    const std::uint32_t least = length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u;
    if (code < least || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
      return false;
    }
    i += length;
  }
  return true;
}

/**
 * @brief The refusal of a string holding a control character.
 * @param key The setting.
 * @param code The character.
 * @return The message.
 */
inline std::string control_message(const std::string& key, unsigned code) {
  char hex[8];
  std::snprintf(hex, sizeof hex, "%04X", code);
  return "`" + key + "` holds a control character (U+" + hex +
    "); a backslash in a double-quoted string is an escape: write a path in single quotes or with forward slashes";
}

/**
 * @brief A TOML decimal integer or float (underscores between digits, no leading zeros, an optional sign and
 *   exponent), as its text without underscores or a leading `+`, and its value; `inf`, `nan` and hexadecimal, octal
 *   or binary integers are not decimals.
 * @param word The text.
 * @return The number, or nothing.
 */
inline std::optional<Value> decimal(std::string_view word) {
  auto body = word;
  if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
    body.remove_prefix(1);
  }
  const auto e = body.find_first_of("eE");
  const auto mantissa = body.substr(0, e);
  const auto exponent = e == std::string_view::npos ? std::optional<std::string_view>() : body.substr(e + 1);
  const auto point = mantissa.find('.');
  const auto whole = mantissa.substr(0, point);
  const auto fraction =
    point == std::string_view::npos ? std::optional<std::string_view>() : mantissa.substr(point + 1);
  const auto digits = [](std::string_view s) {
    return !s.empty() && s.front() != '_' && s.back() != '_' && s.find("__") == std::string_view::npos &&
      std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '_';
      });
  };
  if (!digits(whole) || (whole.size() > 1 && whole.front() == '0')) {
    return std::nullopt;
  }
  if (fraction && !digits(*fraction)) {
    return std::nullopt;
  }
  if (exponent) {
    auto power = *exponent;
    if (!power.empty() && (power.front() == '+' || power.front() == '-')) {
      power.remove_prefix(1);
    }
    if (!digits(power)) {
      return std::nullopt;
    }
  }
  std::string text;
  for (std::size_t k = word.front() == '+' ? 1 : 0; k < word.size(); ++k) {
    if (word[k] != '_') {
      text.push_back(word[k]);
    }
  }
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return Value::num(std::move(text), value);
}

/**
 * @brief A decimal number's text as plain decimal text with the same value (`1.5e-4` is `0.00015`, `1e5` is
 *   `100000`), computed on the digits, never through a float.
 * @param text The text, as `decimal` gives it.
 * @return The plain text, or nothing past 18 significant digits or 255 decimal places.
 */
inline std::optional<std::string> plain_decimal(const std::string& text) {
  std::string sign;
  std::string body = text;
  if (!body.empty() && body.front() == '-') {
    sign = "-";
    body.erase(0, 1);
  }
  std::string mantissa = body;
  long long exponent = 0;
  const auto e = body.find_first_of("eE");
  if (e != std::string::npos) {
    mantissa = body.substr(0, e);
    std::string power = body.substr(e + 1);
    bool negative = false;
    if (!power.empty() && (power.front() == '+' || power.front() == '-')) {
      negative = power.front() == '-';
      power.erase(0, 1);
    }
    if (power.empty()) {
      return std::nullopt;
    }
    for (const char c : power) {
      if (c < '0' || c > '9' || exponent > (std::numeric_limits<long long>::max() - 9) / 10) {
        return std::nullopt;
      }
      exponent = exponent * 10 + (c - '0');
    }
    exponent = negative ? -exponent : exponent;
  }
  const auto point = mantissa.find('.');
  const std::string whole = mantissa.substr(0, point);
  const std::string fraction = point == std::string::npos ? std::string() : mantissa.substr(point + 1);
  // The value is `digits * 10^shift`.
  std::string digits = whole + fraction;
  if (exponent < std::numeric_limits<long long>::min() + static_cast<long long>(fraction.size())) {
    return std::nullopt;
  }
  long long shift = exponent - static_cast<long long>(fraction.size());
  while (digits.size() > 1 && digits.front() == '0') {
    digits.erase(0, 1);
  }
  while (shift < 0 && digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    ++shift;
  }
  if (digits == "0") {
    return std::string("0");
  }
  const auto significant = digits.find_last_not_of('0') + 1;
  if (
    significant > 18 || shift > 18 || static_cast<long long>(digits.size()) + std::max(shift, 0LL) > 18 || shift < -255
  ) {
    return std::nullopt;
  }
  std::string plain;
  if (shift >= 0) {
    plain = digits + std::string(static_cast<std::size_t>(shift), '0');
  } else {
    const auto places = static_cast<std::size_t>(-shift);
    const auto padded = std::string(places + 1 > digits.size() ? places + 1 - digits.size() : 0, '0') + digits;
    plain = padded.substr(0, padded.size() - places) + "." + padded.substr(padded.size() - places);
  }
  return sign + plain;
}

/**
 * @brief Whether a number was written as an integer: TOML reads `2.0` as a float, so an integer takes no `.` or
 *   exponent.
 * @param text The number's text.
 * @return Whether it was.
 */
inline bool integer_text(const std::string& text) {
  return text.find_first_of(".eE") == std::string::npos;
}

/**
 * @brief A yyyymmdd date that exists, from 1970.
 * @param digits The text.
 * @return The date as a number, or nothing.
 */
inline std::optional<std::uint32_t> date_of(const std::string& digits) {
  const auto is_digit = [](char c) {
    return c >= '0' && c <= '9';
  };
  if (digits.size() != 8 || !std::all_of(digits.begin(), digits.end(), is_digit)) {
    return std::nullopt;
  }
  const auto d = static_cast<std::uint32_t>(std::stoul(digits));
  const auto y = d / 10000;
  const auto m = d / 100 % 100;
  const auto day = d % 100;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  std::uint32_t days = 0;
  switch (m) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      days = 31;
      break;
    case 4:
    case 6:
    case 9:
    case 11:
      days = 30;
      break;
    case 2:
      days = leap ? 29 : 28;
      break;
    default:
      return std::nullopt;
  }
  if (y < 1970 || day < 1 || day > days) {
    return std::nullopt;
  }
  return d;
}

/**
 * @brief Whether a text is a fixed offset: `UTC`, `Z`, or `+HH:MM` / `-HH:MM` (hours to 14).
 * @param s The text.
 * @return Whether it is.
 */
inline bool offset_text(const std::string& s) {
  if (s == "UTC" || s == "Z") {
    return true;
  }
  if (s.size() != 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':') {
    return false;
  }
  const auto digit = [](char c) {
    return c >= '0' && c <= '9';
  };
  if (!digit(s[1]) || !digit(s[2]) || !digit(s[4]) || !digit(s[5])) {
    return false;
  }
  return (s[1] - '0') * 10 + (s[2] - '0') <= 14 && (s[4] - '0') * 10 + (s[5] - '0') <= 59;
}

/**
 * @brief Whether a text is an instant: `YYYY-MM-DD`, optionally `THH:MM[:SS]` (or a space for the `T`) and `Z` or
 *   `+HH:MM`.
 * @param s The text.
 * @return Whether it is.
 */
inline bool instant_text(const std::string& s) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  std::string digits = s.substr(0, 4) + s.substr(5, 2) + s.substr(8, 2);
  if (!date_of(digits)) {
    return false;
  }
  if (s.size() == 10) {
    return true;
  }
  if (s[10] != 'T' && s[10] != ' ') {
    return false;
  }
  const auto time = s.substr(11);
  const auto zone_at = time.find_first_of("Z+-");
  const auto clock = time.substr(0, zone_at);
  const auto zone = zone_at == std::string::npos ? std::string() : time.substr(zone_at);
  const auto two = [](const std::string& field, int max) {
    return field.size() == 2 && field[0] >= '0' && field[0] <= '9' && field[1] >= '0' && field[1] <= '9' &&
      (field[0] - '0') * 10 + (field[1] - '0') <= max;
  };
  const auto fields = split(clock, ':');
  const bool clock_ok = (fields.size() == 2 || fields.size() == 3) && two(fields[0], 23) && two(fields[1], 59) &&
    (fields.size() == 2 || two(fields[2], 59));
  return clock_ok && (zone.empty() || offset_text(zone));
}

/**
 * @brief A path made absolute against a directory, as written otherwise.
 * @param path The path.
 * @param base The directory, absolute.
 * @return The path.
 */
inline std::string absolute_against(const std::string& path, const std::string& base) {
  if (!path.empty() && path.front() == '/') {
    return path;
  }
  return (std::filesystem::path(base) / path).string();
}

/**
 * @brief The directory a setting's relative paths are read against: its file's, or the working directory.
 * @param origin Where it was given.
 * @return The directory.
 */
inline std::string base_of(const Origin& origin) {
  if (!origin.command_line()) {
    return std::filesystem::path(origin.file_).parent_path().string();
  }
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  return ec ? std::string() : cwd.string();
}

/**
 * @brief Checks one entry's key and value, making a path absolute against its base.
 * @param e The entry.
 * @return The error, or nothing.
 */
inline std::optional<Error> check(Entry& e) {
  const auto& at = e.origin_;
  const auto bad = [&e](const std::string& what) {
    return Error::at(e.origin_, "`" + e.key_ + "` is " + what + ", not " + e.value_.describe());
  };
  // A string must be UTF-8 (a command-line word need not be, and it is written into run.json) and hold no control
  // character.
  const auto refusal = [&e](const Value& v) -> std::optional<Error> {
    if (v.type_ != Value::Type::STR) {
      return std::nullopt;
    }
    if (!valid_utf8(v.text_)) {
      return Error::at(e.origin_, "`" + e.key_ + "` holds bytes that are not UTF-8");
    }
    if (const auto code = control_character(v.text_)) {
      return Error::at(e.origin_, control_message(e.key_, *code));
    }
    return std::nullopt;
  };
  const auto controlled = [&refusal](const Value& v) -> std::optional<Error> {
    if (auto refused = refusal(v)) {
      return refused;
    }
    for (const auto& item : v.items_) {
      if (auto refused = refusal(item)) {
        return refused;
      }
    }
    return std::nullopt;
  };
  if (auto refused = controlled(e.value_)) {
    return refused;
  }
  Kind kind = Kind::TEXT;
  const bool instrument = e.key_.rfind("instruments.", 0) == 0;
  const bool parameter = e.key_.rfind("params.", 0) == 0;
  if (instrument || parameter) {
    const auto name = e.key_.substr(e.key_.find('.') + 1);
    if (name.find('.') != std::string::npos || !bare(name)) {
      return Error::at(at, "unknown setting `" + e.key_ + "`");
    }
    if (parameter) {
      if (e.value_.type_ == Value::Type::NUM || e.value_.type_ == Value::Type::BOOL) {
        return std::nullopt;
      }
      return Error::at(at, "`" + e.key_ + "` is a number or a boolean, not " + e.value_.describe());
    }
  } else if (e.key_ == "build.algotradelib") {
    return Error::at(
      at,
      "`build.algotradelib` is not a setting: cmdatl builds against the algotradelib it was built with (cmdatl env "
      "shows it)"
    );
  } else if (e.key_.rfind("data.", 0) == 0 && known("bars." + e.key_.substr(5)) != nullptr) {
    // The bar keys were `data.*` until [data] was renamed [bars]; an old spec is refused, never read from either.
    return Error::at(at, "`" + e.key_ + "` moved to `bars." + e.key_.substr(5) + "`: [data] is now [bars]");
  } else if (const auto* key = known(e.key_)) {
    kind = key->kind_;
  } else {
    const auto table = e.key_.substr(0, e.key_.find('.'));
    std::string hint;
    if (e.key_.find('.') == std::string::npos || table == "bars" || table == "replay") {
      hint = "";
    } else if (table == "data") {
      hint = " ([data.NAME] takes file, layout, column, symbol_column, time, time_format and timezone)";
    } else if (table == "build") {
      hint = " ([build] takes duckdb and indicators)";
    } else if (table == "orders") {
      hint = " ([orders] takes type and offset)";
    } else {
      hint = " (tables: [bars], [data.NAME], [instruments], [replay], [params], [orders], [build])";
    }
    return Error::at(at, "unknown setting `" + e.key_ + "`" + hint);
  }
  const auto base = [&at]() {
    return base_of(at);
  };
  switch (kind) {
    case Kind::PATH:
    case Kind::TEXT: {
      if (e.value_.type_ != Value::Type::STR) {
        return bad("a string");
      }
      if (e.value_.text_.empty()) {
        return bad("a non-empty string");
      }
      if (kind == Kind::PATH) {
        e.value_ = Value::str(absolute_against(e.value_.text_, base()));
      }
      return std::nullopt;
    }
    case Kind::PATHS:
    case Kind::TEXTS:
    case Kind::COORDINATES: {
      std::vector<std::string> items;
      if (e.value_.type_ == Value::Type::STR) {
        items.push_back(e.value_.text_);
      } else if (e.value_.type_ == Value::Type::LIST) {
        for (const auto& item : e.value_.items_) {
          if (item.type_ != Value::Type::STR) {
            return bad("a string or a list of strings");
          }
          items.push_back(item.text_);
        }
      } else {
        return bad("a string or a list of strings");
      }
      const auto empty = [](const std::string& item) {
        return item.empty();
      };
      if (items.empty() || std::any_of(items.begin(), items.end(), empty)) {
        return bad("a string or a list of strings");
      }
      for (const auto& item : items) {
        if (item.find(',') != std::string::npos) {
          return Error::at(at, "`" + e.key_ + "`: \"" + item + "\" holds a comma, which the replay cannot read");
        }
      }
      if (kind == Kind::COORDINATES) {
        for (const auto& c : items) {
          const auto parts = split(c, '/');
          if (parts.size() != 3 || std::any_of(parts.begin(), parts.end(), empty)) {
            return Error::at(
              at,
              "`" + e.key_ + "`: \"" + c +
                "\" is not a store coordinate, SOURCE/INTERVAL/SYMBOL (\"kis-krx/1d/005930\")"
            );
          }
          if (std::find(INTERVALS.begin(), INTERVALS.end(), parts[1]) == INTERVALS.end()) {
            return Error::at(at, "`" + e.key_ + "`: \"" + c + "\": the interval is one of " + join(INTERVALS, ", "));
          }
        }
      }
      std::vector<Value> values;
      for (const auto& item : items) {
        values.push_back(Value::str(kind == Kind::PATHS ? absolute_against(item, base()) : item));
      }
      e.value_ = Value::list(std::move(values));
      return std::nullopt;
    }
    case Kind::CHOICE: {
      const auto& choices = known(e.key_)->choices_;
      if (
        e.value_.type_ == Value::Type::STR && std::find(choices.begin(), choices.end(), e.value_.text_) != choices.end()
      ) {
        return std::nullopt;
      }
      std::string list;
      for (const auto& choice : choices) {
        list += (list.empty() ? "" : ", ") + ("\"" + choice + "\"");
      }
      return bad("one of " + list);
    }
    case Kind::NUMBER: {
      const auto* key = known(e.key_);
      const auto v = e.value_.number_;
      if (e.value_.type_ == Value::Type::NUM && (v > key->min_ || (!key->strict_ && v == key->min_))) {
        // The replay reads these as exact decimals, which take no exponent.
        const auto plain = plain_decimal(e.value_.text_);
        if (!plain) {
          return bad("a number of at most 18 digits and 255 decimal places as a plain decimal");
        }
        e.value_ = Value::num(*plain, v);
        return std::nullopt;
      }
      return bad(key->strict_ ? "a number above 0" : "a number, 0 or more");
    }
    case Kind::COUNT: {
      const auto v = e.value_.number_;
      if (e.value_.type_ == Value::Type::NUM && integer_text(e.value_.text_) && v >= 0 && v <= 1e9) {
        const bool negative_zero = e.value_.text_.front() == '-';
        e.value_ = Value::num(negative_zero ? "-0" : std::to_string(static_cast<long long>(v)), v);
        return std::nullopt;
      }
      return bad("a whole number written as an integer, 0 or more");
    }
    case Kind::BOOL: {
      if (e.value_.type_ != Value::Type::BOOL) {
        return bad("`true` or `false`");
      }
      return std::nullopt;
    }
    case Kind::FRACTION: {
      // Decided on the text, which the replay parses exactly; the number's double never decides.
      if (e.value_.type_ != Value::Type::NUM) {
        return bad("a fraction, at least 0 and below 0.3");
      }
      const auto& t = e.value_.text_;
      if (t.find_first_of("eE") != std::string::npos) {
        return bad("written as a plain decimal (`0.002`), without an exponent");
      }
      const bool negative = !t.empty() && t.front() == '-';
      const auto body = negative ? t.substr(1) : t;
      const auto point = body.find('.');
      const auto whole = body.substr(0, point);
      const auto fraction = point == std::string::npos ? std::string() : body.substr(point + 1);
      if (fraction.size() > 18) {
        return bad("a plain decimal of at most 18 fractional digits");
      }
      const auto zero = [](const std::string& d) {
        return std::all_of(d.begin(), d.end(), [](char c) {
          return c == '0';
        });
      };
      const bool below = fraction.empty() || fraction.front() < '3';
      if (!zero(whole) || !below || (negative && !zero(fraction))) {
        return bad("a fraction, at least 0 and below 0.3");
      }
      return std::nullopt;
    }
    case Kind::DATE: {
      std::string digits;
      if (e.value_.type_ == Value::Type::NUM) {
        digits = e.value_.text_;
      } else if (e.value_.type_ == Value::Type::STR) {
        const auto& s = e.value_.text_;
        if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
          std::remove_copy(s.begin(), s.end(), std::back_inserter(digits), '-');
        } else {
          digits = s;
        }
      }
      const auto d = date_of(digits);
      if (!d) {
        return bad("a date, \"YYYY-MM-DD\" or yyyymmdd");
      }
      e.value_ = Value::num(std::to_string(*d), static_cast<double>(*d));
      return std::nullopt;
    }
    case Kind::INSTANT: {
      if (e.value_.type_ != Value::Type::STR || !instant_text(e.value_.text_)) {
        return bad("an instant, \"YYYY-MM-DD\" or \"YYYY-MM-DDTHH:MM[:SS]\" with an optional offset");
      }
      return std::nullopt;
    }
    case Kind::OFFSET: {
      if (e.value_.type_ != Value::Type::STR || !offset_text(e.value_.text_)) {
        return bad("a fixed offset, \"UTC\" or \"+HH:MM\"");
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

/**
 * @brief Whether a path holds a character that a quoted CMake argument cannot: `"`, `;` or `$`.
 * @param path The path.
 * @param setting The setting that gives it.
 * @return The refusal, or nothing.
 */
inline std::optional<std::string> cmake_refuses(const std::string& path, const std::string& setting) {
  if (path.find_first_of("\";$") == std::string::npos) {
    return std::nullopt;
  }
  return "`" + setting + "` = \"" + path + "\": a path holding `\"`, `;` or `$` cannot be passed to CMake";
}

/**
 * @brief Tests whether a text is a C++ name, possibly qualified: identifiers of ASCII letters, digits and `_`, joined
 *   by `::`.
 * @param text The text.
 * @return Whether it is.
 */
inline bool cpp_name(const std::string& text) {
  if (text.empty() || text.size() > 256) {
    return false;
  }
  std::size_t at = 0;
  while (true) {
    const auto start = at;
    while (
      at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) != 0 || text[at] == '_') &&
      static_cast<unsigned char>(text[at]) < 0x80
    ) {
      ++at;
    }
    if (at == start || std::isdigit(static_cast<unsigned char>(text[start])) != 0) {
      return false;
    }
    if (at == text.size()) {
      return true;
    }
    if (text.compare(at, 2, "::") != 0) {
      return false;
    }
    at += 2;
  }
}

/** @brief A parameter's type. */
enum class ParamType {
  INT,
  REAL,
  BOOL,
};

/** @brief What the checks need from the program, as `atlc check --info` gives it. */
struct Program {
  /** @brief The program, absolute. */
  std::string path_;
  /** @brief The generated struct, `atls::` omitted. */
  std::string struct_;
  /** @brief `single`, `named` or `universe`. */
  std::string mode_;
  /** @brief The names in `instruments {}`, in order; empty for the other modes. */
  std::vector<std::string> instruments_;
  /** @brief Whether it has `on_open` or `on_close` (session handlers). */
  bool session_{false};
  /** @brief Each parameter's name and type. */
  std::vector<std::pair<std::string, ParamType>> params_;
  /** @brief The names in `data {}`, in order; empty without it (or from an atlc that reports none). */
  std::vector<std::string> data_;
};

/** @brief The settings checked against their program: the build's settings and the driver's arguments. */
struct Resolved {
  /** @brief `incremental`, `naive` or `exact`. */
  std::string indicators_{"incremental"};
  /** @brief Whether the run reads a store, so needs DuckDB. */
  bool store_{false};
  /** @brief `build.duckdb`, absolute, or empty. */
  std::string duckdb_;
  /** @brief `market` or `limit`. */
  std::string order_type_{"market"};
  /** @brief The limit's offset as written, or empty when no setting gives one. */
  std::string offset_;
  /** @brief `replay.fill_header`, absolute, or empty: the header the driver includes for the market-price rule. */
  std::string fill_header_;
  /** @brief `replay.fill_function`, a C++ name, or empty: the market-price rule the header declares. */
  std::string fill_function_;
  /** @brief The driver's `key=value` arguments, one per key, parameters as `param.NAME` and instruments as
   * `instrument.NAME`. */
  std::vector<std::pair<std::string, std::string>> args_;
};

/**
 * @brief Merges the layers (later winning: the file, then the command line) and checks them together against the
 *   program; every error found.
 * @param layers The layers, each checked key by key.
 * @param program The program's facts.
 * @param resolved Set to the result when there is no error.
 * @return The errors.
 */
inline std::vector<Error> resolve(
  const std::vector<std::vector<Entry>>& layers, const Program& program, Resolved& resolved
) {
  std::vector<Entry> merged;
  for (const auto& layer : layers) {
    for (const auto& e : layer) {
      auto found = std::find_if(merged.begin(), merged.end(), [&e](const Entry& m) {
        return m.key_ == e.key_;
      });
      if (found != merged.end()) {
        *found = e;
      } else {
        merged.push_back(e);
      }
    }
  }
  std::vector<Error> errors;
  const auto get = [&merged](const std::string& key) -> const Entry* {
    for (const auto& e : merged) {
      if (e.key_ == key) {
        return &e;
      }
    }
    return nullptr;
  };
  const auto text = [&get](const std::string& key) -> std::optional<std::string> {
    const auto* e = get(key);
    return e == nullptr ? std::nullopt : std::optional<std::string>(e->value_.text());
  };
  const auto quoted_list = [](const std::vector<std::string>& items, const std::string& separator) {
    std::string out;
    for (const auto& item : items) {
      out += (out.empty() ? "" : separator) + ("\"" + item + "\"");
    }
    return out;
  };

  // The data source, and the keys that belong to it.
  const auto source = text("bars.source").value_or(get("bars.store") != nullptr ? "store" : "wide");
  for (const auto& e : merged) {
    const auto* k = known(e.key_);
    if (
      k != nullptr && !k->sources_.empty() &&
      std::find(k->sources_.begin(), k->sources_.end(), source) == k->sources_.end()
    ) {
      errors.push_back(
        Error::at(
          e.origin_,
          "`" + e.key_ + "` applies to bars.source = " + quoted_list(k->sources_, " or ") + ", and this run reads \"" +
            source + "\""
        )
      );
    }
  }
  const auto both = [&get, &errors](const std::string& a, const std::string& b) {
    const auto* x = get(a);
    const auto* y = get(b);
    if (x != nullptr && y != nullptr) {
      errors.push_back(
        Error::at(x->origin_, "`" + a + "` and `" + b + "` (" + y->origin_.text() + ") name the same thing: give one")
      );
    }
  };
  const auto count = [&get](const std::string& key) -> std::size_t {
    const auto* e = get(key);
    return e != nullptr && e->value_.type_ == Value::Type::LIST ? e->value_.items_.size() : 0;
  };
  if (source == "wide") {
    both("bars.file", "bars.close");
    if (get("bars.file") == nullptr && get("bars.close") == nullptr) {
      errors.push_back(
        Error::nowhere(
          "the run reads wide tables (bars.source = \"wide\"): give the closes, `bars.file` or `bars.close`"
        )
      );
    }
  } else if (source == "csv") {
    both("bars.file", "bars.files");
    both("bars.symbol", "bars.symbols");
    if (get("bars.file") == nullptr && get("bars.files") == nullptr) {
      errors.push_back(Error::nowhere("the run reads CSV (bars.source = \"csv\"): give `bars.file` or `bars.files`"));
    }
    if (text("bars.layout").value_or("long") == "wide") {
      for (const auto& e : merged) {
        if (
          e.key_.rfind("bars.columns.", 0) == 0 || e.key_ == "bars.symbol" || e.key_ == "bars.symbols" ||
          e.key_ == "bars.symbol_column"
        ) {
          errors.push_back(
            Error::at(
              e.origin_,
              "`" + e.key_ + "` applies to bars.layout = \"long\": a wide CSV names its symbols in its header"
            )
          );
        }
      }
    } else if (get("bars.symbol_column") != nullptr) {
      for (const auto* k : {"bars.symbol", "bars.symbols"}) {
        if (const auto* e = get(k)) {
          errors.push_back(
            Error::at(e->origin_, "`" + std::string(k) + "` and `bars.symbol_column` both name symbols: give one")
          );
        }
      }
    } else if (get("bars.files") != nullptr) {
      if (count("bars.symbols") != count("bars.files")) {
        errors.push_back(
          Error::nowhere(
            "bars.files names " + std::to_string(count("bars.files")) + " files and bars.symbols " +
            std::to_string(count("bars.symbols")) + " symbols: give one symbol per file (or bars.symbol_column)"
          )
        );
      }
    } else if (get("bars.file") != nullptr && get("bars.symbol") == nullptr) {
      errors.push_back(
        Error::nowhere(
          "a long CSV names its symbol: give `bars.symbol`, or `bars.symbol_column` for a file of many symbols"
        )
      );
    }
  } else {
    for (const auto* k : {"bars.store", "bars.store_root"}) {
      if (get(k) == nullptr) {
        errors.push_back(
          Error::nowhere("the run reads a store (bars.source = \"store\"): give `" + std::string(k) + "`")
        );
      }
    }
  }

  // Sessions: an error about a setting is at the setting, if it is given; else nowhere.
  const auto at = [&get](const std::string& key, const std::string& message) {
    const auto* e = get(key);
    return e == nullptr ? Error::nowhere(message) : Error::at(e->origin_, message);
  };
  // replay.fill names a rule the library ships, and a header one of one's own: one rule per backtest.
  if (const auto* e = get("replay.fill"); e != nullptr && e->value_.text() != "next-open") {
    if (get("replay.fill_header") != nullptr || get("replay.fill_function") != nullptr) {
      errors.push_back(
        Error::at(
          e->origin_,
          "`replay.fill` = \"" + e->value_.text() +
            "\" and `replay.fill_header` both price market orders: give one of the two"
        )
      );
    }
    if (text("orders.type").value_or("market") == "limit") {
      errors.push_back(
        Error::at(
          e->origin_,
          "`replay.fill` = \"" + e->value_.text() +
            "\" prices market orders, and orders.type = \"limit\" sends none: remove it or use market orders"
        )
      );
    }
  }
  // The market-price rule: a header and the function it declares, both or neither.
  const auto* fill_header = get("replay.fill_header");
  const auto* fill_function = get("replay.fill_function");
  if ((fill_header == nullptr) != (fill_function == nullptr)) {
    errors.push_back(
      Error::at(
        (fill_header != nullptr ? fill_header : fill_function)->origin_,
        "a market-price rule names its header and its function: give both `replay.fill_header` and "
        "`replay.fill_function`"
      )
    );
  }
  if (fill_header != nullptr && fill_header->value_.text().find_first_of("\"\\\n\r") != std::string::npos) {
    errors.push_back(
      Error::at(
        fill_header->origin_, "`replay.fill_header`: a path holding `\"`, `\\` or a line break cannot be included"
      )
    );
  }
  if (fill_function != nullptr && !cpp_name(fill_function->value_.text())) {
    errors.push_back(
      Error::at(
        fill_function->origin_,
        "`replay.fill_function` = \"" + fill_function->value_.text() +
          "\": give a C++ function's name, such as `decision_close` or `rules::decision_close`"
      )
    );
  }
  if (fill_function != nullptr && text("orders.type").value_or("market") == "limit") {
    errors.push_back(
      Error::at(
        fill_function->origin_,
        "`replay.fill_function` prices market orders, and orders.type = \"limit\" sends none: remove the rule or "
        "use market orders"
      )
    );
  }
  // A rule is C++ compiled into the driver cmdatl runs, so it comes from the spec file, never the command line.
  for (const auto* k : {"replay.fill_header", "replay.fill_function"}) {
    if (const auto* e = get(k); e != nullptr && e->origin_.file_.empty()) {
      errors.push_back(
        Error::at(
          e->origin_,
          "`" + std::string(k) +
            "` names C++ that cmdatl compiles and runs: give it in the spec file, "
            "not on the command line"
        )
      );
    }
  }
  // Whether every bar is daily: wide tables always, a CSV by its interval, a store by each coordinate's.
  bool daily = true;
  std::string intraday;
  if (source == "csv") {
    const auto interval = text("bars.interval").value_or("1d");
    if (interval != "1d") {
      daily = false;
      intraday = "bars.interval";
    }
  }
  if (source == "store") {
    if (const auto* e = get("bars.store"); e != nullptr && e->value_.type_ == Value::Type::LIST) {
      for (const auto& item : e->value_.items_) {
        const auto parts = split(item.text(), '/');
        if (parts.size() < 2 || parts[1] != "1d") {
          daily = false;
          intraday = "bars.store";
        }
      }
    }
  }
  const auto* open_point_entry = get("bars.open_point");
  const bool open_point = open_point_entry != nullptr && open_point_entry->value_.text() == "true";
  if (open_point && !program.session_) {
    errors.push_back(at(
      "bars.open_point",
      "bars.open_point splits each day at its open, for a program with on_open or on_close; this one has neither"
    ));
  }
  if (open_point && !daily) {
    errors.push_back(at(intraday, "bars.open_point splits daily bars; intraday bars are replayed as they are"));
  }
  if (program.session_ && daily && !open_point) {
    errors.push_back(at(
      "bars.open_point",
      "the program has on_open or on_close, and a daily bar is a whole session: set bars.open_point = true (each day "
      "as a point at its open, then the day's bar at its close), or give intraday bars"
    ));
  }
  if (open_point && source == "wide") {
    for (const auto* k : {"bars.open", "bars.high", "bars.low"}) {
      if (get(k) == nullptr) {
        errors.push_back(Error::nowhere("bars.open_point on wide tables needs `" + std::string(k) + "`"));
      }
    }
  }
  // A session program's edges come from the market's session window: the market stated, or the one the replay
  // infers (kr for a KIS, Kiwoom, Toss or KRX store, else us). Only US intraday bars are refused: daylight saving moves
  // the session an hour, and a file spanning both seasons would meet closes in winter and silently miss them in summer.
  // A market that meets no edge at all (Korean minute bars read as us) fails the replay instead.
  if (program.session_ && !daily) {
    const auto* market = get("replay.market");
    std::string effective = "us";
    if (market != nullptr) {
      effective = market->value_.text();
    } else if (const auto* store = get("bars.store")) {
      const auto first = store->value_.type_ == Value::Type::LIST ?
        (store->value_.items_.empty() ? std::string() : store->value_.items_.front().text()) :
        store->value_.text();
      const auto source = first.substr(0, first.find('/'));
      if (
        source.rfind("kis", 0) == 0 || source.rfind("kiwoom", 0) == 0 || source.rfind("toss", 0) == 0 ||
        source.find("krx") != std::string::npos
      ) {
        effective = "kr";
      }
    }
    if (effective == "us") {
      const std::string message =
        "on_open and on_close on US intraday bars would follow a fixed UTC-5 session that daylight saving moves: not "
        "supported yet (US daily bars with bars.open_point are)";
      errors.push_back(
        market != nullptr ?
          Error::at(market->origin_, message) :
          Error::nowhere(
            message +
            "; the market is us by default: for Korean bars, set replay.market = "
            "\"kr\""
          )
      );
    }
  }
  if (const auto* from = get("replay.from")) {
    if (const auto* to = get("replay.to"); to != nullptr && from->value_.text() > to->value_.text()) {
      errors.push_back(Error::at(from->origin_, "replay.from is after replay.to (" + to->origin_.text() + ")"));
    }
  }

  // Orders: an offset prices a limit, so it needs one, whichever layer set either.
  const auto kind = text("orders.type").value_or("market");
  if (kind != "limit") {
    if (const auto* e = get("orders.offset")) {
      errors.push_back(
        Error::at(
          e->origin_,
          "`orders.offset` prices a limit: it applies to orders.type = \"limit\", and this run's orders are \"" + kind +
            "\""
        )
      );
    }
  }

  // Instruments.
  std::vector<const Entry*> instruments;
  for (const auto& e : merged) {
    if (e.key_.rfind("instruments.", 0) == 0) {
      instruments.push_back(&e);
    }
  }
  if (program.mode_ == "named") {
    for (const auto& n : program.instruments_) {
      if (get("instruments." + n) == nullptr) {
        errors.push_back(
          Error::nowhere(
            "the program names instrument `" + n + "`: map it to a symbol, `" + n + " = \"...\"` in [instruments]"
          )
        );
      }
    }
    for (const auto* e : instruments) {
      const auto n = e->key_.substr(std::string("instruments.").size());
      if (std::find(program.instruments_.begin(), program.instruments_.end(), n) == program.instruments_.end()) {
        errors.push_back(
          Error::at(
            e->origin_, "the program has no instrument `" + n + "` (it names " + join(program.instruments_, ", ") + ")"
          )
        );
      }
    }
  } else if (program.mode_ != "universe" && instruments.size() > 1) {
    errors.push_back(
      Error::at(
        instruments[1]->origin_,
        "a program without instruments {} or universe {} trades one symbol: [instruments] holds at most one"
      )
    );
  }

  // Data: each value of the program's `data {}` mapped to a file, and nothing else mapped.
  std::vector<std::string> mapped;
  for (const auto& e : merged) {
    if (const auto split = data_name_key(e.key_)) {
      if (std::find(mapped.begin(), mapped.end(), split->first) == mapped.end()) {
        mapped.push_back(split->first);
        if (std::find(program.data_.begin(), program.data_.end(), split->first) == program.data_.end()) {
          errors.push_back(
            Error::at(
              e.origin_,
              program.data_.empty() ?
                "the program has no data {}: remove [data." + split->first + "]" :
                "the program has no data `" + split->first + "` (it reads " + join(program.data_, ", ") + ")"
            )
          );
        }
      }
    }
  }
  for (const auto& n : program.data_) {
    const auto* file = get("data." + n + ".file");
    if (file == nullptr) {
      errors.push_back(
        Error::nowhere("the program reads data `" + n + "`: map it to a file, `file = \"...\"` in [data." + n + "]")
      );
      continue;
    }
    const auto* layout = get("data." + n + ".layout");
    const bool wide = layout != nullptr && layout->value_.text() == "wide";
    for (const auto* key : {"column", "symbol_column"}) {
      if (const auto* e = get("data." + n + "." + key); e != nullptr && wide) {
        errors.push_back(
          Error::at(e->origin_, "a wide data file names its columns by symbol: remove `" + e->key_ + "`")
        );
      }
    }
    const bool per_instrument = wide || get("data." + n + ".symbol_column") != nullptr;
    if (per_instrument && program.mode_ == "named") {
      errors.push_back(
        Error::at(
          file->origin_,
          "a program with instruments {} reads one value of `" + n +
            "` for the program: give a file with a time and a value column, no symbols"
        )
      );
    }
  }

  // Parameters.
  std::vector<std::pair<std::string, std::string>> params;
  for (const auto& e : merged) {
    if (e.key_.rfind("params.", 0) != 0) {
      continue;
    }
    const auto name = e.key_.substr(std::string("params.").size());
    const auto found = std::find_if(program.params_.begin(), program.params_.end(), [&name](const auto& p) {
      return p.first == name;
    });
    if (found == program.params_.end()) {
      std::vector<std::string> names;
      for (const auto& p : program.params_) {
        names.push_back(p.first);
      }
      errors.push_back(
        Error::at(
          e.origin_,
          "the program has no parameter `" + name + "`" +
            (names.empty() ? std::string(" (it has none)") : " (it has " + join(names, ", ") + ")")
        )
      );
      continue;
    }
    const auto& v = e.value_;
    std::string value;
    switch (found->second) {
      case ParamType::BOOL:
        if (v.type_ != Value::Type::BOOL) {
          errors.push_back(Error::at(e.origin_, "`" + name + "` is a bool parameter: give `true` or `false`"));
          continue;
        }
        value = v.boolean_ ? "1" : "0";
        break;
      case ParamType::INT:
        if (v.type_ != Value::Type::NUM || !integer_text(v.text_) || std::abs(v.number_) > 9e15) {
          errors.push_back(
            Error::at(
              e.origin_,
              "`" + name +
                "` is an int parameter: give a whole number written as an integer, of magnitude at most 9e15, not " +
                v.describe()
            )
          );
          continue;
        }
        value = v.number_ == 0 && v.text_.front() == '-' ? "-0" : std::to_string(static_cast<long long>(v.number_));
        break;
      case ParamType::REAL:
        if (v.type_ != Value::Type::NUM) {
          errors.push_back(Error::at(e.origin_, "`" + name + "` is a real parameter: give a number"));
          continue;
        }
        value = v.text_;
        break;
    }
    params.emplace_back("param." + name, value);
  }
  if (!errors.empty()) {
    return errors;
  }

  Resolved out;
  out.args_.emplace_back("bars.source", source);
  for (const auto& e : merged) {
    if (
      e.key_ == "strategy" || e.key_ == "bars.source" || e.key_.rfind("build.", 0) == 0 ||
      e.key_.rfind("params.", 0) == 0 || e.key_ == "replay.fill_header" || e.key_ == "replay.fill_function"
    ) {
      continue;
    }
    const auto key = e.key_.rfind("instruments.", 0) == 0 ? "instrument." + e.key_.substr(12) : e.key_;
    out.args_.emplace_back(key, e.value_.text());
  }
  out.args_.insert(out.args_.end(), params.begin(), params.end());
  out.store_ = source == "store";
  // DuckDB's directory reaches CMake only for a store run, so only then must it be one CMake can take.
  if (const auto* e = get("build.duckdb"); e != nullptr && out.store_) {
    if (const auto refused = cmake_refuses(e->value_.text(), "build.duckdb")) {
      errors.push_back(Error::at(e->origin_, *refused));
      return errors;
    }
    out.duckdb_ = e->value_.text();
  }
  out.indicators_ = text("build.indicators").value_or("incremental");
  out.fill_header_ = text("replay.fill_header").value_or("");
  out.fill_function_ = text("replay.fill_function").value_or("");
  out.order_type_ = kind;
  out.offset_ = text("orders.offset").value_or("");
  // The driver's arguments are bounded, so a huge setting is refused here rather than failing at exec.
  if (out.args_.size() > MAX_ARGS) {
    errors.push_back(
      Error::nowhere(
        "the settings make " + std::to_string(out.args_.size()) + " arguments, more than " + std::to_string(MAX_ARGS)
      )
    );
  }
  // The setting an argument came from, for the place of an error about it.
  const auto origin_of = [&get](const std::string& key) {
    const auto setting = key.rfind("instrument.", 0) == 0 ?
      "instruments." + key.substr(11) :
      key.rfind("param.", 0) == 0 ?
      "params." + key.substr(6) :
      key;
    const auto* e = get(setting);
    return e == nullptr ? Origin{} : e->origin_;
  };
  for (const auto& [key, value] : out.args_) {
    if (!valid_key(key)) {
      errors.push_back(Error::at(origin_of(key), "`" + key.substr(0, 140) + "` is longer than a key may be (128)"));
    } else if (value.size() > MAX_VALUE || value.find('\0') != std::string::npos) {
      errors.push_back(
        Error::at(origin_of(key), "`" + key + "` is longer than " + std::to_string(MAX_VALUE) + " bytes")
      );
    }
  }
  if (errors.empty()) {
    resolved = std::move(out);
  }
  return errors;
}

/**
 * @brief Reads a command-line setting, typed by its key (as CmdStan types its arguments, never as a TOML value): a
 *   text, path, choice, offset or instant key takes the text as it is; a number, count or fraction key a decimal; a
 *   boolean `true` or `false`; a date either form; a list one bare item or a one-line TOML array; `params.NAME` a
 *   number or `true`/`false`.
 * @param word `KEY=VALUE`, a setting by `is_setting`.
 * @param entry Set to the entry, unchecked.
 * @return The error, or nothing.
 */
inline std::optional<Error> from_command_line(const std::string& word, Entry& entry) {
  const auto eq = word.find('=');
  entry.key_ = word.substr(0, eq);
  entry.origin_ = Origin{};
  const auto text = word.substr(eq + 1);
  if (entry.key_ == "strategy") {
    return Error::at(entry.origin_, "`strategy` is not a setting on the command line: give the program as the target");
  }
  const auto number_or_text = [&text]() {
    if (auto n = decimal(text)) {
      return *n;
    }
    return Value::str(text);
  };
  if (entry.key_.rfind("params.", 0) == 0) {
    entry.value_ = text == "true" ? Value::boolean(true) : text == "false" ? Value::boolean(false) : number_or_text();
    return std::nullopt;
  }
  const auto* key = known(entry.key_);
  const auto kind = key == nullptr ? Kind::TEXT : key->kind_;
  switch (kind) {
    case Kind::NUMBER:
    case Kind::COUNT:
    case Kind::FRACTION:
      entry.value_ = number_or_text();
      break;
    case Kind::BOOL:
      entry.value_ = text == "true" ? Value::boolean(true) : text == "false" ? Value::boolean(false) : Value::str(text);
      break;
    case Kind::PATHS:
    case Kind::TEXTS:
    case Kind::COORDINATES: {
      if (text.empty() || text.front() != '[') {
        entry.value_ = Value::str(text);
        break;
      }
      // A one-line TOML array of strings.
      if (text.find('\n') != std::string::npos) {
        return Error::at(entry.origin_, "`" + entry.key_ + "`: an array is written on one line");
      }
      const std::string document = "v = " + text;
      if (config_detail::nesting_refusal(document) || config_detail::non_ascii_refusal(document)) {
        return Error::at(entry.origin_, "`" + entry.key_ + "`: " + text + " is not a TOML array of strings");
      }
      toml::table parsed;
      try {
        parsed = toml::parse(std::string_view(document));
      } catch (const TomlInvariant&) {
        return Error::at(entry.origin_, "`" + entry.key_ + "`: " + text + " is not a TOML array of strings");
      } catch (const toml::parse_error&) {
        return Error::at(entry.origin_, "`" + entry.key_ + "`: " + text + " is not a TOML array of strings");
      }
      const auto* array = parsed.get("v") == nullptr ? nullptr : parsed.get("v")->as_array();
      if (array == nullptr) {
        return Error::at(entry.origin_, "`" + entry.key_ + "`: " + text + " is not a TOML array of strings");
      }
      std::vector<Value> items;
      for (const auto& element : *array) {
        const auto* s = element.as_string();
        if (s == nullptr) {
          return Error::at(entry.origin_, "`" + entry.key_ + "`: an array holds strings");
        }
        items.push_back(Value::str(s->get()));
      }
      entry.value_ = Value::list(std::move(items));
      break;
    }
    case Kind::PATH:
    case Kind::TEXT:
    case Kind::CHOICE:
    case Kind::DATE:
    case Kind::INSTANT:
    case Kind::OFFSET:
      entry.value_ = Value::str(text);
      break;
  }
  return std::nullopt;
}

/**
 * @brief The byte offset of a position toml++ gives (a line, and a column counted in code points from 1, a leading
 *   byte-order mark not counted).
 * @param text The document.
 * @param starts Each line's first byte.
 * @param line The line.
 * @param column The column.
 * @return The offset, or nothing when the position is not in the text.
 */
inline std::optional<std::size_t> offset_of(
  const std::string& text, const std::vector<std::size_t>& starts, std::size_t line, std::size_t column
) {
  if (line == 0 || line > starts.size() || column == 0) {
    return std::nullopt;
  }
  auto at = starts[line - 1];
  // toml++ skips a byte-order mark without counting it.
  if (line == 1 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    at = 3;
  }
  for (std::size_t k = 1; k < column; ++k) {
    if (at >= text.size() || text[at] == '\n') {
      return std::nullopt;
    }
    ++at;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0u) == 0x80u) {
      ++at;
    }
  }
  return at;
}

/**
 * @brief The text of a number in the document, from its first character to the next delimiter.
 * @param text The document.
 * @param at Its first byte.
 * @return The text.
 */
inline std::string number_text_at(const std::string& text, std::size_t at) {
  auto end = at;
  while (end < text.size() && std::strchr(" \t\r\n,]}#", text[end]) == nullptr) {
    ++end;
  }
  return text.substr(at, end - at);
}

/** @brief A file's entries and errors, in the order they were written. */
struct Read {
  /** @brief The entries, unchecked. */
  std::vector<Entry> entries_;
  /** @brief The errors. */
  std::vector<Error> errors_;
};

/**
 * @brief Flattens a parsed TOML document into entries: each leaf `table.key` with its value; numbers re-read from the
 *   document's text; arrays of strings or numbers; anything else (a date or a time, an array of tables or of
 *   booleans, a quoted key that is not bare) an error at its line.
 * @param root The document.
 * @param text Its text.
 * @param file The file, absolute.
 * @return What was read.
 */
inline Read flatten(const toml::table& root, const std::string& text, const std::string& file) {
  std::vector<std::size_t> starts{0};
  for (std::size_t k = 0; k < text.size(); ++k) {
    if (text[k] == '\n') {
      starts.push_back(k + 1);
    }
  }
  struct Placed {
    std::size_t line_;
    std::size_t column_;
    std::optional<Entry> entry_;
    std::optional<Error> error_;
  };
  std::vector<Placed> placed;
  const auto origin = [&file](const toml::source_region& region) {
    return Origin{file, region.begin.line};
  };
  const auto refuse = [&placed, &origin](const toml::source_region& region, const std::string& message) {
    placed.push_back({region.begin.line, region.begin.column, std::nullopt, Error::at(origin(region), message)});
  };
  // A number's value from its text in the document; toml++'s own value only confirms the position was read right.
  const auto number = [&](const toml::node& node, const std::string& key) -> std::optional<Value> {
    const auto at = offset_of(text, starts, node.source().begin.line, node.source().begin.column);
    const auto written = at ? number_text_at(text, *at) : std::string();
    auto value = decimal(written);
    if (!value) {
      refuse(
        node.source(),
        "`" + key + "` = " + (written.empty() ? std::string("this number") : written) +
          ": write a decimal number (hexadecimal, octal, binary, inf and nan are not read)"
      );
      return std::nullopt;
    }
    const double expected =
      node.is_integer() ? static_cast<double>(node.as_integer()->get()) : node.as_floating_point()->get();
    if (value->number_ != expected) {
      refuse(node.source(), "`" + key + "`: the number's text could not be read back");
      return std::nullopt;
    }
    return value;
  };
  const auto scalar = [&](const toml::node& node, const std::string& key) -> std::optional<Value> {
    if (const auto* s = node.as_string()) {
      return Value::str(s->get());
    }
    if (node.is_integer() || node.is_floating_point()) {
      return number(node, key);
    }
    if (const auto* b = node.as_boolean()) {
      return Value::boolean(b->get());
    }
    if (node.is_date() || node.is_time() || node.is_date_time()) {
      refuse(node.source(), "`" + key + "`: write a date or a time as a string, in quotes (\"2024-01-02\")");
      return std::nullopt;
    }
    return std::nullopt;
  };
  std::function<void(const toml::table&, const std::string&)> walk;
  walk = [&](const toml::table& table, const std::string& prefix) {
    for (auto&& [name, node] : table) {
      const auto part = std::string(name.str());
      if (!bare(part)) {
        refuse(
          name.source(),
          "`" + part + "` is not a key the settings take: keys are bare (letters, digits, `_`, `-`) joined by dots"
        );
        continue;
      }
      const auto key = prefix.empty() ? part : prefix + "." + part;
      if (const auto* inner = node.as_table()) {
        walk(*inner, key);
        continue;
      }
      if (const auto* array = node.as_array()) {
        if (!array->empty() && array->front().is_table()) {
          refuse(node.source(), "`" + key + "`: arrays of tables are not part of the run spec");
          continue;
        }
        std::vector<Value> items;
        bool ok = true;
        for (const auto& element : *array) {
          if (!element.is_string() && !element.is_integer() && !element.is_floating_point()) {
            refuse(element.source(), "`" + key + "`: an array holds strings or numbers");
            ok = false;
            break;
          }
          auto item = scalar(element, key);
          if (!item) {
            ok = false;
            break;
          }
          items.push_back(std::move(*item));
        }
        if (ok) {
          placed.push_back(
            {node.source().begin.line, node.source().begin.column,
              Entry{key, Value::list(std::move(items)), origin(node.source())}, std::nullopt}
          );
        }
        continue;
      }
      auto value = scalar(node, key);
      if (value) {
        placed.push_back(
          {node.source().begin.line, node.source().begin.column, Entry{key, std::move(*value), origin(node.source())},
            std::nullopt}
        );
      }
    }
  };
  walk(root, "");
  std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
    return a.line_ != b.line_ ? a.line_ < b.line_ : a.column_ < b.column_;
  });
  Read read;
  for (auto& p : placed) {
    if (p.entry_) {
      read.entries_.push_back(std::move(*p.entry_));
    } else {
      read.errors_.push_back(std::move(*p.error_));
    }
  }
  return read;
}

/**
 * @brief Reads a run spec's text: a syntax error is one error at its line; otherwise every key's error, each at its
 *   line, and the entries checked key by key (paths made absolute against the file's directory).
 * @param text The document.
 * @param file The file, absolute.
 * @return The entries and the errors.
 */
inline Read read_text(const std::string& text, const std::string& file) {
  Read read;
  if (const auto deep = config_detail::nesting_refusal(text)) {
    read.errors_.push_back(Error::at(Origin{file, deep->at_.line}, "not TOML this version reads: " + deep->message_));
    return read;
  }
  if (const auto foreign = config_detail::non_ascii_refusal(text)) {
    read.errors_.push_back(Error::at(Origin{file, foreign->at_.line}, "not valid TOML: " + foreign->message_));
    return read;
  }
  toml::table root;
  try {
    // Without a source path: toml++ copies one into a shared string in a constructor marked noexcept, so an
    // allocation failing there would end the process.
    root = toml::parse(std::string_view(text));
  } catch (const TomlInvariant& error) {
    read.errors_.push_back(Error::file(file, std::string("not TOML this version reads (") + error.what() + ")"));
    return read;
  } catch (const toml::parse_error& error) {
    read.errors_.push_back(
      Error::at(Origin{file, error.source().begin.line}, "not valid TOML: " + std::string(error.description()))
    );
    return read;
  }
  read = flatten(root, text, file);
  // Every key's error, the entries flatten read included, in the file's order.
  for (auto& e : read.entries_) {
    if (auto error = check(e)) {
      read.errors_.push_back(std::move(*error));
    }
  }
  std::stable_sort(read.errors_.begin(), read.errors_.end(), [](const Error& a, const Error& b) {
    return a.line_ < b.line_;
  });
  return read;
}

/**
 * @brief Reads a run spec file: its entries checked, and the program it names, if any (`strategy` is optional here:
 *   whether a program must come from the spec is the caller's to decide).
 * @param path The file, absolute.
 * @param program Set to the program's path, absolute, when the spec names one; left as it is otherwise.
 * @return The entries and the errors.
 */
inline Read read_file(const std::string& path, std::string& program) {
  Read read;
#if defined(_WIN32)
  (void)program;
  read.errors_.push_back(Error::file(path, "a run spec is read on macOS and Linux only in this version", "unreadable"));
  return read;
#else
  ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (fd.get() < 0) {
    read.errors_.push_back(
      Error::file(path, std::string("cannot read the run spec: ") + std::strerror(errno), "unreadable")
    );
    return read;
  }
  std::string text;
  const auto got = read_whole(fd.get(), MAX_SPEC_BYTES, text);
  if (got.status_ != FileRead::Status::OK) {
    read.errors_.push_back(Error::file(path, "cannot read the run spec: " + got.text(MAX_SPEC_BYTES), "unreadable"));
    return read;
  }
  read = read_text(text, path);
  if (!read.errors_.empty()) {
    return read;
  }
  const auto strategy = std::find_if(read.entries_.begin(), read.entries_.end(), [](const Entry& e) {
    return e.key_ == "strategy";
  });
  if (strategy != read.entries_.end()) {
    program = strategy->value_.text();
  }
  return read;
#endif
}

/** @brief What `atlc check FILE --info` answered, checked. */
struct Info {
  /** @brief Whether the program is valid (`status` `ok`). */
  bool ok_{false};
  /** @brief atlc's version. */
  std::string atlc_;
  /** @brief The program's facts, when valid. */
  Program program_;
  /** @brief The diagnostics, each `severity`, `code`, `message`, `file`, `line`, `origin`. */
  json::array diagnostics_;
};

/**
 * @brief Whether two absolute paths name one file as written: equal once each is made lexically normal (atlc drops
 *   `.` components and repeated slashes but keeps `..`).
 * @param a One path.
 * @param b The other.
 * @return Whether they are equal.
 */
inline bool same_path(const std::string& a, const std::string& b) {
  return std::filesystem::path(a).lexically_normal() == std::filesystem::path(b).lexically_normal();
}

/**
 * @brief Whether a value is one of atlc's diagnostics: `severity` `error` or `warning`, `code`, `message` and
 *   `origin` strings, `file` a string or null, `line` a positive integer or null.
 * @param value The value.
 * @return Whether it is.
 */
inline bool info_diagnostic(const json::value& value) {
  const auto* object = value.if_object();
  if (object == nullptr) {
    return false;
  }
  for (const auto* key : {"severity", "code", "message", "origin"}) {
    const auto* field = object->if_contains(key);
    if (field == nullptr || !field->is_string()) {
      return false;
    }
  }
  const auto& severity = object->at("severity").as_string();
  const auto* file = object->if_contains("file");
  const auto* line = object->if_contains("line");
  return (severity == "error" || severity == "warning") && file != nullptr && (file->is_null() || file->is_string()) &&
    line != nullptr && (line->is_null() || (line->is_int64() && line->as_int64() >= 1));
}

/**
 * @brief Reads `atlc check FILE --info`'s one JSON object and checks every field cmdatl uses: `atlc_info` 1; `status`
 *   `error` exactly when a diagnostic is an error, and `program` null exactly then; the program's path absolute and the
 *   one cmdatl passed; its struct one cmdatl may paste into C++; its mode one of three, with instruments only when
 *   named; parameters and instruments named by bare keys; `session` a boolean.
 * @param text atlc's standard output.
 * @param passed The program's path as cmdatl passed it.
 * @return The answer, or nothing when it is not one.
 */
inline std::optional<Info> read_info(const std::string& text, const std::string& passed) {
  boost::system::error_code error;
  auto parsed = json::parse(text, error);
  if (error || !parsed.is_object()) {
    return std::nullopt;
  }
  const auto& object = parsed.as_object();
  const auto* format = object.if_contains("atlc_info");
  const auto* status = object.if_contains("status");
  const auto* version = object.if_contains("atlc");
  const auto* diagnostics = object.if_contains("diagnostics");
  const auto* program = object.if_contains("program");
  if (
    format == nullptr || !format->is_int64() || format->as_int64() != 1 || status == nullptr || !status->is_string() ||
    version == nullptr || !version->is_string() || diagnostics == nullptr || !diagnostics->is_array() ||
    program == nullptr
  ) {
    return std::nullopt;
  }
  Info info;
  info.atlc_ = std::string(version->as_string());
  bool failed = false;
  for (const auto& d : diagnostics->as_array()) {
    if (!info_diagnostic(d)) {
      return std::nullopt;
    }
    failed = failed || d.as_object().at("severity").as_string() == "error";
  }
  info.diagnostics_ = diagnostics->as_array();
  if (status->as_string() != (failed ? "error" : "ok") || program->is_null() != failed) {
    return std::nullopt;
  }
  info.ok_ = !failed;
  if (failed) {
    return info;
  }
  if (!program->is_object()) {
    return std::nullopt;
  }
  const auto& facts = program->as_object();
  const auto string_of = [&facts](const char* key) -> std::optional<std::string> {
    const auto* field = facts.if_contains(key);
    if (field == nullptr || !field->is_string()) {
      return std::nullopt;
    }
    return std::string(field->as_string());
  };
  const auto path = string_of("path");
  const auto name = string_of("struct");
  const auto mode = string_of("mode");
  const auto* instruments = facts.if_contains("instruments");
  const auto* session = facts.if_contains("session");
  const auto* params = facts.if_contains("params");
  if (
    !path || path->empty() || path->front() != '/' || !same_path(*path, passed) || !name || !valid_struct(*name) ||
    !mode || (*mode != "single" && *mode != "named" && *mode != "universe") || instruments == nullptr ||
    !instruments->is_array() || session == nullptr || !session->is_bool() || params == nullptr || !params->is_array() ||
    instruments->as_array().size() > MAX_ARGS || params->as_array().size() > MAX_ARGS
  ) {
    return std::nullopt;
  }
  auto& p = info.program_;
  p.path_ = *path;
  p.struct_ = *name;
  p.mode_ = *mode;
  p.session_ = session->as_bool();
  for (const auto& instrument : instruments->as_array()) {
    if (!instrument.is_string() || !bare(instrument.as_string()) || instrument.as_string().size() > 64) {
      return std::nullopt;
    }
    p.instruments_.emplace_back(instrument.as_string());
  }
  if ((p.mode_ == "named") == p.instruments_.empty()) {
    return std::nullopt;
  }
  for (const auto& param : params->as_array()) {
    const auto* entry = param.if_object();
    const auto* param_name = entry == nullptr ? nullptr : entry->if_contains("name");
    const auto* type = entry == nullptr ? nullptr : entry->if_contains("type");
    if (
      param_name == nullptr || !param_name->is_string() || !bare(param_name->as_string()) ||
      param_name->as_string().size() > 64 || type == nullptr || !type->is_string()
    ) {
      return std::nullopt;
    }
    const auto& t = type->as_string();
    if (t != "int" && t != "real" && t != "bool") {
      return std::nullopt;
    }
    p.params_.emplace_back(
      std::string(param_name->as_string()),
      t == "int" ?
        ParamType::INT :
        t == "real" ?
        ParamType::REAL :
        ParamType::BOOL
    );
  }
  if (const auto* data = facts.if_contains("data")) {
    if (!data->is_array() || data->as_array().size() > MAX_ARGS) {
      return std::nullopt;
    }
    for (const auto& value : data->as_array()) {
      const auto* entry = value.if_object();
      const auto* value_name = entry == nullptr ? nullptr : entry->if_contains("name");
      const auto* type = entry == nullptr ? nullptr : entry->if_contains("type");
      if (
        value_name == nullptr || !value_name->is_string() || !bare(value_name->as_string()) ||
        value_name->as_string().size() > 64 || type == nullptr || !type->is_string() || type->as_string() != "real"
      ) {
        return std::nullopt;
      }
      p.data_.emplace_back(value_name->as_string());
    }
  }
  return info;
}

} // namespace spec
} // namespace cmdatl
