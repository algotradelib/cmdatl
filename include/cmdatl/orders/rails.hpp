#pragma once

// The rails shared by the order commands and the paper-run driver: the risk limits from the operator's file, the
// account's journal opened as its one writer, and the catalog's hints.

#include "../account/account.hpp"
#include "../core/errors.hpp"

#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cmdatl {

/**
 * @brief The `next` the catalog gives a code.
 * @param code The code.
 * @return The text, or empty.
 */
inline std::string catalog_next(const std::string& code) {
  for (const auto& entry : error_catalog()) {
    if (entry.code_ == code) {
      return entry.next_;
    }
  }
  return {};
}

namespace orders_detail {

namespace atl = algotradelib;

/**
 * @brief The phase named in the operator's file, as the library's.
 * @param name The name.
 * @return The phase, or nothing.
 */
inline std::optional<atl::strategy::MarketPhase> phase_named(const std::string& name) {
  for (const auto phase : atl::strategy::all_market_phases) {
    if (atl::strategy::market_phase_name(phase) == name) {
      return phase;
    }
  }
  return std::nullopt;
}

}  // namespace orders_detail

/**
 * @brief The risk limits a call enforces, from the operator's file: the allowed universe, the smallest cap of the
 *   symbols (0 for none), the order value, the placements left today over 24 hours, buying power and sellable enforced, and the
 *   permitted phases.
 * @param config The operator's file.
 * @param symbols The symbols whose caps apply: one for an order command, a run's symbols, none for a call that never
 *   places.
 * @param orders_left Placements left today.
 * @return The limits.
 */
inline algotradelib::strategy::RiskLimits risk_limits(
  const OperatorConfig& config,
  const std::vector<std::string>& symbols,
  std::int64_t orders_left
) {
  namespace atl = algotradelib;
  atl::strategy::RiskLimits limits;
  std::vector<atl::InstrumentId> universe;
  for (const auto& allowed : config.limits_.symbols_) {
    universe.push_back(paper::kr(allowed));
  }
  limits.permitted_universe_ = std::move(universe);
  // The library has one cap per runner, so several symbols get the smallest of theirs; a symbol without one caps at 0.
  std::int64_t cap = 0;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    const auto found = config.limits_.position_.find(symbols[i]);
    const std::int64_t own = found == config.limits_.position_.end() ? 0 : found->second;
    cap = i == 0 ? own : std::min(cap, own);
  }
  limits.max_position_quantity_ = atl::Decimal64(cap, 0);
  limits.max_order_notional_ = atl::Decimal64::parse(config.limits_.order_value_);
  limits.max_orders_per_window_ = static_cast<std::size_t>(std::max<std::int64_t>(orders_left, 0));
  limits.rate_window_ = std::chrono::hours(24);
  limits.enforce_buying_power_ = true;
  limits.enforce_sellable_quantity_ = true;
  std::vector<atl::strategy::MarketPhase> phases;
  for (const auto& name : config.limits_.phases_) {
    if (const auto phase = orders_detail::phase_named(name)) {
      phases.push_back(*phase);
    }
  }
  limits.permitted_phases_ = std::move(phases);
  return limits;
}

namespace orders_detail {

/** @brief The journal opened for one command, or the refusal. */
struct OpenedJournal {
  /** @brief The journal. */
  std::unique_ptr<algotradelib::runner::FileJournal> journal_;
  /** @brief The refusal. */
  std::optional<Reply> refusal_;
};

/**
 * @brief Opens the journal as the command's one writer: the lock still the account's, the journal still the file the
 *   account opened (else nothing is constructed, since constructing it on a vanished path would start a fresh one),
 *   then the library's journal.
 * @param account The account.
 * @return The journal, or the refusal (exit 4).
 */
inline OpenedJournal open_journal(Account& account) {
  OpenedJournal opened;
  if (!account.lock().still_at_path()) {
    opened.refusal_ = lock_lost_reply();
    return opened;
  }
#if !defined(_WIN32)
  if (!account.journal_unchanged()) {
    opened.refusal_ = journal_missing_reply("missing, empty or replaced");
    return opened;
  }
#endif
  opened.journal_ = std::make_unique<algotradelib::runner::FileJournal>(account.journal_path(), account.config().scope());
  if (!opened.journal_->open()) {
    opened.refusal_ = error_reply(
      Exit::UNKNOWN,
      "unknown.journal_damaged",
      "the account's journal refused to open: " + opened.journal_->rejection(),
      catalog_next("unknown.journal_damaged")
    );
    opened.journal_.reset();
  }
  return opened;
}

}  // namespace orders_detail

}  // namespace cmdatl
