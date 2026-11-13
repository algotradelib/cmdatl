#pragma once

// Which working orders an account's journal owns, by positive evidence only: the run's start uses it before a
// withdraw, and once() before a withdrawal on a broker's account.

#include "../core/time.hpp"
#include "./account.hpp"

#include <algotradelib/runner>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cmdatl {

/**
 * @brief Which of the account's working orders a run may take as an earlier run's order, before anything is sent: the
 *   run's withdraw cancels every working order, so each must be one the account's journal owns.
 *
 * A row is owned only with positive evidence: the journal still owes a named placement on one of the run's instruments
 * that was sent on `today` (its earliest line is its `PENDING` line, written before the send, on `today`) and whose
 * broker number came from an acknowledgement (`ACCEPTED`) also written on `today`; every line of that placement names
 * that number or none, no other placement's line names it, and the row carries that number with the same instrument,
 * side and quantity, the same limit price when the row shows one, and an order date of `today` when the row shows one.
 * KRX day orders expire at the close, so a placement sent on an earlier day owns no working row, even when its
 * acknowledgement was written today (a process suspended between the broker's answer and its line); one whose send day
 * the journal does not show owns none; a placement whose acknowledgement was lost (no number in the journal, or one
 * only a recovery sweep wrote) owns none either.
 * Each placement owns at most one row. It reads the journal and appends nothing.
 * @tparam Tag The broker's tag.
 * @param replay The account's journal, as read at the start.
 * @param owed What the journal still owes, as the library restores it.
 * @param ids The run's instruments.
 * @param today The run's KST date, as days from 1970-01-01.
 * @param rows The working orders.
 * @return For each row, the journal id of the placement that owns it, or empty when none does.
 */
template <typename Tag>
std::vector<std::string> inherited_orders(
  const atl::runner::JournalReplay& replay, const std::vector<Owed>& owed, const std::vector<atl::InstrumentId>& ids,
  std::int64_t today, const std::vector<atl::WorkingOrder<Tag>>& rows
) {
  const auto same = [](const atl::Decimal64& a, const atl::Decimal64& b) {
    return !atl::decimal_less(a, b) && !atl::decimal_less(b, a);
  };
  const auto on_today = [today](std::chrono::system_clock::time_point at) {
    return in_interval(at) && kst_day(at) == today;
  };
  // The broker numbers today's acknowledgements gave the placements still owed, each with its line.
  struct Claim {
    const atl::runner::JournalEntry* acknowledged_{nullptr};
    bool contested_{false};
    bool used_{false};
  };
  std::map<std::string, Claim> claims;
  const auto named = std::string(atl::runner::obligation_state_name(atl::runner::ObligationState::NAMED));
  for (const auto& row : owed) {
    if (row.state_ != named || row.order_id_.empty()) {
      continue;
    }
    const atl::runner::JournalEntry* acknowledged = nullptr;
    // When the placement was sent: its earliest line, which must be a `PENDING` line (written before the send).
    std::optional<std::chrono::system_clock::time_point> earliest;
    std::optional<std::chrono::system_clock::time_point> sent;
    bool consistent = true;
    for (const auto& entry : replay.entries_) {
      if (entry.action_ != atl::runner::JournalAction::PLACE || entry.id_ != row.journal_id_) {
        continue;
      }
      if (!earliest || entry.recorded_at_ < *earliest) {
        earliest = entry.recorded_at_;
      }
      if (entry.outcome_ == atl::runner::JournalOutcome::PENDING && (!sent || entry.recorded_at_ < *sent)) {
        sent = entry.recorded_at_;
      }
      if (entry.requires_operator_ || (!entry.order_id_.empty() && entry.order_id_ != row.order_id_)) {
        consistent = false;
      }
      if (
        acknowledged == nullptr && entry.outcome_ == atl::runner::JournalOutcome::ACCEPTED &&
        entry.order_id_ == row.order_id_ && on_today(entry.recorded_at_)
      ) {
        acknowledged = &entry;
      }
    }
    if (!consistent || acknowledged == nullptr || !sent || *sent != *earliest || !on_today(*sent)) {
      continue;
    }
    if (std::find(ids.begin(), ids.end(), acknowledged->instrument_) == ids.end()) {
      continue;
    }
    // A second placement with the same number is found below, among the lines that name it.
    Claim claim;
    claim.acknowledged_ = acknowledged;
    claims.emplace(row.order_id_, claim);
  }
  // A number another placement's line names is no evidence of either.
  for (const auto& entry : replay.entries_) {
    if (entry.action_ != atl::runner::JournalAction::PLACE || entry.order_id_.empty()) {
      continue;
    }
    const auto found = claims.find(entry.order_id_);
    if (found != claims.end() && entry.id_ != found->second.acknowledged_->id_) {
      found->second.contested_ = true;
    }
  }
  std::vector<std::string> owners(rows.size());
  for (std::size_t at = 0; at < rows.size(); ++at) {
    const auto& order = rows[at].order_;
    const auto found = order.order_id_.empty() ? claims.end() : claims.find(order.order_id_);
    if (found == claims.end() || found->second.contested_ || found->second.used_) {
      continue;
    }
    const auto& line = *found->second.acknowledged_;
    const bool price_agrees = !order.price_ || (line.price_ && same(*order.price_, *line.price_));
    const bool date_agrees = !order.ordered_at_ || (!order.ordered_date_assumed_ && on_today(*order.ordered_at_));
    if (
      !(order.instrument_ == line.instrument_) || order.side_ != line.side_ || !same(order.quantity_, line.quantity_) ||
      !price_agrees || !date_agrees
    ) {
      continue;
    }
    found->second.used_ = true;
    owners[at] = line.id_;
  }
  return owners;
}

} // namespace cmdatl
