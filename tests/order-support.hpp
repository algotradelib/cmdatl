#pragma once

// What the order tests share: running an order command in process, the paper log's place and cancel lines, and the
// invariant the exit table promises about them (`no_send_ok`).

#include "./paper-support.hpp"

#include <cmdatl/cli/cli.hpp>

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace support {

/** @brief A command's reply, read. */
struct Answer {
  /** @brief The exit code. */
  int exit_{0};
  /** @brief The reply. */
  boost::json::object body_;
  /** @brief The error code, or empty. */
  std::string code_;
  /** @brief `reason`, or empty. */
  std::string reason_;
};

/**
 * @brief Runs a command in process in JSON and reads its reply.
 * @param fixture The account.
 * @param args The command line.
 * @return The answer.
 */
inline Answer ask(const PaperFixture& fixture, const std::vector<std::string>& args) {
  const auto ran = fixture.json(args);
  Answer answer;
  answer.exit_ = ran.exit_;
  answer.body_ = reply_of(ran.out_);
  if (const auto* error = answer.body_.if_contains("error")) {
    answer.code_ = std::string(error->as_object().at("code").as_string());
  }
  if (const auto* reason = answer.body_.if_contains("reason")) {
    answer.reason_ = std::string(reason->as_string());
  }
  return answer;
}

/**
 * @brief The paper log's lines of one op.
 * @param text The log.
 * @param op `place`, `cancel`, `advance` or `result`.
 * @return The lines.
 */
inline std::vector<boost::json::object> ops_of(const std::string& text, const std::string& op) {
  std::vector<boost::json::object> out;
  for (const auto& line : lines_of(text)) {
    boost::system::error_code error;
    const auto value = boost::json::parse(line, error);
    if (error || !value.is_object()) {
      continue;
    }
    if (value.as_object().at("op").as_string() == op) {
      out.push_back(value.as_object());
    }
  }
  return out;
}

/**
 * @brief The invariant the exit table promises, from the paper log's bytes before and after a command: exit 0 `plan`,
 *   `met`, `nothing_working`, `clean` or `released`, and exits 1, 2, 3 and 130, add no `place` or `cancel` line; exit 0
 *   `placed` adds exactly one `place`; exit 0 `withdrawal_sent` adds only `cancel` lines; exit 4 may add either.
 * @param answer The reply.
 * @param before The log before.
 * @param after The log after.
 * @return Whether it holds.
 */
inline bool no_send_ok(const Answer& answer, const std::string& before, const std::string& after) {
  const auto places = ops_of(after, "place").size() - ops_of(before, "place").size();
  const auto cancels = ops_of(after, "cancel").size() - ops_of(before, "cancel").size();
  if (after.compare(0, before.size(), before) != 0 && answer.exit_ != 4) {
    // Only a repair of a torn tail may change earlier bytes, and only before anything is sent.
    return places == 0 && cancels == 0;
  }
  if (answer.exit_ == 4) {
    return true;
  }
  if (answer.exit_ == 0 && answer.reason_ == "placed") {
    return places == 1 && cancels == 0;
  }
  if (answer.exit_ == 0 && answer.reason_ == "withdrawal_sent") {
    return places == 0 && cancels >= 1;
  }
  return places == 0 && cancels == 0;
}

/**
 * @brief D5's check over a whole paper log: every `place` line is a limit order whose price is within the collar of
 *   the price of its symbol at its instant, as the market's own path gives it.
 * @param log The log.
 * @param collar The collar.
 * @param price_at The market's price of a symbol at an instant (nanoseconds), or a negative number when none.
 * @return Whether every line passes.
 */
template <typename PriceAt>
bool limits_in_collar(const std::string& log, double collar, const PriceAt& price_at) {
  for (const auto& place : ops_of(log, "place")) {
    if (place.at("type").as_string() != "limit" || !place.contains("price")) {
      return false;
    }
    const double limit = std::stod(std::string(place.at("price").as_string()));
    const double mark = price_at(std::string(place.at("symbol").as_string()), place.at("at").as_int64());
    if (mark <= 0 || std::abs(limit - mark) > collar * mark + 1e-9) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Runs a command and checks `no_send_ok` around it.
 * @param fixture The account.
 * @param args The command line.
 * @return The answer.
 */
inline Answer ask_checked(const PaperFixture& fixture, const std::vector<std::string>& args) {
  const auto before = std::filesystem::exists(fixture.log()) ? slurp(fixture.log()) : std::string();
  auto answer = ask(fixture, args);
  const auto after = std::filesystem::exists(fixture.log()) ? slurp(fixture.log()) : std::string();
  INFO(cmdatl::join(args) << " -> " << answer.exit_ << " " << answer.code_ << " " << answer.reason_);
  CHECK(no_send_ok(answer, before, after));
  return answer;
}

/**
 * @brief Plans a target and returns its plan id, checking the plan answered `plan`.
 * @param fixture The account.
 * @param symbol The symbol.
 * @param target The target.
 * @param limit The limit.
 * @return The plan id.
 */
inline std::string plan_id(const PaperFixture& fixture, const std::string& symbol, const std::string& target, const std::string& limit) {
  const auto planned = ask_checked(fixture, {"order", "plan", symbol, "--target", target, "--limit", limit});
  INFO(boost::json::serialize(planned.body_));
  REQUIRE(planned.exit_ == 0);
  REQUIRE(planned.reason_ == "plan");
  return std::string(planned.body_.at("plan_id").as_string());
}

}  // namespace support
