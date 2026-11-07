#pragma once

// The operator's file: TOML named by CMDATL_CONFIG only, one account per file, read once per invocation.
//
// Strict in two layers: toml++ refuses what TOML 1.0 forbids (a key or a table defined twice, an unclosed table, a
// bare word as a value, text that is not UTF-8); the validator below then refuses any key it does not list, any
// value of another TOML type, and any value outside its range, with the file, the line and the column.

#include "../core/reply.hpp"
#include "../paper/bars.hpp"
#include "../paper/time.hpp"
#include "../system/files.hpp"

#include <stdexcept>

namespace cmdatl {

/** @brief toml++ found one of its own invariants broken by a document: the document is refused. */
class TomlInvariant : public std::runtime_error {
public:
  /**
   * @brief Builds the error.
   * @param what The invariant.
   */
  explicit TomlInvariant(const char* what) : std::runtime_error(what) {}
};

} // namespace cmdatl

// toml++ checks its own invariants with TOML_ASSERT, an abort without NDEBUG and, with it, an assumption the optimizer
// may build on (`TOML_ASSERT_ASSUME`); a table header such as `[~x]` reaches one in toml++ 3.4.0.
// Kept as a check that throws, whatever NDEBUG says, so such a document is refused, never an abort or undefined
// behaviour.
#pragma push_macro("NDEBUG")
#undef NDEBUG
#define TOML_ASSERT(expr) ((expr) ? static_cast<void>(0) : throw ::cmdatl::TomlInvariant("toml++ invariant: " #expr))
#include <toml++/toml.hpp>
#pragma pop_macro("NDEBUG")

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cmdatl {

/** @brief The largest operator's file, in bytes. */
inline constexpr std::size_t MAX_CONFIG_BYTES = 64u << 10;

/**
 * @brief Whether a limit sits outside the collar around the last price: the one rule once() and a run's placements
 *   apply, in the same arithmetic, so they agree at the edge.
 * @param limit The limit price.
 * @param mark The last price.
 * @param collar The operator's collar, as a fraction.
 * @return Whether it is outside (a last price of zero or below is outside every collar).
 */
inline bool outside_collar(double limit, double mark, double collar) {
  return mark <= 0 || std::abs(limit - mark) > collar * mark;
}

/** @brief The operator's limits. */
struct Limits {
  /** @brief The symbols that may be traded at all. */
  std::vector<std::string> symbols_;
  /** @brief The largest position each symbol may reach, in shares. */
  std::map<std::string, std::int64_t> position_;
  /** @brief The largest value of one order, in KRW, as a decimal. */
  std::string order_value_;
  /** @brief The most placements per KST date, every sender on the account. */
  std::int64_t daily_orders_{0};
  /** @brief The order type allowed: `limit`. */
  std::string order_type_{"limit"};
  /** @brief How far a limit may sit from the last price, as a fraction. */
  double collar_{0.05};
  /** @brief The market phases in which anything may be sent. */
  std::vector<std::string> phases_{"open"};
  /** @brief How long a plan id stays good, in seconds. */
  std::int64_t plan_ttl_{60};
  /** @brief How old holdings and the listing may be when a decision is made, in seconds. */
  std::int64_t portfolio_age_{30};
};

/** @brief What the paper market is built from. */
struct PaperSettings {
  /** @brief The bars file, absolute. */
  std::string bars_;
  /** @brief The opening cash, KRW. */
  std::int64_t cash_{0};
  /** @brief The first instant, as written (`YYYY-MM-DDTHH:MM`, KST). */
  std::string start_;
  /** @brief The first instant. */
  paper::Clock::time_point start_at_{};
  /** @brief Seconds an accepted order stays out of the order list. */
  std::int64_t listing_lag_{0};
  /** @brief Seconds a fill stays out of holdings. */
  std::int64_t holdings_lag_{0};
};

/** @brief The operator's file, read and checked. */
struct OperatorConfig {
  /** @brief The file, absolute. */
  std::string path_;
  /** @brief Its directory, absolute. */
  std::string dir_;
  /** @brief The `atlc` key, absolute, or empty. */
  std::string atlc_;
  /** @brief The broker: `paper` or `kis`. */
  std::string broker_;
  /** @brief The environment: `paper` for the paper broker, else `demo` or `real`. */
  std::string env_;
  /** @brief The account's number; empty for a KIS account whose login names it (the run fills it from the login). */
  std::string number_;
  /** @brief The limits. */
  Limits limits_;
  /** @brief The paper market. */
  PaperSettings paper_;

  /**
   * @brief The armed file: `<stem>.armed` beside the operator's file, fixed.
   * @return The path.
   */
  std::string armed_path() const {
    const std::filesystem::path file(path_);
    return (file.parent_path() / (file.stem().string() + ".armed")).string();
  }

  /**
   * @brief The operator's approval for `order release`: `<stem>.release` beside the operator's file, fixed.
   * @return The path.
   */
  std::string release_path() const {
    const std::filesystem::path file(path_);
    return (file.parent_path() / (file.stem().string() + ".release")).string();
  }

  /**
   * @brief The journal's scope, `<broker>:<env>:<number>`.
   * @return The scope.
   */
  std::string scope() const {
    return number_.empty() ? broker_ + ":" + env_ : broker_ + ":" + env_ + ":" + number_;
  }

  /**
   * @brief The account's directory name, `<broker>-<env>-<number>`.
   * @return The name.
   */
  std::string account_name() const {
    return number_.empty() ? broker_ + "-" + env_ : broker_ + "-" + env_ + "-" + number_;
  }

  /**
   * @brief Whether a symbol may be traded.
   * @param symbol The symbol.
   * @return Whether it is in `limits.symbols`.
   */
  bool allows(const std::string& symbol) const {
    return std::find(limits_.symbols_.begin(), limits_.symbols_.end(), symbol) != limits_.symbols_.end();
  }
};

/** @brief Why the operator's file was not read. */
struct ConfigError {
  /** @brief `config.missing`, `config.invalid` or `unsupported.broker`. */
  std::string code_;
  /** @brief What is wrong. */
  std::string message_;
  /** @brief The line, from 1, or 0 when none applies. */
  std::uint32_t line_{0};
  /** @brief The column, from 1, or 0 when none applies. */
  std::uint32_t column_{0};
};

/** @brief The operator's file as one invocation read it: every consumer in the invocation reads this one. */
struct ConfigSnapshot {
  /** @brief What was found. */
  enum class State {
    UNSET,
    MISSING,
    INVALID,
    OK,
  };
  /** @brief What was found. */
  State state_{State::UNSET};
  /** @brief `CMDATL_CONFIG` as written, or empty. */
  std::string path_;
  /** @brief The file, when `OK`. */
  std::optional<OperatorConfig> config_;
  /** @brief Why not, when `MISSING` or `INVALID`. */
  ConfigError error_;

  /**
   * @brief The state's name.
   * @return `unset`, `missing`, `invalid` or `ok`.
   */
  const char* state_name() const {
    switch (state_) {
      case State::UNSET:
        return "unset";
      case State::MISSING:
        return "missing";
      case State::INVALID:
        return "invalid";
      case State::OK:
        return "ok";
    }
    return "unknown";
  }
};

namespace config_detail {

/** @brief A refusal of the validator, with where it applies. */
struct Refusal {
  /** @brief The code. */
  std::string code_;
  /** @brief What is wrong. */
  std::string message_;
  /** @brief Where. */
  toml::source_position at_;
};

/**
 * @brief A refusal at a node.
 * @param where Where.
 * @param message What is wrong.
 * @param code The code.
 * @return The refusal.
 */
inline Refusal refuse(const toml::source_region& where, std::string message, std::string code = "config.invalid") {
  return Refusal{std::move(code), std::move(message), where.begin};
}

/**
 * @brief The name of a TOML node's type, for a message.
 * @param node The node.
 * @return The name.
 */
inline std::string type_name(const toml::node& node) {
  switch (node.type()) {
    case toml::node_type::table:
      return "a table";
    case toml::node_type::array:
      return "an array";
    case toml::node_type::string:
      return "a string";
    case toml::node_type::integer:
      return "an integer";
    case toml::node_type::floating_point:
      return "a float";
    case toml::node_type::boolean:
      return "a boolean";
    case toml::node_type::date:
    case toml::node_type::time:
    case toml::node_type::date_time:
      return "a TOML date or time";
    default:
      return "a value";
  }
}

/**
 * @brief Refuses a key a table does not list: a misspelt optional key would otherwise leave its default in force.
 * @param table The table.
 * @param name The table's name, for the message (empty for the top level).
 * @param allowed The keys it may have.
 * @return The refusal of the first other key, or nothing.
 */
inline std::optional<Refusal> only_keys(
  const toml::table& table, const std::string& name, const std::set<std::string>& allowed
) {
  for (const auto& entry : table) {
    const auto key = std::string(entry.first.str());
    if (allowed.count(key) == 0) {
      return refuse(
        entry.first.source(),
        (name.empty() ? "" : "[" + name + "] ") + "has no key " + key + " (the keys are: " +
          [&] {
            std::string list;
            for (const auto& k : allowed) {
              list += (list.empty() ? "" : ", ") + k;
            }
            return list;
          }() +
          ")"
      );
    }
  }
  return std::nullopt;
}

/**
 * @brief An integer key within a range.
 * @param table The table.
 * @param key The key.
 * @param name The key's full name, for the message.
 * @param low The lowest value.
 * @param high The highest value.
 * @param out Where the value goes when present.
 * @return The refusal, or nothing (also when absent).
 */
inline std::optional<Refusal> integer_in(
  const toml::table& table, const char* key, const std::string& name, std::int64_t low, std::int64_t high,
  std::int64_t& out
) {
  const auto* node = table.get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  const auto* value = node->as_integer();
  if (value == nullptr) {
    return refuse(node->source(), name + " must be an integer, not " + type_name(*node));
  }
  if (value->get() < low || value->get() > high) {
    return refuse(
      node->source(),
      name + " must be from " + std::to_string(low) + " to " + std::to_string(high) + ", not " +
        std::to_string(value->get())
    );
  }
  out = value->get();
  return std::nullopt;
}

/**
 * @brief A string key.
 * @param table The table.
 * @param key The key.
 * @param name The key's full name, for the message.
 * @param out Where the value goes when present.
 * @return The refusal, or nothing (also when absent).
 */
inline std::optional<Refusal> string_of(
  const toml::table& table, const char* key, const std::string& name, std::optional<std::string>& out
) {
  const auto* node = table.get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  const auto* value = node->as_string();
  if (value == nullptr) {
    return refuse(node->source(), name + " must be a string, not " + type_name(*node));
  }
  out = value->get();
  return std::nullopt;
}

/**
 * @brief A required table.
 * @param table The parent.
 * @param key The key.
 * @param out Where the table goes.
 * @return The refusal, or nothing.
 */
inline std::optional<Refusal> table_of(const toml::table& table, const char* key, const toml::table*& out) {
  const auto* node = table.get(key);
  if (node == nullptr) {
    return refuse(table.source(), std::string("the file needs a [") + key + "] table");
  }
  out = node->as_table();
  if (out == nullptr) {
    return refuse(node->source(), std::string(key) + " must be a table, not " + type_name(*node));
  }
  return std::nullopt;
}

/**
 * @brief A path from the file, relative to its directory.
 * @param text The path as written.
 * @param dir The file's directory.
 * @return The absolute path.
 */
inline std::string resolved(const std::string& text, const std::string& dir) {
  const std::filesystem::path path(text);
  return path.is_absolute() ?
    path.lexically_normal().string() :
    (std::filesystem::path(dir) / path).lexically_normal().string();
}

/**
 * @brief Whether a text is an order value: `[0-9]{1,14}(\.[0-9]{1,4})?`, above 0 and at most 10^13.
 * @param text The text.
 * @return Whether it is.
 */
inline bool order_value_text(const std::string& text) {
  const auto dot = text.find('.');
  const auto whole = text.substr(0, dot);
  const auto fraction = dot == std::string::npos ? std::string() : text.substr(dot + 1);
  const auto digits = [](const std::string& part) {
    return std::all_of(part.begin(), part.end(), [](char c) {
      return c >= '0' && c <= '9';
    });
  };
  if (whole.empty() || whole.size() > 14 || !digits(whole)) {
    return false;
  }
  if (dot != std::string::npos && (fraction.empty() || fraction.size() > 4 || !digits(fraction))) {
    return false;
  }
  const auto units = std::stoll(whole);
  const bool fraction_zero = fraction.find_first_not_of('0') == std::string::npos;
  if (units == 0 && fraction_zero) {
    return false;
  }
  return units < 10000000000000LL || (units == 10000000000000LL && fraction_zero);
}

/**
 * @brief Whether a KIS account number is written as the operator file takes it: eight digits, `-`, two digits.
 * @param text The number.
 * @return Whether it is.
 */
inline bool kis_account_number(const std::string& text) {
  if (text.size() != 11 || text[8] != '-') {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i != 8 && (text[i] < '0' || text[i] > '9')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Checks a parsed operator's file against every rule and builds the config.
 * @param root The parsed file.
 * @param config Where the config goes; its path and directory are already set.
 * @return The first refusal, or nothing.
 */
inline std::optional<Refusal> validate(const toml::table& root, OperatorConfig& config) {
  if (auto bad = only_keys(root, "", {"account", "atlc", "limits", "paper"})) {
    return bad;
  }
  std::optional<std::string> text;
  if (auto bad = string_of(root, "atlc", "atlc", text)) {
    return bad;
  }
  if (text) {
    if (text->empty()) {
      return refuse(root.get("atlc")->source(), "atlc must name a file");
    }
    config.atlc_ = resolved(*text, config.dir_);
  }

  const toml::table* account = nullptr;
  if (auto bad = table_of(root, "account", account)) {
    return bad;
  }
  if (auto bad = only_keys(*account, "account", {"broker", "env", "number"})) {
    return bad;
  }
  const auto* broker = account->get("broker");
  if (broker == nullptr) {
    return refuse(account->source(), "[account] needs broker = \"paper\"");
  }
  if (broker->as_string() == nullptr) {
    return refuse(broker->source(), "account.broker must be a string, not " + type_name(*broker));
  }
  config.broker_ = broker->as_string()->get();
  if (config.broker_ == "kiwoom" || config.broker_ == "toss") {
    return refuse(
      broker->source(),
      "the broker " + config.broker_ + " is not supported in this version; broker = \"kis\" or \"paper\" is",
      "unsupported.broker"
    );
  }
  if (config.broker_ != "paper" && config.broker_ != "kis") {
    return refuse(broker->source(), "account.broker must be \"kis\" or \"paper\", not \"" + config.broker_ + "\"");
  }
  text = std::nullopt;
  if (auto bad = string_of(*account, "env", "account.env", text)) {
    return bad;
  }
  if (config.broker_ == "paper") {
    if (text && *text != "paper") {
      return refuse(
        account->get("env")->source(),
        "account.env must be \"paper\" with the paper broker (demo and real come with the brokers that have them)"
      );
    }
    config.env_ = "paper";
  } else {
    // Demo unless real is written out: the library chooses the host from this alone.
    if (text && *text != "demo" && *text != "real") {
      return refuse(account->get("env")->source(), "account.env must be \"demo\" or \"real\", not \"" + *text + "\"");
    }
    config.env_ = text.value_or("demo");
  }
  text = std::nullopt;
  if (auto bad = string_of(*account, "number", "account.number", text)) {
    return bad;
  }
  if (!text && config.broker_ == "kis") {
    // The login names the account: one login per broker and environment, so the number is a pin, not a need.
    config.number_.clear();
  } else if (!text) {
    return refuse(account->source(), "[account] needs number, the account's number");
  }
  if (text) {
    const bool plain = !text->empty() && text->size() <= 64 && std::all_of(text->begin(), text->end(), [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (!plain) {
      return refuse(account->get("number")->source(), "account.number must be 1 to 64 of letters, digits, '_' or '-'");
    }
    if (config.broker_ == "kis" && !kis_account_number(*text)) {
      return refuse(
        account->get("number")->source(),
        "account.number for KIS is the eight-digit account and the two-digit product code, as \"12345678-01\""
      );
    }
    config.number_ = *text;
  }

  const toml::table* limits = nullptr;
  if (auto bad = table_of(root, "limits", limits)) {
    return bad;
  }
  if (
    auto bad = only_keys(
      *limits, "limits",
      {"collar", "daily_orders", "order_type", "order_value", "phases", "plan_ttl", "portfolio_age", "position",
        "symbols"}
    )
  ) {
    return bad;
  }
  auto& l = config.limits_;
  const auto* symbols = limits->get("symbols");
  if (symbols == nullptr) {
    return refuse(limits->source(), "[limits] needs symbols, the symbols that may be traded");
  }
  if (symbols->as_array() == nullptr) {
    return refuse(symbols->source(), "limits.symbols must be an array of strings, not " + type_name(*symbols));
  }
  const auto& symbol_array = *symbols->as_array();
  if (symbol_array.empty() || symbol_array.size() > 50) {
    return refuse(symbols->source(), "limits.symbols must list 1 to 50 symbols");
  }
  for (const auto& element : symbol_array) {
    const auto* symbol = element.as_string();
    if (symbol == nullptr) {
      return refuse(element.source(), "limits.symbols must hold strings, not " + type_name(element));
    }
    if (!paper::valid_symbol(symbol->get())) {
      return refuse(element.source(), "a symbol is 1 to 32 of letters, digits, '.', '_' or '-'");
    }
    if (std::find(l.symbols_.begin(), l.symbols_.end(), symbol->get()) != l.symbols_.end()) {
      return refuse(element.source(), "limits.symbols lists " + symbol->get() + " twice");
    }
    l.symbols_.push_back(symbol->get());
  }
  const auto* position = limits->get("position");
  if (position == nullptr) {
    return refuse(limits->source(), "[limits] needs position, the largest position of each symbol");
  }
  if (position->as_table() == nullptr) {
    return refuse(position->source(), "limits.position must be a table of integers, not " + type_name(*position));
  }
  for (const auto& entry : *position->as_table()) {
    const auto symbol = std::string(entry.first.str());
    if (!config.allows(symbol)) {
      return refuse(entry.first.source(), "limits.position names " + symbol + ", which limits.symbols does not list");
    }
    std::int64_t cap = 0;
    if (
      auto bad =
        integer_in(*position->as_table(), symbol.c_str(), "limits.position." + symbol, 0, paper::MAX_SHARES, cap)
    ) {
      return bad;
    }
    l.position_[symbol] = cap;
  }
  for (const auto& symbol : l.symbols_) {
    if (l.position_.count(symbol) == 0) {
      return refuse(position->source(), "limits.position has no entry for " + symbol + ": every symbol needs one");
    }
  }
  const auto* order_value = limits->get("order_value");
  if (order_value == nullptr) {
    return refuse(limits->source(), "[limits] needs order_value, the largest value of one order in KRW");
  }
  if (const auto* whole = order_value->as_integer()) {
    if (whole->get() <= 0 || whole->get() > 10000000000000LL) {
      return refuse(order_value->source(), "limits.order_value must be above 0 and at most 10000000000000");
    }
    l.order_value_ = std::to_string(whole->get());
  } else if (const auto* written = order_value->as_string()) {
    if (!order_value_text(written->get())) {
      return refuse(
        order_value->source(),
        "limits.order_value must be a decimal above 0 and at most 10000000000000, with at most four decimals"
      );
    }
    l.order_value_ = written->get();
  } else {
    return refuse(
      order_value->source(),
      "limits.order_value must be an integer, or a string holding a decimal, not " + type_name(*order_value)
    );
  }
  const auto* daily = limits->get("daily_orders");
  if (daily == nullptr) {
    return refuse(limits->source(), "[limits] needs daily_orders, the most placements per KST date");
  }
  if (auto bad = integer_in(*limits, "daily_orders", "limits.daily_orders", 0, 10000, l.daily_orders_)) {
    return bad;
  }
  text = std::nullopt;
  if (auto bad = string_of(*limits, "order_type", "limits.order_type", text)) {
    return bad;
  }
  if (text && *text != "limit") {
    return refuse(limits->get("order_type")->source(), "limits.order_type must be \"limit\"");
  }
  if (const auto* collar = limits->get("collar")) {
    const auto* value = collar->as_floating_point();
    if (value == nullptr) {
      return refuse(
        collar->source(),
        "limits.collar must be a float such as 0.05, not " + type_name(*collar) +
          (collar->is_integer() ? " (write it with a decimal point)" : "")
      );
    }
    if (!(value->get() > 0) || !(value->get() <= 0.30)) {
      return refuse(collar->source(), "limits.collar must be above 0 and at most 0.30");
    }
    l.collar_ = value->get();
  }
  if (const auto* phases = limits->get("phases")) {
    const auto* array = phases->as_array();
    if (array == nullptr) {
      return refuse(phases->source(), "limits.phases must be an array of strings, not " + type_name(*phases));
    }
    if (array->empty()) {
      return refuse(phases->source(), "limits.phases must list at least one phase");
    }
    l.phases_.clear();
    for (const auto& element : *array) {
      const auto* phase = element.as_string();
      if (phase == nullptr) {
        return refuse(element.source(), "limits.phases must hold strings, not " + type_name(element));
      }
      const auto& name = phase->get();
      if (name != "pre_open" && name != "open" && name != "post_close") {
        return refuse(element.source(), "a phase orders may go out in is pre_open, open or post_close, not " + name);
      }
      if (std::find(l.phases_.begin(), l.phases_.end(), name) != l.phases_.end()) {
        return refuse(element.source(), "limits.phases lists " + name + " twice");
      }
      l.phases_.push_back(name);
    }
  }
  if (auto bad = integer_in(*limits, "plan_ttl", "limits.plan_ttl", 1, 3600, l.plan_ttl_)) {
    return bad;
  }
  if (auto bad = integer_in(*limits, "portfolio_age", "limits.portfolio_age", 1, 86400, l.portfolio_age_)) {
    return bad;
  }

  if (config.broker_ != "paper") {
    // A broker's account has its own market: a [paper] table would only mislead.
    if (const auto* node = root.get("paper")) {
      return refuse(node->source(), "[paper] describes the paper market; a " + config.broker_ + " account has none");
    }
    return std::nullopt;
  }
  const toml::table* paper = nullptr;
  if (auto bad = table_of(root, "paper", paper)) {
    return bad;
  }
  if (auto bad = only_keys(*paper, "paper", {"bars", "cash", "holdings_lag", "listing_lag", "start"})) {
    return bad;
  }
  auto& p = config.paper_;
  text = std::nullopt;
  if (auto bad = string_of(*paper, "bars", "paper.bars", text)) {
    return bad;
  }
  if (!text || text->empty()) {
    return refuse(paper->source(), "[paper] needs bars, the CSV of daily bars");
  }
  p.bars_ = resolved(*text, config.dir_);
  if (paper->get("cash") == nullptr) {
    return refuse(paper->source(), "[paper] needs cash, the opening cash in KRW");
  }
  if (auto bad = integer_in(*paper, "cash", "paper.cash", 1, 10000000000000LL, p.cash_)) {
    return bad;
  }
  const auto* start = paper->get("start");
  if (start == nullptr) {
    return refuse(paper->source(), "[paper] needs start, the first instant (\"YYYY-MM-DDTHH:MM\", KST)");
  }
  if (start->is_date_time() || start->is_date() || start->is_time()) {
    return refuse(start->source(), "paper.start is written as a string, such as start = \"2024-03-25T09:30\"");
  }
  if (start->as_string() == nullptr) {
    return refuse(start->source(), "paper.start must be a string, not " + type_name(*start));
  }
  const auto at = paper::parse_kst(start->as_string()->get());
  if (!at) {
    return refuse(
      start->source(), "paper.start must be a real KST instant written YYYY-MM-DDTHH:MM, from 1990-01-01 to 2100-12-31"
    );
  }
  p.start_ = start->as_string()->get();
  p.start_at_ = *at;
  if (auto bad = integer_in(*paper, "listing_lag", "paper.listing_lag", 0, 86400, p.listing_lag_)) {
    return bad;
  }
  if (auto bad = integer_in(*paper, "holdings_lag", "paper.holdings_lag", 0, 86400, p.holdings_lag_)) {
    return bad;
  }
  return std::nullopt;
}

/** @brief The most segments a key or a table header may have; the operator's file needs two. */
inline constexpr int MAX_KEY_SEGMENTS = 8;

/** @brief The deepest arrays and inline tables may nest; the operator's file needs one level. */
inline constexpr int MAX_VALUE_NESTING = 8;

/**
 * @brief Refuses a character outside ASCII where TOML allows only ASCII, outside strings and comments, before toml++
 *   parses the text: toml++ 3.4 classifies some such characters (U+3001, U+FE70, ...) by a path it marks unreachable,
 *   which is undefined behaviour in an optimized build.
 * @param text The whole file.
 * @return A refusal at the character's line and column, or nothing.
 */
inline std::optional<Refusal> non_ascii_refusal(std::string_view text) {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  std::size_t i = 0;
  const auto at = [&text, &i](std::string_view token) {
    return text.compare(i, token.size(), token) == 0;
  };
  const auto step = [&]() {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte == '\n') {
      ++line;
      column = 1;
    } else if ((byte & 0xC0u) != 0x80u) {
      ++column;
    }
    ++i;
  };
  // Skips a string opened at `i`; a single-line one ends at its quote or at a new line, where toml++ refuses it.
  const auto skip_string = [&](char quote) {
    const bool escapes = quote == '"';
    const std::string_view triple = escapes ? std::string_view("\"\"\"") : std::string_view("'''");
    const bool multi = at(triple);
    for (std::size_t k = 0; k < (multi ? 3u : 1u); ++k) {
      step();
    }
    while (i < text.size()) {
      if (escapes && text[i] == '\\') {
        step();
        if (i < text.size()) {
          step();
        }
      } else if (multi && at(triple)) {
        for (int k = 0; k < 3; ++k) {
          step();
        }
        return;
      } else if (!multi && (text[i] == quote || text[i] == '\n')) {
        if (text[i] == quote) {
          step();
        }
        return;
      } else {
        step();
      }
    }
  };
  // A byte-order mark that opens the file is toml++'s to skip.
  if (at("\xEF\xBB\xBF")) {
    i = 3;
  }
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      skip_string(c);
    } else if (c == '#') {
      while (i < text.size() && text[i] != '\n') {
        step();
      }
    } else if ((static_cast<unsigned char>(c) & 0x80u) != 0) {
      return Refusal{
        "config.invalid", "a character outside ASCII outside a string or a comment, which TOML does not allow there",
        {line, column}
      };
    } else {
      step();
    }
  }
  return std::nullopt;
}

/**
 * @brief Bounds how deep the text can nest tables before toml++ parses it: toml++ recurses once per level, and a
 *   dotted key or header 16,000 segments deep, well under 64 KiB, exhausts the stack.
 * @details Scans outside strings and comments: a run of dots not broken by `=`, `,`, a bracket, a brace or a new line
 *   is one key path, of at most `MAX_KEY_SEGMENTS` segments; brackets and braces opened in a value nest at most
 *   `MAX_VALUE_NESTING` deep.
 *   A value has at most one dot outside a string (a float, a time), so no value the file can hold is refused.
 * @param text The whole file.
 * @return A refusal at the first segment or bracket past the bound, or nothing.
 */
inline std::optional<Refusal> nesting_refusal(std::string_view text) {
  std::uint32_t line = 1;
  std::uint32_t column = 0;
  int dots = 0;
  int depth = 0;
  bool header_line = false;
  bool line_blank = true;
  std::size_t i = 0;
  const auto advance = [&](std::size_t count) {
    for (std::size_t k = 0; k < count && i < text.size(); ++k, ++i) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if (byte == '\n') {
        ++line;
        column = 0;
        line_blank = true;
      } else if (byte != ' ' && byte != '\t' && byte != '\r') {
        line_blank = false;
      }
      if (byte != '\n' && (byte & 0xC0u) != 0x80u) {
        ++column;
      }
    }
  };
  const auto at = [&](std::string_view token) {
    return text.compare(i, token.size(), token) == 0;
  };
  // Skips a string opened at `i`, ending a single-line one at a new line as toml++ refuses it there.
  const auto skip_string = [&](char quote) {
    const bool escapes = quote == '"';
    const std::string_view triple = escapes ? std::string_view("\"\"\"") : std::string_view("'''");
    if (at(triple)) {
      advance(3);
      while (i < text.size()) {
        if (escapes && text[i] == '\\') {
          advance(2);
        } else if (at(triple)) {
          advance(3);
          // Up to two quotes may sit just inside the closing delimiter.
          for (int extra = 0; extra < 2 && i < text.size() && text[i] == quote; ++extra) {
            advance(1);
          }
          return;
        } else {
          advance(1);
        }
      }
      return;
    }
    advance(1);
    while (i < text.size() && text[i] != '\n') {
      if (escapes && text[i] == '\\') {
        advance(text.size() > i + 1 && text[i + 1] == '\n' ? 1 : 2);
      } else if (text[i] == quote) {
        advance(1);
        return;
      } else {
        advance(1);
      }
    }
  };
  const auto refuse_here = [&](std::string message) {
    toml::source_position where{};
    where.line = line;
    where.column = column + 1;
    return Refusal{"config.invalid", std::move(message), where};
  };
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      skip_string(c);
      continue;
    }
    if (c == '#') {
      while (i < text.size() && text[i] != '\n') {
        advance(1);
      }
      continue;
    }
    if (c == '.') {
      if (++dots >= MAX_KEY_SEGMENTS) {
        return refuse_here("a key or table header has more than " + std::to_string(MAX_KEY_SEGMENTS) + " segments");
      }
    } else if (c == '=' || c == ',' || c == '\n') {
      dots = 0;
      if (c == '\n' && header_line) {
        header_line = false;
        depth = 0;
      }
    } else if (c == '[' || c == '{') {
      dots = 0;
      // A header's brackets open a line outside any value; they name a table and do not nest a value.
      if (c == '[' && depth == 0 && !header_line && line_blank) {
        header_line = true;
      } else if (!header_line && ++depth > MAX_VALUE_NESTING) {
        return refuse_here(
          "arrays and inline tables nest more than " + std::to_string(MAX_VALUE_NESTING) + " levels deep"
        );
      }
    } else if (c == ']' || c == '}') {
      dots = 0;
      if (!header_line && depth > 0) {
        --depth;
      }
    }
    advance(1);
  }
  return std::nullopt;
}

} // namespace config_detail

/**
 * @brief Reads the operator's file named by `CMDATL_CONFIG`, once: opened without blocking on a FIFO, checked through
 *   its descriptor (a regular file of at most 64 KiB), read through the same descriptor, bounded in nesting, parsed
 *   by toml++ and checked.
 * @return The snapshot.
 */
inline ConfigSnapshot load_operator_config() {
  ConfigSnapshot snapshot;
  const char* named = std::getenv("CMDATL_CONFIG");
  if (named == nullptr || *named == '\0') {
    snapshot.state_ = ConfigSnapshot::State::UNSET;
    snapshot.error_ = {"config.missing", "CMDATL_CONFIG is not set: it names the operator's file", 0, 0};
    return snapshot;
  }
  snapshot.path_ = named;
  const auto invalid = [&snapshot](std::string message, std::uint32_t line = 0, std::uint32_t column = 0) {
    snapshot.state_ = ConfigSnapshot::State::INVALID;
    snapshot.error_ = {"config.invalid", std::move(message), line, column};
    return snapshot;
  };
#if defined(_WIN32)
  return invalid("the operator's file is read on macOS and Linux only in this version");
#else
  ScopedFd fd(open_lifted(named, O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (fd.get() < 0) {
    const int error = errno;
    if (error == ENOENT || error == ENOTDIR) {
      snapshot.state_ = ConfigSnapshot::State::MISSING;
      snapshot.error_ = {"config.missing", "CMDATL_CONFIG names " + snapshot.path_ + ", which does not exist", 0, 0};
      return snapshot;
    }
    return invalid("the operator's file " + snapshot.path_ + " cannot be opened: " + std::strerror(error));
  }
  std::string text;
  const auto read = read_whole(fd.get(), MAX_CONFIG_BYTES, text);
  if (read.status_ != FileRead::Status::OK) {
    return invalid("the operator's file " + snapshot.path_ + " was not read: " + read.text(MAX_CONFIG_BYTES));
  }
  OperatorConfig config;
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(snapshot.path_, ec);
  config.path_ = (ec ? std::filesystem::path(snapshot.path_) : absolute).lexically_normal().string();
  config.dir_ = std::filesystem::path(config.path_).parent_path().string();
  if (auto deep = config_detail::nesting_refusal(text)) {
    return invalid(
      "the operator's file " + snapshot.path_ + " is not TOML this version reads: " + deep->message_, deep->at_.line,
      deep->at_.column
    );
  }
  if (auto foreign = config_detail::non_ascii_refusal(text)) {
    return invalid(
      "the operator's file " + snapshot.path_ + " is not valid TOML: " + foreign->message_, foreign->at_.line,
      foreign->at_.column
    );
  }
  toml::table root;
  try {
    // Without a source path: toml++ copies one into a shared string in a constructor marked noexcept, so an
    // allocation failing there would end the process; the messages here name the file themselves.
    root = toml::parse(std::string_view(text));
  } catch (const TomlInvariant& error) {
    return invalid("the operator's file " + snapshot.path_ + " is not TOML this version reads (" + error.what() + ")");
  } catch (const toml::parse_error& error) {
    return invalid(
      "the operator's file " + snapshot.path_ + " is not valid TOML: " + std::string(error.description()),
      error.source().begin.line, error.source().begin.column
    );
  }
  if (auto refusal = config_detail::validate(root, config)) {
    snapshot.state_ = ConfigSnapshot::State::INVALID;
    snapshot.error_ = {
      refusal->code_,
      "the operator's file " + snapshot.path_ + ": " + refusal->message_,
      refusal->at_.line,
      refusal->at_.column,
    };
    return snapshot;
  }
  snapshot.state_ = ConfigSnapshot::State::OK;
  snapshot.config_ = std::move(config);
  return snapshot;
#endif
}

/**
 * @brief The reply for an operator's file that was not read: its code, with the file, the line and the column.
 * @param snapshot The snapshot, not `OK`.
 * @return The reply (exit 1).
 */
inline Reply config_reply(const ConfigSnapshot& snapshot) {
  const auto& error = snapshot.error_;
  std::string next = "the operator fixes their file; cmdatl env shows what was read";
  if (error.code_ == "config.missing") {
    next = "the operator sets CMDATL_CONFIG to their file";
  } else if (error.code_ == "unsupported.broker") {
    next = "this version trades the paper broker only";
  }
  auto reply = error_reply(Exit::FAILED, error.code_, error.message_, next);
  auto& object = reply.body_["error"].as_object();
  object["file"] = snapshot.path_.empty() ? json::value(nullptr) : json::value(snapshot.path_);
  object["line"] = error.line_ == 0 ? json::value(nullptr) : json::value(error.line_);
  object["column"] = error.column_ == 0 ? json::value(nullptr) : json::value(error.column_);
  return reply;
}

/**
 * @brief The root of every account's state: `$XDG_STATE_HOME/cmdatl` when that is set and absolute, else
 *   `$HOME/.local/state/cmdatl` when `HOME` is set and absolute.
 * @return The root, or nothing.
 */
inline std::optional<std::string> state_root() {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  if (xdg != nullptr && *xdg == '/') {
    return (std::filesystem::path(xdg) / "cmdatl").lexically_normal().string();
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home == '/') {
    return (std::filesystem::path(home) / ".local" / "state" / "cmdatl").lexically_normal().string();
  }
  return std::nullopt;
}

/**
 * @brief The account's state directory: `<root>/accounts/<broker>-<env>-<number>`.
 * @param config The operator's file.
 * @return The directory, or nothing when there is no state root.
 */
inline std::optional<std::string> account_dir(const OperatorConfig& config) {
  const auto root = state_root();
  if (!root) {
    return std::nullopt;
  }
  return (std::filesystem::path(*root) / "accounts" / config.account_name()).string();
}

/**
 * @brief A run's directory: `<root>/runs/<broker>-<env>-<number>/<run id>`, beside `accounts/`, never inside an
 *   account's directory, so a run's files never change how an account's state is classified.
 * @param config The operator's file.
 * @param run The run's id.
 * @return The directory, or nothing when there is no state root.
 */
inline std::optional<std::string> run_dir(const OperatorConfig& config, const std::string& run) {
  const auto root = state_root();
  if (!root) {
    return std::nullopt;
  }
  return (std::filesystem::path(*root) / "runs" / config.account_name() / run).string();
}

} // namespace cmdatl
