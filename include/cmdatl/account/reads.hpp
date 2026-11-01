#pragma once

// The account commands: `status`, `account`, `quote`, `order list`, `clock` and `clock advance`, as reply builders on
// an open account.
// Common reads go through the library's session; what only the paper market knows goes through the desk.
// Money and quantities are strings; every instant is KST with its offset.

#include "../core/table.hpp"
#include "./account.hpp"
#include "./capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace cmdatl {

/**
 * @brief What an owed row says to do, by what the market shows and the library's state.
 * @param symbol The symbol.
 * @param state The library's state: `named`, `unnamed` or `operator`.
 * @param status What the market shows for its order (`pending`, `filled`, `canceled`), or nothing.
 * @return The hint.
 */
inline std::string owed_next(
  const std::string& symbol, const std::string& state, const std::optional<std::string>& status
) {
  if (state == "operator") {
    return "only the operator settles it: cmdatl order release " + symbol + " with the approval file";
  }
  if (status && *status == "pending") {
    return "still working: cmdatl order cancel " + symbol +
      " withdraws it, or wait (cmdatl clock advance on paper); "
      "then cmdatl order reconcile " +
      symbol;
  }
  if (status) {
    return "cmdatl order reconcile " + symbol + " records it from holdings; sends nothing";
  }
  return "the broker never named it: cmdatl order reconcile " + symbol +
    "; if it stays owed, the operator reads the "
    "order list and holdings, then cmdatl order release " +
    symbol;
}

/**
 * @brief A whole amount as the reply's text.
 * @param value The amount.
 * @return The text.
 */
inline std::string money(std::int64_t value) {
  return std::to_string(value);
}

/**
 * @brief A reply carrying a read's body that cannot be affirmed: exit 4 `unknown.journal_damaged` when the journal is
 *   not intact, the body kept beside the error; the body alone otherwise.
 * @param account The account.
 * @param body The read's body.
 * @return The reply.
 */
inline Reply read_reply(const Account& account, json::object body) {
  if (account.journal_intact()) {
    Reply reply;
    reply.body_ = std::move(body);
    return reply;
  }
  auto reply = account.journal_damaged_reply();
  for (auto& entry : body) {
    reply.body_[entry.key()] = std::move(entry.value());
  }
  return reply;
}

/**
 * @brief The gates every placement shares, as `not_accepted` words in their fixed order.
 * @param readiness The gates.
 * @return The failing ones among `not_armed`, `orders_left_unknown`, `daily_orders_used`, `phase`.
 */
inline std::vector<std::string> account_gates(const Readiness& readiness) {
  std::vector<std::string> failing;
  if (!readiness.armed_) {
    failing.emplace_back("not_armed");
  }
  if (!readiness.orders_left_) {
    failing.emplace_back("orders_left_unknown");
  } else if (*readiness.orders_left_ <= 0) {
    failing.emplace_back("daily_orders_used");
  }
  if (!readiness.phase_permitted_) {
    failing.emplace_back("phase");
  }
  return failing;
}

/**
 * @brief Words as a JSON array.
 * @param words The words.
 * @return The array.
 */
inline json::array words_json(const std::vector<std::string>& words) {
  json::array out;
  for (const auto& word : words) {
    out.push_back(json::value(word));
  }
  return out;
}

/**
 * @brief The orders left today as JSON.
 * @param readiness The gates.
 * @return The count, or null when unknown.
 */
inline json::value orders_left_json(const Readiness& readiness) {
  return readiness.orders_left_ ? json::value(*readiness.orders_left_) : json::value(nullptr);
}

/**
 * @brief One quote row: the last price, the band, and whether an order on it passes the preliminary gates now.
 * @tparam Desk The desk type.
 * @param account The account.
 * @param desk The desk.
 * @param symbol The symbol, in the bars.
 * @return The row.
 */
template <typename Desk>
json::object quote_row(Account& account, Desk& desk, const std::string& symbol) {
  const auto& readiness = account.readiness();
  json::object row;
  row["symbol"] = symbol;
  const auto quote = desk.session().quote(paper::kr(symbol));
  row["last_price"] = quote ? json::value(quote.value().last_price_.str()) : json::value(nullptr);
  const auto band = desk.session().client().price_limits(paper::kr(symbol));
  const bool banded = band && band.value().upper_ && band.value().lower_;
  row["upper_limit"] = banded ? json::value(band.value().upper_->str()) : json::value(nullptr);
  row["lower_limit"] = banded ? json::value(band.value().lower_->str()) : json::value(nullptr);
  const bool allowed = account.config().allows(symbol);
  row["symbol_allowed"] = allowed;
  auto failing = account_gates(readiness);
  if (!desk.accepts_orders_at(symbol, desk.now())) {
    failing.emplace_back("market_closed");
  }
  if (!allowed) {
    failing.emplace_back("symbol_not_allowed");
  }
  if (!quote) {
    failing.emplace_back("no_price");
  }
  row["orders_accepted_now"] = failing.empty();
  row["not_accepted"] = words_json(failing);
  return row;
}

/**
 * @brief The holdings rows, optionally of one symbol.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @param only The symbol, or nothing for all.
 * @return The rows.
 */
template <typename Desk>
json::array positions_json(Desk& desk, const std::optional<std::string>& only) {
  const auto holdings = desk.session().holdings();
  if (!holdings) {
    throw std::runtime_error("the paper broker did not answer holdings: " + holdings.error().message_);
  }
  json::array rows;
  for (const auto& p : holdings.value().positions_) {
    if (only && p.instrument_.symbol_ != *only) {
      continue;
    }
    json::object row;
    row["symbol"] = p.instrument_.symbol_;
    row["quantity"] = p.quantity_.str();
    row["sellable"] = p.sellable_quantity_ ? json::value(p.sellable_quantity_->str()) : json::value(nullptr);
    row["average_price"] = p.average_purchase_price_.str();
    row["last_price"] = p.last_price_.str();
    row["market_value"] = p.market_value_.amount_.str();
    rows.push_back(std::move(row));
  }
  return rows;
}

/**
 * @brief The buying power as text.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @return The amount.
 */
template <typename Desk>
std::string buying_power_text(Desk& desk) {
  const auto power = desk.session().buying_power(algotradelib::Currency::krw());
  if (!power) {
    throw std::runtime_error("the paper broker did not answer buying power: " + power.error().message_);
  }
  return power.value().cash_.amount_.str();
}

/**
 * @brief The owed rows, optionally of one symbol: the library's state, and what the market shows.
 * @tparam Desk The desk type.
 * @param account The account.
 * @param desk The desk.
 * @param only The symbol, or nothing for all.
 * @return The rows.
 */
template <typename Desk>
json::array owed_json(const Account& account, Desk& desk, const std::optional<std::string>& only) {
  json::array rows;
  for (const auto& owed : account.journal().owed_) {
    if (only && owed.symbol_ != *only) {
      continue;
    }
    json::object row;
    row["symbol"] = owed.symbol_;
    row["side"] = owed.side_;
    row["quantity"] = owed.quantity_;
    row["journal_id"] = owed.journal_id_;
    row["state"] = owed.state_;
    row["order_id"] = owed.order_id_.empty() ? json::value(nullptr) : json::value(owed.order_id_);
    const auto status = owed.order_id_.empty() ? std::nullopt : desk.order_status(owed.order_id_);
    row["broker_status"] = status ? json::value(*status) : json::value(nullptr);
    row["next"] = owed_next(owed.symbol_, owed.state_, status);
    rows.push_back(std::move(row));
  }
  return rows;
}

/**
 * @brief The working-order rows.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @return The rows.
 */
template <typename Desk>
json::array orders_json(Desk& desk) {
  const auto working = desk.session().working_orders();
  if (!working) {
    throw std::runtime_error("the paper broker did not answer the order list: " + working.error().message_);
  }
  json::array rows;
  for (const auto& w : working.value().orders_) {
    const auto& o = w.order_;
    json::object row;
    row["order_id"] = o.order_id_;
    row["symbol"] = o.instrument_.symbol_;
    row["side"] = o.side_ == algotradelib::OrderSide::BUY ? "buy" : "sell";
    row["quantity"] = o.quantity_.str();
    row["filled"] = o.execution_.filled_quantity_.str();
    row["price"] = o.price_ ? json::value(o.price_->str()) : json::value(nullptr);
    row["status"] = o.status_ == algotradelib::OrderStatus::PARTIAL_FILLED ? "partial" : "pending";
    rows.push_back(std::move(row));
  }
  return rows;
}

/**
 * @brief Where the data ends: the last session's close.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @return The instant, KST with its offset.
 */
template <typename Desk>
std::string data_end_text(Desk& desk) {
  return paper::kst_iso(paper::open_of(desk.data_end()) + paper::SESSION_LENGTH);
}

/**
 * @brief `cmdatl status`: the whole picture in one read under one lock.
 * @tparam Desk The desk type.
 * @param invocation The arguments.
 * @param account The account.
 * @param desk The desk.
 * @param output The output.
 * @return The reply.
 */
template <typename Desk>
Reply status_reply(const Invocation& invocation, Account& account, Desk& desk, Output& output) {
  (void)invocation;
  (void)output;
  const auto& readiness = account.readiness();
  json::object body;
  body["now"] = paper::kst_iso(desk.now());
  body["phase"] = readiness.phase_;
  body["armed"] = readiness.armed_;
  auto failing = account_gates(readiness);
  const auto& symbols = account.config().limits_.symbols_;
  const bool any_open = std::any_of(symbols.begin(), symbols.end(), [&](const std::string& symbol) {
    return desk.accepts_orders_at(symbol, desk.now());
  });
  if (!any_open) {
    failing.emplace_back("market_closed");
  }
  body["orders_accepted_now"] = failing.empty();
  body["not_accepted"] = words_json(failing);
  body["orders_left_today"] = orders_left_json(readiness);
  body["positions"] = positions_json(desk, std::nullopt);
  body["cash"] = money(desk.paper_cash());
  body["buying_power"] = buying_power_text(desk);
  body["owed"] = owed_json(account, desk, std::nullopt);
  body["orders"] = orders_json(desk);
  json::array quotes;
  for (const auto& symbol : symbols) {
    quotes.push_back(quote_row(account, desk, symbol));
  }
  body["quotes"] = std::move(quotes);
  body["data_end"] = data_end_text(desk);
  body["capabilities"] = account_capabilities<std::decay_t<decltype(desk.session().client())>>(true);
  body["state_dir"] = account.dir();
  return read_reply(account, std::move(body));
}

/**
 * @brief `cmdatl account [--symbol SYMBOL]`: positions, cash, buying power, owed orders, orders left today.
 * @tparam Desk The desk type.
 * @param invocation The arguments.
 * @param account The account.
 * @param desk The desk.
 * @param output The output.
 * @return The reply.
 */
template <typename Desk>
Reply account_reply(const Invocation& invocation, Account& account, Desk& desk, Output& output) {
  (void)output;
  const auto only = invocation.has("symbol") ? std::optional<std::string>(invocation.get("symbol")) : std::nullopt;
  json::object body;
  body["now"] = paper::kst_iso(desk.now());
  body["positions"] = positions_json(desk, only);
  body["cash"] = money(desk.paper_cash());
  body["buying_power"] = buying_power_text(desk);
  body["owed"] = owed_json(account, desk, only);
  body["orders_left_today"] = orders_left_json(account.readiness());
  return read_reply(account, std::move(body));
}

/**
 * @brief `cmdatl quote SYMBOL...`: one row per symbol; a symbol with no rows in the bars answers `quote.no_data`,
 *   or `unknown.journal_damaged` naming it when the journal is not intact.
 * @tparam Desk The desk type.
 * @param invocation The arguments.
 * @param account The account.
 * @param desk The desk.
 * @param output The output.
 * @return The reply.
 */
template <typename Desk>
Reply quote_reply(const Invocation& invocation, Account& account, Desk& desk, Output& output) {
  (void)output;
  const auto found = invocation.values_.find("SYMBOL");
  const auto symbols = found == invocation.values_.end() ? std::vector<std::string>{} : found->second;
  std::vector<std::string> missing;
  for (const auto& symbol : symbols) {
    if (!desk.has_symbol(symbol) && std::find(missing.begin(), missing.end(), symbol) == missing.end()) {
      missing.push_back(symbol);
    }
  }
  if (!missing.empty()) {
    std::string names;
    for (const auto& symbol : missing) {
      names += (names.empty() ? "" : ", ") + symbol;
    }
    if (!account.journal_intact()) {
      // The journal's unknown state comes first; the missing symbols stay in its message, and no quote is given.
      auto reply = account.journal_damaged_reply();
      auto& error = reply.body_["error"].as_object();
      error["message"] =
        std::string(error["message"].as_string()) + "; the paper market's bars have no rows for " + names;
      return reply;
    }
    return error_reply(
      Exit::FAILED, "quote.no_data", "the paper market's bars have no rows for " + names,
      "cmdatl env shows the bars file"
    );
  }
  json::object body;
  body["now"] = paper::kst_iso(desk.now());
  body["phase"] = account.readiness().phase_;
  json::array quotes;
  for (const auto& symbol : symbols) {
    quotes.push_back(quote_row(account, desk, symbol));
  }
  body["quotes"] = std::move(quotes);
  return read_reply(account, std::move(body));
}

/**
 * @brief `cmdatl order list`: the working orders.
 * @tparam Desk The desk type.
 * @param invocation The arguments.
 * @param account The account.
 * @param desk The desk.
 * @param output The output.
 * @return The reply.
 */
template <typename Desk>
Reply order_list_reply(const Invocation& invocation, Account& account, Desk& desk, Output& output) {
  (void)invocation;
  (void)output;
  json::object body;
  body["now"] = paper::kst_iso(desk.now());
  body["orders"] = orders_json(desk);
  return read_reply(account, std::move(body));
}

/**
 * @brief `cmdatl clock`: the account's time, the phase and where the data ends.
 * @tparam Desk The desk type.
 * @param invocation The arguments.
 * @param account The account.
 * @param desk The desk.
 * @param output The output.
 * @return The reply.
 */
template <typename Desk>
Reply clock_reply(const Invocation& invocation, Account& account, Desk& desk, Output& output) {
  (void)invocation;
  (void)output;
  json::object body;
  body["now"] = paper::kst_iso(desk.now());
  body["phase"] = account.readiness().phase_;
  body["data_end"] = data_end_text(desk);
  return read_reply(account, std::move(body));
}

/**
 * @brief Reads a whole number written in digits only, within a range.
 * @param text The text.
 * @param low The lowest value.
 * @param high The highest value.
 * @return The number, or nothing.
 */
inline std::optional<std::int64_t> whole_in(const std::string& text, std::int64_t low, std::int64_t high) {
  if (text.empty() || text.size() > 6 || text.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const auto value = std::stoll(text);
  if (value < low || value > high) {
    return std::nullopt;
  }
  return value;
}

/**
 * @brief Checks `clock advance`'s arguments before the account is opened: exactly one of `--minutes N` (1-20160),
 *   `--hours N` (1-336), `--to TIME`, `--next-open`.
 * @param invocation The arguments.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> check_advance(const Invocation& invocation) {
  int given = 0;
  for (const char* name : {"minutes", "hours", "to", "next-open"}) {
    given += invocation.has(name) ? 1 : 0;
  }
  if (given != 1) {
    return error_reply(
      Exit::USAGE, "usage.bad_option", "clock advance takes exactly one of --minutes, --hours, --to and --next-open",
      "cmdatl clock advance --help"
    );
  }
  if (invocation.has("minutes") && !whole_in(invocation.get("minutes"), 1, 20160)) {
    return error_reply(Exit::USAGE, "usage.bad_value", "--minutes is a whole number from 1 to 20160", "");
  }
  if (invocation.has("hours") && !whole_in(invocation.get("hours"), 1, 336)) {
    return error_reply(Exit::USAGE, "usage.bad_value", "--hours is a whole number from 1 to 336", "");
  }
  if (invocation.has("to") && !paper::parse_kst(invocation.get("to"))) {
    return error_reply(
      Exit::USAGE, "usage.bad_value", "--to is a KST time written YYYY-MM-DDTHH:MM, from 1990-01-01 to 2100-12-31", ""
    );
  }
  return std::nullopt;
}

/**
 * @brief Checks that every `SYMBOL` and `--symbol` is a symbol, before the account is opened.
 * @param invocation The arguments.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> check_symbols(const Invocation& invocation) {
  for (const char* name : {"SYMBOL", "symbol"}) {
    const auto found = invocation.values_.find(name);
    if (found == invocation.values_.end()) {
      continue;
    }
    for (const auto& symbol : found->second) {
      if (!paper::valid_symbol(symbol)) {
        return error_reply(
          Exit::USAGE, "usage.bad_value",
          "'" + symbol + "' is not a symbol: 1 to 32 of letters, digits, '.', '_' or '-'", ""
        );
      }
    }
  }
  return std::nullopt;
}

/**
 * @brief What the market did since the last drain, as reply rows.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @return The rows: at, kind, order_id, symbol, side, quantity, and for a fill its price, commission and tax.
 */
template <typename Desk>
json::array events_json(Desk& desk) {
  json::array events;
  for (const auto& event : desk.drain_events()) {
    json::object row;
    row["at"] = paper::kst_iso(event.at_);
    row["kind"] = event.kind_;
    row["order_id"] = event.order_id_;
    row["symbol"] = event.symbol_;
    row["side"] = event.side_;
    row["quantity"] = std::to_string(event.quantity_);
    if (event.price_) {
      row["price"] = money(paper::amount_of(*event.price_));
      row["commission"] = money(paper::amount_of(event.commission_));
      row["tax"] = money(paper::amount_of(event.tax_));
    }
    events.push_back(std::move(row));
  }
  return events;
}

/**
 * @brief The reply after an advance whose line may have reached the disk, or did: its outcome is not known.
 * @param what What happened.
 * @return The reply (exit 4).
 */
inline Reply advance_unknown(const std::string& what) {
  return error_reply(
    Exit::UNKNOWN, "unknown.clock_advance", what + "; the clock may have moved",
    "cmdatl clock shows where the clock stands; do not repeat the advance before reading it"
  );
}

/**
 * @brief `cmdatl clock advance`: moves the paper clock, matching resting orders on the way.
 *
 * Refused before anything is appended (nothing changed): a target not after now, past the data, a journal that is
 * not intact, a lost lock.
 * A line not written whole leaves the clock where it was (exit 1); a whole line whose flush failed, or anything
 * failing after the flush, leaves the outcome unknown (exit 4).
 *
 * @tparam Desk The desk type.
 * @param invocation The arguments, checked by `check_advance`.
 * @param account The account.
 * @param desk The desk.
 * @param output The output.
 * @return The reply.
 */
template <typename Desk>
Reply advance_reply(const Invocation& invocation, Account& account, Desk& desk, Output& output) {
  (void)output;
  if (!account.journal_intact()) {
    return account.journal_damaged_reply();
  }
  const auto from = desk.now();
  paper::Clock::time_point target = from;
  if (invocation.has("minutes")) {
    target = from + std::chrono::minutes(*whole_in(invocation.get("minutes"), 1, 20160));
  } else if (invocation.has("hours")) {
    target = from + std::chrono::hours(*whole_in(invocation.get("hours"), 1, 336));
  } else if (invocation.has("to")) {
    target = *paper::parse_kst(invocation.get("to"));
  } else {
    const auto next = desk.next_session_after(from);
    if (!next) {
      return error_reply(
        Exit::FAILED, "clock.past_data",
        "no session after " + paper::kst_iso(from) + " in the paper market's data, which ends " + data_end_text(desk),
        "cmdatl clock shows where the data ends"
      );
    }
    target = *next;
  }
  if (target <= from) {
    return error_reply(
      Exit::FAILED, "clock.backwards", "the clock is at " + paper::kst_iso(from) + " and only moves forward",
      "cmdatl clock shows where the clock stands"
    );
  }
  if (target > desk.clock_limit()) {
    return error_reply(
      Exit::FAILED, "clock.past_data",
      "the paper market's data ends " + data_end_text(desk) + "; the clock goes at most a day past it",
      "cmdatl clock shows where the data ends"
    );
  }
  const auto outcome = desk.advance_to(target);
  switch (outcome.status_) {
    case paper::Appended::OK:
      break;
    case paper::Appended::LOCK_LOST:
      return lock_lost_reply();
    case paper::Appended::NOT_WRITTEN:
      return error_reply(
        Exit::FAILED, "file.unwritable",
        "the advance could not be written to the paper log (" + std::string(std::strerror(outcome.error_)) +
          "); the clock has not moved",
        "check the account's directory"
      );
    case paper::Appended::NOT_SYNCED:
      return advance_unknown(
        "the advance was written but not flushed to disk (" + std::string(std::strerror(outcome.error_)) + ")"
      );
  }
  // Committed: from here a failure leaves the outcome unknown, never a plain failure.
  try {
    json::object body;
    body["from"] = paper::kst_iso(from);
    body["now"] = paper::kst_iso(desk.now());
    body["phase"] = phase_of(desk);
    auto events = events_json(desk);
    body["events"] = std::move(events);
    Reply reply;
    reply.body_ = std::move(body);
    return reply;
  } catch (const std::exception& error) {
    return advance_unknown(std::string("the advance was applied but its reply failed: ") + error.what());
  }
}

} // namespace cmdatl
