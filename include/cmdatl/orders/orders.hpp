#pragma once

// The order commands: `order plan`, `order place`, `order cancel`, `order reconcile` and `order release`, as reply
// builders on an open account.
//
// Every one of them, in this order: its arguments by their grammar before the account opens; the account opened under
// its lock; one simulated second, whose market events are the reply's `events`; its own checks; an interrupt noted
// before once() is called answers exit 130; the journal checked to be the file the account opened and opened as the
// one writer; once() or release(); the view read again for the reply's `owed`.
// Nothing places or cancels except through once().

#include "../account/account.hpp"
#include "../account/reads.hpp"
#include "../core/errors.hpp"
#include "../core/table.hpp"
#include "../system/process.hpp"
#include "../system/syscalls.hpp"
#include "./once.hpp"
#include "./plans.hpp"
#include "./rails.hpp"

#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace cmdatl {

/** @brief The exit codes of an order command. */
inline const std::vector<Exit> ORDER_EXITS{
  Exit::OK, Exit::FAILED, Exit::USAGE, Exit::REFUSED, Exit::UNKNOWN, Exit::INTERRUPTED
};

/** @brief The error codes the order commands' own checks answer with, beside once()'s. */
inline const std::vector<std::string> ORDER_ERRORS{
  "usage.bad_value",
  "clock.past_data",
  "refused.symbol_not_allowed",
  "refused.position_limit",
  "refused.daily_order_limit",
  "refused.order_value",
};

/**
 * @brief Every error code an order command can answer with beyond opening the account: its own checks', once()'s and
 *   release()'s.
 * @return The codes.
 */
inline std::vector<std::string> order_error_codes() {
  auto codes = ORDER_ERRORS;
  for (
    const auto* code :
    {"refused.not_armed", "refused.no_plan", "refused.plan_used", "refused.stale_plan", "refused.holding_moved",
      "refused.awaiting", "unknown.journal_missing"}
  ) {
    codes.emplace_back(code);
  }
  for (const auto& reason : sending::ONCE_REASONS) {
    if (reason.exit_ == sending::OnceExit::DONE) {
      continue;
    }
    const auto code =
      std::string(reason.exit_ == sending::OnceExit::REFUSED ? "refused." : "unknown.") + reason.reason_;
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
      codes.push_back(code);
    }
  }
  return codes;
}

/**
 * @brief The error code of one of once()'s reasons.
 * @param exit The exit.
 * @param reason The reason.
 * @return `refused.<reason>`, `unknown.<reason>` (`unknown.internal` for an exception), or empty on exit 0.
 */
inline std::string once_code(sending::OnceExit exit, const std::string& reason) {
  switch (exit) {
    case sending::OnceExit::DONE:
      return {};
    case sending::OnceExit::REFUSED:
      return "refused." + reason;
    case sending::OnceExit::UNKNOWN:
      return "unknown." + reason;
  }
  return "unknown.unmapped";
}

/**
 * @brief Whether a text is a reason: 1-200 bytes of printable UTF-8, not only spaces.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_reason(std::string_view text) {
  if (text.empty() || text.size() > 200 || text.find_first_not_of(' ') == std::string_view::npos) {
    return false;
  }
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7f) {
        return false;
      }
      ++i;
      continue;
    }
    std::size_t more = 0;
    std::uint32_t point = 0;
    if ((c & 0xe0) == 0xc0) {
      more = 1;
      point = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      more = 2;
      point = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      more = 3;
      point = c & 0x07;
    } else {
      return false;
    }
    if (i + more >= text.size()) {
      return false;
    }
    for (std::size_t k = 1; k <= more; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      point = (point << 6) | (next & 0x3f);
    }
    const std::uint32_t least[] = {0, 0x80, 0x800, 0x10000};
    if (
      point < least[more] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) || (point >= 0x80 && point < 0xa0)
    ) {
      return false;
    }
    i += more + 1;
  }
  return true;
}

/**
 * @brief Checks an order command's arguments before the account opens: `SYMBOL`, `--target`, `--limit`, `PLAN_ID` and
 *   `--reason`, each by its grammar.
 * @param invocation The arguments.
 * @return The refusal (exit 2), or nothing.
 */
inline std::optional<Reply> check_order_args(const Invocation& invocation) {
  if (auto refused = check_symbols(invocation)) {
    return refused;
  }
  const auto bad = [](const std::string& message) {
    return error_reply(Exit::USAGE, "usage.bad_value", message, "");
  };
  if (invocation.has("target") && !valid_target(invocation.get("target"))) {
    return bad("--target is whole shares written 0 or 1 to 999999999, with no sign, fraction or leading zero");
  }
  if (invocation.has("limit") && !valid_limit(invocation.get("limit"))) {
    return bad("--limit is a price from 1 to 10000000 with at most four decimals");
  }
  if (invocation.has("PLAN_ID") && !valid_plan_id(invocation.get("PLAN_ID"))) {
    return bad("a plan id is p and ten hex digits, as order plan gives it");
  }
  if (invocation.has("reason") && !valid_reason(invocation.get("reason"))) {
    return bad("--reason is 1 to 200 bytes of printable text, not only spaces");
  }
  return std::nullopt;
}

/**
 * @brief once()'s options as the operator's file sets them.
 * @param config The operator's file.
 * @param limits The risk limits.
 * @return The options, with the journal-related walls on.
 */
inline sending::OnceConfig once_config(const OperatorConfig& config, algotradelib::strategy::RiskLimits limits) {
  sending::OnceConfig once;
  once.armed_file_ = config.armed_path();
  once.limits_ = std::move(limits);
  once.collar_ = config.limits_.collar_;
  once.maximum_portfolio_age_ = std::chrono::seconds(config.limits_.portfolio_age_);
  once.confirmation_timeout_ = std::chrono::minutes(10);
  once.refuse_unjournaled_orders_ = true;
  return once;
}

/** @brief The state of one symbol after `order reconcile`, and its exit. */
struct ReconcileRow {
  /** @brief `clean`, `working`, `holding_moved`, or once()'s reason. */
  std::string state_;
  /** @brief The row's exit. */
  Exit exit_{Exit::OK};
  /** @brief The row's error code, when not 0. */
  std::string code_;
};

/**
 * @brief The row of a reconcile's dry run: `clean` when met, `working` when it would withdraw (an order rests),
 *   `holding_moved` (3) when it would buy or sell, once()'s refusal (3) or unknown (4) otherwise.
 * @param result once()'s answer.
 * @return The row.
 */
inline ReconcileRow reconcile_row(const sending::OnceResult& result) {
  if (result.exit_ == sending::OnceExit::DONE) {
    if (result.reason_ == "plan") {
      if (result.plan_kind_ == "withdraw") {
        return {"working", Exit::OK, ""};
      }
      return {"holding_moved", Exit::REFUSED, "refused.holding_moved"};
    }
    return {"clean", Exit::OK, ""};
  }
  const auto exit = result.exit_ == sending::OnceExit::REFUSED ? Exit::REFUSED : Exit::UNKNOWN;
  return {result.reason_, exit, once_code(result.exit_, result.reason_)};
}

/**
 * @brief The overall exit of a reconcile: 4 when any row is 4 (the first such row's code), else 3 when any row is 3
 *   (the first's code) or anything is still owed (`refused.awaiting`), else 0.
 * @param rows The rows.
 * @param owed_after Whether anything is owed after the call.
 * @return The exit and its code (empty on 0).
 */
inline std::pair<Exit, std::string> reconcile_exit(const std::vector<ReconcileRow>& rows, bool owed_after) {
  for (const auto& row : rows) {
    if (row.exit_ == Exit::UNKNOWN) {
      return {Exit::UNKNOWN, row.code_};
    }
  }
  for (const auto& row : rows) {
    if (row.exit_ == Exit::REFUSED) {
      return {Exit::REFUSED, row.code_};
    }
  }
  if (owed_after) {
    return {Exit::REFUSED, "refused.awaiting"};
  }
  return {Exit::OK, ""};
}

namespace orders_detail {

/**
 * @brief A reply's body merged into a reply.
 * @param reply The reply.
 * @param body The fields, which do not replace the reply's own.
 * @return The reply.
 */
inline Reply with_body(Reply reply, json::object body) {
  for (auto& entry : body) {
    if (!reply.body_.contains(entry.key())) {
      reply.body_[entry.key()] = std::move(entry.value());
    }
  }
  return reply;
}

/**
 * @brief A refusal or unknown by code, its `next` from the catalog, with the body's fields.
 * @param exit The exit.
 * @param code The code.
 * @param message What happened.
 * @param body The fields.
 * @return The reply.
 */
inline Reply coded(Exit exit, const std::string& code, const std::string& message, json::object body) {
  bool retry = false;
  for (const auto& entry : error_catalog()) {
    if (entry.code_ == code) {
      retry = entry.retry_;
    }
  }
  return with_body(error_reply(exit, code, message, catalog_next(code), retry), std::move(body));
}

/**
 * @brief once()'s answer as a reply: `reason` and `result` on exit 0, the coded error otherwise.
 * @param result The answer.
 * @param body The fields.
 * @return The reply.
 */
inline Reply from_once(const sending::OnceResult& result, json::object body) {
  if (!result.phase_.empty()) {
    body["phase"] = result.phase_;
  }
  if (result.exit_ == sending::OnceExit::DONE) {
    body["reason"] = result.reason_;
    body["detail"] = result.detail_;
    body["result"] = "done";
    Reply reply;
    reply.body_ = std::move(body);
    return reply;
  }
  const auto exit = result.exit_ == sending::OnceExit::REFUSED ? Exit::REFUSED : Exit::UNKNOWN;
  body["reason"] = result.reason_;
  return coded(exit, once_code(result.exit_, result.reason_), result.detail_, std::move(body));
}

/**
 * @brief The holding of a symbol, from the session's holdings.
 * @tparam Desk The desk.
 * @param desk The desk.
 * @param symbol The symbol.
 * @return The quantity (0 when none), or nothing when holdings could not be read.
 */
template <typename Desk>
std::optional<algotradelib::Decimal64> held_of(Desk& desk, const std::string& symbol) {
  const auto holdings = desk.session().holdings();
  if (!holdings) {
    return std::nullopt;
  }
  algotradelib::Decimal64 held(0, 0);
  for (const auto& position : holdings.value().positions_) {
    if (position.instrument_.symbol_ == symbol) {
      held = position.quantity_;
    }
  }
  return held;
}

/**
 * @brief The journal's size in bytes, by `lstat`.
 * @param account The account.
 * @return The size, or nothing.
 */
inline std::optional<std::int64_t> journal_size(const Account& account) {
#if !defined(_WIN32)
  struct stat info{};
  if (::lstat(account.journal_path().c_str(), &info) != 0) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(info.st_size);
#else
  (void)account;
  return std::nullopt;
#endif
}

/** @brief The start every order command shares: the journal intact, and one simulated second. */
struct Started {
  /** @brief The refusal, when the command stops here. */
  std::optional<Reply> refusal_;
  /** @brief The reply's fields so far: `now` and `events`. */
  json::object body_;
};

/**
 * @brief Refuses a journal that is not intact, then takes the per-call second: the clock moved one second, and what
 *   the market did in it as `events`.
 * @param account The account.
 * @return The start.
 */
inline Started start(Account& account) {
  Started started;
  if (!account.journal_intact()) {
    started.refusal_ = account.journal_damaged_reply();
    return started;
  }
  auto& desk = account.desk();
  const auto target = desk.now() + std::chrono::seconds(1);
  if (target > desk.clock_limit()) {
    started.refusal_ = error_reply(
      Exit::FAILED, "clock.past_data",
      "the paper market's data ends " + data_end_text(desk) + "; an order command takes one simulated second, past it",
      "cmdatl clock shows where the data ends"
    );
    return started;
  }
  switch (desk.advance_to(target)) {
    case Moved::OK:
      break;
    case Moved::LOCK_LOST:
      started.refusal_ = lock_lost_reply();
      return started;
    case Moved::NOT_WRITTEN:
      started.refusal_ = error_reply(
        Exit::FAILED, "file.unwritable",
        "the command's simulated second could not be written to the account's state file; the clock has not moved "
        "and nothing was sent",
        "check the account's directory"
      );
      return started;
    case Moved::UNCONFIRMED:
      started.refusal_ = error_reply(
        Exit::FAILED, "file.unwritable",
        "the command's simulated second was written but not flushed; the paper clock may have moved one second and "
        "nothing was sent",
        "check the account's directory"
      );
      return started;
    case Moved::HALTED:
      started.refusal_ = paper_state_reply(desk.halted().value_or("the paper market halted") + "; nothing was sent");
      return started;
  }
  started.body_["now"] = paper::kst_iso(desk.now());
  started.body_["events"] = events_json(desk);
  return started;
}

/**
 * @brief The refusal after SIGINT or SIGTERM before once() was called.
 * @param body The fields.
 * @return The reply (exit 130).
 */
inline Reply interrupted_before_send(json::object body) {
  return with_body(
    error_reply(
      Exit::INTERRUPTED, "interrupted", "interrupted by a signal before anything could be sent",
      "run the command again", true
    ),
    std::move(body)
  );
}

/**
 * @brief The reply's last fields: the view read again (`owed`), and the lock checked.
 * @param account The account.
 * @param reply The reply.
 * @param symbol The symbol whose owed rows the reply carries, or nothing for all.
 * @return The reply.
 */
inline Reply finish(Account& account, Reply reply, const std::optional<std::string>& symbol) {
#if !defined(_WIN32)
  account.refresh_view();
#endif
  reply.body_["owed"] = owed_json(account, account.desk(), symbol);
  if (!account.lock().still_at_path()) {
    return lock_lost_reply();
  }
  return reply;
}

/** @brief A precheck's refusal. */
struct Precheck {
  /** @brief The code, or empty when it passes. */
  std::string code_;
  /** @brief What failed. */
  std::string message_;
};

/**
 * @brief The checks of a request against the operator's file and the book, in their order.
 * @param account The account.
 * @param symbol The symbol.
 * @param target The target, as typed.
 * @param limit The limit, as typed.
 * @param need_armed Whether the armed file is required (a place).
 * @param held Set to the holding.
 * @return The first refusal, or none.
 */
inline Precheck precheck(
  Account& account, const std::string& symbol, const std::string& target, const std::string& limit, bool need_armed,
  std::optional<algotradelib::Decimal64>& held
) {
  namespace atl = algotradelib;
  const auto& config = account.config();
  if (!config.allows(symbol)) {
    return {"refused.symbol_not_allowed", symbol + " is not in the operator's limits.symbols"};
  }
  const auto wanted = std::stoll(target);
  const auto cap = config.limits_.position_.find(symbol);
  if (cap == config.limits_.position_.end() || wanted > cap->second) {
    return {
      "refused.position_limit",
      "target " + target + " exceeds the operator's limit of " +
        (cap == config.limits_.position_.end() ? std::string("0") : std::to_string(cap->second)) + " for " + symbol
    };
  }
  if (need_armed) {
    std::error_code ec;
    if (!std::filesystem::exists(config.armed_path(), ec)) {
      return {"refused.not_armed", "the operator's armed file does not exist: " + config.armed_path()};
    }
  }
  held = held_of(account.desk(), symbol);
  if (!held) {
    return {"refused.holdings_unread", "the holdings could not be read"};
  }
  const auto change = std::llabs(wanted - std::stoll(held->str()));
  if (change == 0) {
    return {};
  }
  const auto left = orders_left(config, account.journal());
  if (!left || *left == 0) {
    return {
      "refused.daily_order_limit",
      std::to_string(account.journal().placed_today_) + " placements today; the operator allows " +
        std::to_string(config.limits_.daily_orders_)
    };
  }
  const auto value = atl::decimal_multiply(atl::Decimal64(change, 0), atl::Decimal64::parse(limit));
  const auto ceiling = atl::Decimal64::parse(config.limits_.order_value_);
  if (!value || atl::decimal_less(ceiling, *value)) {
    return {
      "refused.order_value",
      "an order of " + std::to_string(change) + " shares at " + limit + " is " +
        (value ? value->str() : std::string("too large")) + " KRW; the operator allows " + config.limits_.order_value_
    };
  }
  return {};
}

} // namespace orders_detail

/**
 * @brief `cmdatl order plan SYMBOL --target N --limit P`: what `order place` would send, checked, with a plan id.
 * @param invocation The arguments, checked by `check_order_args`.
 * @param account The account.
 * @param output The output.
 * @return The reply.
 */
inline Reply plan_reply(const Invocation& invocation, Account& account, Output& output) {
  (void)output;
  using namespace orders_detail;
  auto started = start(account);
  if (started.refusal_) {
    return std::move(*started.refusal_);
  }
  auto body = std::move(started.body_);
  const auto symbol = invocation.get("SYMBOL");
  const auto target = invocation.get("target");
  const auto limit = invocation.get("limit");
  body["symbol"] = symbol;
  body["target"] = target;
  body["limit"] = limit;
  std::optional<algotradelib::Decimal64> held;
  if (const auto refused = precheck(account, symbol, target, limit, false, held); !refused.code_.empty()) {
    return finish(account, coded(Exit::REFUSED, refused.code_, refused.message_, std::move(body)), symbol);
  }
  if (interrupted()) {
    return interrupted_before_send(std::move(body));
  }
  sending::OnceResult result;
  {
    auto opened = open_journal(account);
    if (opened.refusal_) {
      return with_body(std::move(*opened.refusal_), std::move(body));
    }
    const auto left = orders_left(account.config(), account.journal()).value_or(0);
    auto config = once_config(account.config(), risk_limits(account.config(), {symbol}, left));
    config.dry_run_ = true;
    sending::OnceRequest request;
    request.instrument_ = paper::kr(symbol);
    request.target_quantity_ = algotradelib::Decimal64::parse(target);
    request.limit_ = algotradelib::Decimal64::parse(limit);
    result = sending::once(account.desk(), *opened.journal_, account.lock(), request, config);
  }
  if (held) {
    body["held"] = held->str();
  }
  if (result.exit_ == sending::OnceExit::DONE && result.reason_ == "plan") {
    auto book = PlanBook::load(account.dir());
    Plan plan;
    plan.symbol_ = symbol;
    plan.target_ = target;
    plan.limit_ = limit;
    plan.snapshot_ =
      snapshot_of(account.desk().session(), account.config().scope(), symbol, target, limit, journal_size(account));
    plan.sim_ = paper::nanos(account.desk().now());
    const auto id = book.issue(plan);
    const auto saved = book.save(account.lock());
    if (saved.lock_lost_) {
      return with_body(lock_lost_reply(), std::move(body));
    }
    if (saved.error_ != 0) {
      return finish(
        account,
        with_body(
          error_reply(
            Exit::FAILED, "file.unwritable",
            "the plan book could not be written (" + std::string(std::strerror(saved.error_)) +
              "); no plan was issued and nothing was sent",
            "check the account's directory"
          ),
          std::move(body)
        ),
        symbol
      );
    }
    body["plan_id"] = id;
    body["would"] = result.plan_kind_;
    if (result.plan_kind_ != "withdraw" && held) {
      body["quantity"] = std::to_string(std::llabs(std::stoll(target) - std::stoll(held->str())));
    }
    body["expires_at"] =
      paper::kst_iso(account.desk().now() + std::chrono::seconds(account.config().limits_.plan_ttl_));
    body["next"] = "cmdatl order place " + id;
  }
  return finish(account, from_once(result, std::move(body)), symbol);
}

/**
 * @brief `cmdatl order place PLAN_ID`: sends the plan, if nothing moved since and the operator's file still allows it.
 * @param invocation The arguments, checked by `check_order_args`.
 * @param account The account.
 * @param output The output.
 * @return The reply.
 */
inline Reply place_reply(const Invocation& invocation, Account& account, Output& output) {
  (void)output;
  using namespace orders_detail;
  auto started = start(account);
  if (started.refusal_) {
    return std::move(*started.refusal_);
  }
  auto body = std::move(started.body_);
  const auto id = invocation.get("PLAN_ID");
  body["plan_id"] = id;
  auto book = PlanBook::load(account.dir());
  auto plan = book.find(id);
  const auto now_nanos = paper::nanos(account.desk().now());
  if (!plan || plan->sim_ > now_nanos) {
    return finish(
      account,
      coded(
        Exit::REFUSED, "refused.no_plan",
        book.damaged() || plan ? "the plan book is damaged and is read as empty" : "the plan book holds no plan " + id,
        std::move(body)
      ),
      std::nullopt
    );
  }
  body["symbol"] = plan->symbol_;
  body["target"] = plan->target_;
  body["limit"] = plan->limit_;
  std::optional<algotradelib::Decimal64> held;
  if (
    const auto refused = precheck(account, plan->symbol_, plan->target_, plan->limit_, true, held);
    !refused.code_.empty()
  ) {
    return finish(account, coded(Exit::REFUSED, refused.code_, refused.message_, std::move(body)), plan->symbol_);
  }
  if (plan->used_) {
    return finish(
      account, coded(Exit::REFUSED, "refused.plan_used", "a plan sends once; plan again", std::move(body)),
      plan->symbol_
    );
  }
  const auto snapshot = snapshot_of(
    account.desk().session(), account.config().scope(), plan->symbol_, plan->target_, plan->limit_,
    journal_size(account)
  );
  const auto age = now_nanos - plan->sim_;
  const auto ttl =
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(account.config().limits_.plan_ttl_))
      .count();
  if (snapshot != plan->snapshot_ || age > ttl) {
    return finish(
      account,
      coded(
        Exit::REFUSED, "refused.stale_plan",
        snapshot != plan->snapshot_ ?
          "the account moved since the plan was made" :
          "the plan is older than limits.plan_ttl",
        std::move(body)
      ),
      plan->symbol_
    );
  }
  if (interrupted()) {
    return interrupted_before_send(std::move(body));
  }
  book.use(id);
  const auto saved = book.save(account.lock());
  if (saved.lock_lost_) {
    // Nothing published: the plan book is the newer holder's, and once() is never called.
    return with_body(lock_lost_reply(), std::move(body));
  }
  if (saved.error_ != 0) {
    return finish(
      account,
      with_body(
        error_reply(
          Exit::FAILED, "file.unwritable",
          "the plan book could not be written (" + std::string(std::strerror(saved.error_)) +
            "); the plan is unused and nothing was sent",
          "check the account's directory"
        ),
        std::move(body)
      ),
      plan->symbol_
    );
  }
  sys::kill_point("order.before_once");
  if (interrupted()) {
    // The plan is used and nothing was sent: a new plan works.
    return finish(account, interrupted_before_send(std::move(body)), plan->symbol_);
  }
  sending::OnceResult result;
  {
    auto opened = open_journal(account);
    if (opened.refusal_) {
      return with_body(std::move(*opened.refusal_), std::move(body));
    }
    const auto left = orders_left(account.config(), account.journal()).value_or(0);
    const auto config = once_config(account.config(), risk_limits(account.config(), {plan->symbol_}, left));
    sending::OnceRequest request;
    request.instrument_ = paper::kr(plan->symbol_);
    request.target_quantity_ = algotradelib::Decimal64::parse(plan->target_);
    request.limit_ = algotradelib::Decimal64::parse(plan->limit_);
    result = sending::once(account.desk(), *opened.journal_, account.lock(), request, config);
  }
  sys::kill_point("order.after_once");
  if (result.order_) {
    json::object order;
    order["order_id"] = result.order_->order_id_.empty() ? json::value(nullptr) : json::value(result.order_->order_id_);
    order["side"] = result.order_->side_;
    order["quantity"] = result.order_->quantity_;
    order["limit"] = result.order_->limit_;
    body["order"] = std::move(order);
  }
  if (!result.withdrawn_.empty()) {
    body["withdrawals"] = words_json(result.withdrawn_);
    if (result.reason_ == "withdrawal_sent") {
      body["next"] = "plan again once cmdatl order list shows them gone";
    }
  }
  return finish(account, from_once(result, std::move(body)), plan->symbol_);
}

/**
 * @brief `cmdatl order cancel SYMBOL`: withdraws every working order on the symbol; never places, needs no armed file,
 *   runs in any phase.
 * @param invocation The arguments.
 * @param account The account.
 * @param output The output.
 * @return The reply.
 */
inline Reply cancel_reply(const Invocation& invocation, Account& account, Output& output) {
  (void)output;
  using namespace orders_detail;
  auto started = start(account);
  if (started.refusal_) {
    return std::move(*started.refusal_);
  }
  auto body = std::move(started.body_);
  const auto symbol = invocation.get("SYMBOL");
  body["symbol"] = symbol;
  if (!account.desk().has_symbol(symbol)) {
    return with_body(
      error_reply(
        Exit::FAILED, "quote.no_data", "the paper market's bars have no rows for " + symbol,
        "cmdatl env shows the bars file"
      ),
      std::move(body)
    );
  }
  if (interrupted()) {
    return interrupted_before_send(std::move(body));
  }
  sending::OnceResult result;
  {
    auto opened = open_journal(account);
    if (opened.refusal_) {
      return with_body(std::move(*opened.refusal_), std::move(body));
    }
    auto config = once_config(account.config(), risk_limits(account.config(), {}, 0));
    config.withdraw_only_ = true;
    config.limits_.max_orders_per_window_ = 0;
    sending::OnceRequest request;
    request.instrument_ = paper::kr(symbol);
    request.target_quantity_ = algotradelib::Decimal64(0, 0);
    result = sending::once(account.desk(), *opened.journal_, account.lock(), request, config);
  }
  body["withdrawals"] = words_json(result.withdrawn_);
  return finish(account, from_once(result, std::move(body)), symbol);
}

/**
 * @brief `cmdatl order reconcile [SYMBOL]`: records from holdings what the journal owes, sending nothing.
 * @param invocation The arguments.
 * @param account The account.
 * @param output The output.
 * @return The reply.
 */
inline Reply reconcile_reply(const Invocation& invocation, Account& account, Output& output) {
  (void)output;
  using namespace orders_detail;
  auto started = start(account);
  if (started.refusal_) {
    return std::move(*started.refusal_);
  }
  auto body = std::move(started.body_);
  std::vector<std::string> symbols;
  if (invocation.has("SYMBOL")) {
    const auto symbol = invocation.get("SYMBOL");
    if (!account.desk().has_symbol(symbol)) {
      return with_body(
        error_reply(
          Exit::FAILED, "quote.no_data", "the paper market's bars have no rows for " + symbol,
          "cmdatl env shows the bars file"
        ),
        std::move(body)
      );
    }
    symbols.push_back(symbol);
  } else {
    for (const auto& owed : account.journal().owed_) {
      if (std::find(symbols.begin(), symbols.end(), owed.symbol_) == symbols.end()) {
        symbols.push_back(owed.symbol_);
      }
    }
  }
  if (interrupted()) {
    return interrupted_before_send(std::move(body));
  }
  std::vector<ReconcileRow> rows;
  json::array checked;
  if (!symbols.empty()) {
    auto opened = open_journal(account);
    if (opened.refusal_) {
      return with_body(std::move(*opened.refusal_), std::move(body));
    }
    for (const auto& symbol : symbols) {
      json::object row;
      row["symbol"] = symbol;
      ReconcileRow state;
      const auto held = held_of(account.desk(), symbol);
      if (!held) {
        state = {"holdings_unread", Exit::REFUSED, "refused.holdings_unread"};
        row["detail"] = "the holdings could not be read";
      } else {
        auto config = once_config(account.config(), risk_limits(account.config(), {}, 0));
        config.dry_run_ = true;
        // Reconcile never sends: no phase stands between it and the confirmations holdings support.
        config.limits_.permitted_phases_.reset();
        sending::OnceRequest request;
        request.instrument_ = paper::kr(symbol);
        request.target_quantity_ = *held;
        const auto result = sending::once(account.desk(), *opened.journal_, account.lock(), request, config);
        state = reconcile_row(result);
        row["detail"] = result.detail_;
      }
      row["state"] = state.state_;
      rows.push_back(state);
      checked.push_back(std::move(row));
    }
  }
  body["checked"] = std::move(checked);
#if !defined(_WIN32)
  account.refresh_view();
#endif
  if (!account.journal_intact()) {
    return finish(account, with_body(account.journal_damaged_reply(), std::move(body)), std::nullopt);
  }
  const auto overall = reconcile_exit(rows, !account.journal().owed_.empty());
  if (overall.first == Exit::OK) {
    body["reason"] = "clean";
    body["result"] = "done";
    Reply reply;
    reply.body_ = std::move(body);
    return finish(account, std::move(reply), std::nullopt);
  }
  body["reason"] = overall.second.substr(overall.second.find('.') + 1);
  return finish(
    account,
    coded(
      overall.first, overall.second,
      overall.first == Exit::UNKNOWN ?
        "an order's outcome is unknown; see checked" :
        "an order is owed or resting; see checked and owed",
      std::move(body)
    ),
    std::nullopt
  );
}

/**
 * @brief `cmdatl order release SYMBOL --reason TEXT`: the operator's settlement of what the journal owes on a symbol,
 *   on the approval file `<stem>.release`; sends nothing.
 * @param invocation The arguments.
 * @param account The account.
 * @param output The output.
 * @return The reply.
 */
inline Reply release_reply(const Invocation& invocation, Account& account, Output& output) {
  (void)output;
  using namespace orders_detail;
  auto started = start(account);
  if (started.refusal_) {
    return std::move(*started.refusal_);
  }
  auto body = std::move(started.body_);
  const auto symbol = invocation.get("SYMBOL");
  body["symbol"] = symbol;
  if (interrupted()) {
    return interrupted_before_send(std::move(body));
  }
  sending::OnceResult result;
#if !defined(_WIN32)
  {
    auto opened = open_journal(account);
    if (opened.refusal_) {
      return with_body(std::move(*opened.refusal_), std::move(body));
    }
    result = sending::release(
      account.desk(), *opened.journal_, account.lock(), paper::kr(symbol), invocation.get("reason"),
      account.config().armed_path(), account.config().release_path(), [](std::string_view text) {
        return paper::valid_symbol(text);
      }
    );
  }
#endif
  body["released"] = words_json(result.released_);
  if (result.exit_ == sending::OnceExit::DONE) {
    body["approval_kept"] = result.approval_kept_;
  }
  return finish(account, from_once(result, std::move(body)), symbol);
}

} // namespace cmdatl
