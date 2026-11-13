#pragma once

// The account model: the state directory, the owner lock, the state files and their classification, the journal's
// read side, and the desk, opened in that order by `Account::open`, the one place an account is opened.

#include "../core/reply.hpp"
#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "./mock_desk.hpp"
#include "./operator.hpp"

#include <algotradelib/live>
#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {

/**
 * @brief A KRX instrument as the account's commands name it.
 * @param symbol The symbol.
 * @return The instrument.
 */
inline algotradelib::InstrumentId kr(const std::string& symbol) {
  return {algotradelib::Country::KR, algotradelib::Venue::COMBINED, symbol};
}

/** @brief One placement the journal has not accounted for, as the library's restore names it. */
struct Owed {
  /** @brief The id of its journal lines. */
  std::string journal_id_;
  /** @brief The symbol. */
  std::string symbol_;
  /** @brief `buy` or `sell`. */
  std::string side_;
  /** @brief The quantity. */
  std::string quantity_;
  /** @brief The broker's id, if a line named it. */
  std::string order_id_;
  /** @brief The library's state: `named`, `unnamed` or `operator`. */
  std::string state_;
};

/** @brief What the account's journal says, read once, as the library restores it. */
struct JournalView {
  /** @brief Whether the journal is this account's: readable, with a header of this scope, and no foreign entry. */
  bool owned_{false};
  /** @brief Whether it is owned and recovery read every line and left the runner able to send. */
  bool intact_{false};
  /** @brief What is wrong, when not intact. */
  std::string detail_;
  /** @brief The scope its header names, or empty. */
  std::string scope_found_;
  /** @brief Every placement still owed (`Runner::obligations`); empty when the journal is not this account's. */
  std::vector<Owed> owed_;
  /** @brief Placements whose first line was written on the account's KST date. */
  std::int64_t placed_today_{0};
};

/**
 * @brief Placements the operator still allows today, never below zero; unknown when the journal is not intact.
 * @param config The operator's file.
 * @param view The journal's view.
 * @return The count, or nothing.
 */
inline std::optional<std::int64_t> orders_left(const OperatorConfig& config, const JournalView& view) {
  if (!view.intact_) {
    return std::nullopt;
  }
  return view.placed_today_ >= config.limits_.daily_orders_ ? 0 : config.limits_.daily_orders_ - view.placed_today_;
}

namespace account_detail {

/** @brief The strategy the restoring runner binds: none, and it is never fed. */
struct NoStrategy {
  /**
   * @brief Never called.
   * @tparam Ctx The context.
   * @param ctx The context.
   * @param event The bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& event) {
    (void)ctx;
    (void)event;
  }
};

/** @brief The journal's contents as read once: what `recover_runner` reads of a journal. */
struct ReadContents {
  /** @brief The contents. */
  const algotradelib::runner::JournalReplay* replay_;

  /**
   * @brief The contents, the same each call.
   * @return A copy of the one read.
   */
  algotradelib::runner::JournalReplay replay() const {
    return *replay_;
  }
};

} // namespace account_detail

/**
 * @brief Checks a journal's ownership against the account: readable, a header of this scope, and every entry carrying
 *   no scope or this one (an entry without a scope is accepted beneath a valid header, for compatibility).
 * @param replay The journal, read.
 * @param scope The account's scope.
 * @param view Where `owned_`, `scope_found_` and `detail_` go.
 */
inline void check_ownership(
  const algotradelib::runner::JournalReplay& replay, const std::string& scope, JournalView& view
) {
  view.scope_found_ = replay.scope_;
  if (!replay.readable()) {
    view.detail_ = replay.detail_.empty() ? "the journal cannot be read" : replay.detail_;
    return;
  }
  if (!replay.has_header_) {
    view.detail_ = "the journal has no header, so the account it belongs to cannot be established";
    return;
  }
  if (replay.scope_ != scope) {
    view.detail_ = "the journal belongs to " + replay.scope_ + ", not to " + scope;
    return;
  }
  for (const auto& entry : replay.entries_) {
    if (!entry.scope_.empty() && entry.scope_ != replay.scope_) {
      view.detail_ = "the journal holds an entry of " + entry.scope_ + " beneath the header of " + replay.scope_;
      return;
    }
  }
  view.owned_ = true;
}

/**
 * @brief The journal as the library restores it: a serialized runner over the account's session, with a journal held
 *   in memory, recovers the contents read once; nothing is sent and nothing is written.
 * @tparam Session The session type.
 * @param session The account's session; the runner is bound to it and never sends.
 * @param replay The journal, read once.
 * @param scope The account's scope.
 * @param today The KST date to count placements on, as days from 1970-01-01.
 * @return The view; not intact when a placement's instant is outside the supported interval, as its date is then
 *   unknown (a salvaged operator-only line at the epoch is not counted at all).
 */
template <typename Session>
JournalView journal_view(
  Session& session, const algotradelib::runner::JournalReplay& replay, const std::string& scope, std::int64_t today
) {
  namespace atl = algotradelib;
  JournalView view;
  check_ownership(replay, scope, view);
  if (!view.owned_) {
    return view;
  }
  using Executor = atl::runner::JournalingExecutor<atl::live::SessionExecutor<Session>, atl::runner::MemoryJournal>;
  using Bound =
    atl::runner::Runner<account_detail::NoStrategy, typename Session::Caps, typename Session::BrokerTag, Executor>;
  atl::runner::MemoryJournal memory;
  Bound bound(
    account_detail::NoStrategy{}, atl::strategy::RiskLimits{}, atl::live::default_series_capacity,
    Executor(atl::live::SessionExecutor<Session>(session, atl::Venue::UNSPECIFIED), memory)
  );
  const account_detail::ReadContents contents{&replay};
  const auto recovery = atl::runner::recover_runner(bound, contents);
  view.intact_ = !(
    recovery.truncated_ || recovery.unattributable_ || recovery.unreadable_lines_ > 0 || recovery.unreadable_file_ ||
    bound.guard().halted()
  );
  if (!view.intact_) {
    view.detail_ = recovery.detail_.empty() ? "the journal is damaged" : recovery.detail_;
  }
  for (const auto& owed : bound.obligations()) {
    Owed row;
    row.journal_id_ = owed.journal_id_;
    row.symbol_ = owed.instrument_.symbol_;
    row.side_ = owed.side_ == atl::OrderSide::BUY ? "buy" : "sell";
    row.quantity_ = owed.quantity_.str();
    row.order_id_ = owed.order_id_;
    row.state_ = std::string(atl::runner::obligation_state_name(owed.state_));
    view.owed_.push_back(std::move(row));
  }
  std::map<std::string, std::chrono::system_clock::time_point> first_line;
  for (const auto& entry : replay.entries_) {
    if (entry.action_ != atl::runner::JournalAction::PLACE || entry.id_.empty()) {
      continue;
    }
    if (entry.requires_operator_ && entry.recorded_at_ == std::chrono::system_clock::time_point{}) {
      // A salvaged operator-only line records the epoch because it has no time: no date to count it on.
      continue;
    }
    if (!in_interval(entry.recorded_at_)) {
      // The library does not bound an operator-only line's instant; one outside the interval gives no KST date.
      if (view.intact_) {
        view.intact_ = false;
        view.detail_ = "the journal's entry " + entry.id_ + " was written at an instant outside 1990 to 2100";
      }
      continue;
    }
    const auto found = first_line.find(entry.id_);
    if (found == first_line.end()) {
      first_line.emplace(entry.id_, entry.recorded_at_);
    } else if (entry.recorded_at_ < found->second) {
      found->second = entry.recorded_at_;
    }
  }
  for (const auto& entry : first_line) {
    if (kst_day(entry.second) == today) {
      ++view.placed_today_;
    }
  }
  return view;
}

#if !defined(_WIN32)
/**
 * @brief Creates a directory and its missing parents, each new one `0700`.
 * @param dir The directory.
 * @return 0, or the error number.
 */
inline int make_private_dirs(const std::filesystem::path& dir) {
  std::filesystem::path at;
  for (const auto& part : dir) {
    at /= part;
    if (::mkdir(at.c_str(), 0700) != 0 && errno != EEXIST) {
      return errno;
    }
  }
  struct stat info{};
  if (::stat(dir.c_str(), &info) != 0) {
    return errno;
  }
  return S_ISDIR(info.st_mode) ? 0 : ENOTDIR;
}
#endif

/**
 * @brief The refusal of mock state that cannot be rebuilt.
 * @param what What is wrong.
 * @return The reply (exit 4).
 */
inline Reply paper_state_reply(const std::string& what) {
  return error_reply(
    Exit::UNKNOWN, "unknown.mock_state", what + "; the mock account's state cannot be rebuilt",
    "stop and report to the operator, who restores the account's state file market.jsonl from where it was, or moves "
    "the account's whole directory aside to abandon this mock account"
  );
}

/**
 * @brief The refusal of a journal that is gone while other state exists.
 * @param what `missing` or `empty`.
 * @return The reply (exit 4).
 */
inline Reply journal_missing_reply(const std::string& what) {
  return error_reply(
    Exit::UNKNOWN, "unknown.journal_missing",
    "the account's journal is " + what + " while other state exists; what was sent and today's count are unknown",
    "stop and report to the operator, who reads the broker's order list and holdings, then restores the journal "
    "(moving the whole directory aside starts the account afresh and forgets any order the broker still holds)"
  );
}

/**
 * @brief The refusal after the lock was replaced.
 * @return The reply (exit 4).
 */
inline Reply lock_lost_reply() {
  return error_reply(
    Exit::UNKNOWN, "unknown.lock_lost",
    "the account's lock file was replaced while this command held it; it wrote nothing more",
    "stop and report to the operator, who finds what replaced the lock file and stops it"
  );
}

// A desk: what the account and order commands are given of an account's broker, and all they use of it.
// The reads (`reads.hpp`), the order commands, once() and the plan book are templates over it, or take it from
// `Account::desk()`; `Account` is the one place a desk is built, and `MockDesk` (`mock_desk.hpp`) the one desk.
// On a broker's account the reads alone are given `BrokerDesk` (`broker_desk.hpp`), built by the command path over
// sessions it owns, with the members below but `env_name` and `sends()`: it sends nothing.
//
// Every desk has:
//   - `broker_name`, `env_name`: static; the operator file's `broker` word, and every reply's `env`.
//   - `Session`, `session()`: the trading session, a session with the calls of the library's `BlockingSession`:
//     holdings, buying power, the working-order listing, reference data, placements and cancellations. Every placement and cancellation through
//     it passes the final checks of the operator's file (the armed file for a placement, the lock for both) before it
//     can reach the broker.
//   - `MarketSession`, `market_session()`: the session quotes, price bands and the calendar (so the phase) are read
//     from, each as its own capabilities allow; the trading session itself where the broker needs no other.
//   - `now()`: the account's clock, which dates the phase, the journal's lines and plans.
//   - `sends()`: how many placements and cancellations this process has sent towards the broker, counted before the
//     outcome is known; once() decides by its growth during a call whether anything may have reached the broker.
//   - `has_symbol(symbol)`: whether the account's market has the symbol.
//   - `order_status(id, working)`: what the broker shows for an order it named (`pending`, `filled`, `canceled`), or
//     nothing, given the working orders read once.
//   - `accepts_orders_at(symbol, at)`: whether the broker takes an order on the symbol at an instant.
//
// A simulated broker also has, with `simulated` true (a desk without them has it false, and a command that reads one
// does not compile against that desk):
//   - `paper_cash()`: its cash after every fill and charge, as whole-won text.
//   - `advance_to(target)`, `next_session_after(at)`, `clock_limit()`, `drain_events()`, `halted()`: its clock, moved
//     by command (`clock advance`, and the second every order command takes) and answering `Moved`, the next session's
//     open, the latest instant the clock may reach, what the market did since the last drain (`MockEvent`), and why
//     the market halted, if it did.
//   - `data_end()`: the last date its data trades.

/** @brief The desk an account trades through: the library's mock, kept in the account's state file. */
using AccountDesk = MockDesk;

/**
 * @brief The market phase at a desk's clock: the library's KRX session window on the calendar of the desk's market
 *   session; unknown whether today trades when it has no calendar or the calendar cannot be read.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @return The phase's name, such as `open`.
 */
template <typename Desk>
std::string phase_of(Desk& desk) {
  const auto window = algotradelib::strategy::SessionWindow::krx();
  const auto now = desk.now();
  std::optional<bool> trading;
  if constexpr (Desk::MarketSession::Caps::has_calendar) {
    const auto calendar = desk.market_session().calendar(algotradelib::Country::KR);
    if (calendar) {
      trading = calendar.value().is_trading_day(window.local_date(now));
    }
  }
  return std::string(algotradelib::strategy::market_phase_name(window.phase(now, trading)));
}

/** @brief The key only `Account::open` can make. */
class AccountKey {
  friend class Account;
  AccountKey() = default;
};

/** @brief The preliminary gates of a placement, computed once per invocation under the lock. */
struct Readiness {
  /** @brief Whether the armed file exists. */
  bool armed_{false};
  /** @brief Placements left today; nothing when the journal is not intact. */
  std::optional<std::int64_t> orders_left_;
  /** @brief The market phase. */
  std::string phase_;
  /** @brief Whether the phase is in `limits.phases`. */
  bool phase_permitted_{false};
};

/**
 * @brief One open account: its config, its held lock, its journal's view and its desk.
 *
 * Built only by `open`.
 */
class Account {
public:
  /** @brief What `open` gives: the account, or the refusal. */
  struct Opened {
    /** @brief The account. */
    std::unique_ptr<Account> account_;
    /** @brief The refusal. */
    std::optional<Reply> refusal_;
  };

  /**
   * @brief Builds an account that is not open yet; only `open` can.
   * @param key The key.
   * @param config The operator's file.
   * @param dir The account's directory.
   */
  Account(AccountKey key, OperatorConfig config, std::string dir) : config_(std::move(config)), lock_(std::move(dir)) {
    (void)key;
  }

  Account(const Account&) = delete;
  Account& operator=(const Account&) = delete;
  Account(Account&&) = delete;
  Account& operator=(Account&&) = delete;

#if !defined(_WIN32)
  /**
   * @brief Opens the account: the directory, the lock (waiting at most 10 s, printing who holds it once), the bars,
   *   the state files and their classification, the desk on its state file, the journal (created for a fresh account
   *   after the state file), and the journal's view, in that order.
   *
   * Each refusal changes nothing it has not said it changes.
   * @param snapshot The operator's file, read.
   * @param output Where the waiting line goes.
   * @param interrupted Whether SIGINT or SIGTERM arrived.
   * @return The account, or the refusal.
   */
  static Opened open(const ConfigSnapshot& snapshot, Output& output, const std::function<bool()>& interrupted) {
    Opened opened;
    const auto& config = *snapshot.config_;
    if (config.broker_ != "mock") {
      // The account and order commands trade the mock desk; a broker's account is traded by `cmdatl run` only.
      opened.refusal_ = error_reply(
        Exit::FAILED, "unsupported.broker",
        "the account and order commands trade the mock broker in this version; a " + config.broker_ +
          " account is traded by cmdatl run",
        "read the account in the broker's own app, or run a strategy with cmdatl run"
      );
      return opened;
    }
    opened.account_ = locked(config, output, interrupted, opened.refusal_);
    if (opened.account_) {
      opened.refusal_ = opened.account_->open_held();
    }
    if (opened.refusal_) {
      opened.account_.reset();
    }
    return opened;
  }

  /**
   * @brief Opens a broker's account on a desk the caller owns: the directory, the lock (as `open`), and the journal's
   *   file, read; its view waits for `view_at`, which needs the desk's instant.
   * @param config The operator's file, with the account the login names.
   * @param output Where the waiting line goes.
   * @param interrupted Whether a stop was asked for.
   * @return The account, or the refusal.
   */
  static Opened open_broker(const OperatorConfig& config, Output& output, const std::function<bool()>& interrupted) {
    Opened opened;
    opened.account_ = locked(config, output, interrupted, opened.refusal_);
    if (!opened.account_) {
      return opened;
    }
    // An absent journal is an empty one, as a run treats it.
    struct stat info{};
    std::string why;
    opened.account_->journal_absent_ = ::lstat(opened.account_->journal_path().c_str(), &info) != 0 && errno == ENOENT;
    if (!opened.account_->journal_absent_ && !opened.account_->state_file_ok("journal.jsonl", why)) {
      opened.refusal_ = journal_refused_reply(why);
      opened.account_.reset();
      return opened;
    }
    opened.account_->replay_ = algotradelib::runner::read_journal_file(opened.account_->journal_path());
    return opened;
  }
#endif

  /**
   * @brief The operator's file.
   * @return The config.
   */
  const OperatorConfig& config() const noexcept {
    return config_;
  }

  /**
   * @brief The held lock.
   * @return The lock.
   */
  const OwnerLock& lock() const noexcept {
    return lock_;
  }

  /**
   * @brief The account's directory.
   * @return The directory.
   */
  const std::string& dir() const noexcept {
    return lock_.dir();
  }

  /**
   * @brief The account's desk.
   * @return The desk.
   */
  AccountDesk& desk() {
    return *desk_;
  }

  /**
   * @brief The journal's view, computed at open, or at the last `refresh_view`.
   * @return The view.
   */
  const JournalView& journal() const noexcept {
    return journal_;
  }

  /**
   * @brief The journal's path.
   * @return The path.
   */
  std::string journal_path() const {
    return dir() + "/journal.jsonl";
  }

#if !defined(_WIN32)
  /**
   * @brief Whether the journal at its path is still the file `open` classified: a regular file with one name, not
   *   empty, on the same device and inode.
   * @return Whether it is.
   */
  bool journal_unchanged() const {
    struct stat info{};
    if (::lstat(journal_path().c_str(), &info) != 0) {
      return false;
    }
    return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size > 0 && journal_identity_ &&
      info.st_dev == journal_identity_->first && info.st_ino == journal_identity_->second;
  }

  /**
   * @brief Reads the journal again and recomputes the view and the readiness, with the ownership checks of `open`.
   */
  void refresh_view() {
    replay_ = algotradelib::runner::read_journal_file(journal_path());
    journal_ = journal_view(desk_->session(), replay_, config_.scope(), kst_day(desk_->now()));
    readiness_.reset();
  }

  /**
   * @brief The journal's view and the phase at a broker's desk's instant, after its `begin()`.
   * @tparam Desk The desk type.
   * @param desk The desk; the view's runner is bound to its trading session, and never sends.
   */
  template <typename Desk>
  void view_at(Desk& desk) {
    if (journal_absent_) {
      journal_.owned_ = true;
      journal_.intact_ = true;
    } else {
      journal_ = journal_view(desk.session(), replay_, config_.scope(), kst_day(desk.now()));
    }
    phase_ = desk.phase();
    readiness_.reset();
  }
#endif

  /**
   * @brief The readiness gates, computed once.
   * @return The gates.
   */
  const Readiness& readiness() {
    if (!readiness_) {
      readiness_ = compute_readiness();
    }
    return *readiness_;
  }

  /**
   * @brief Whether the journal's view lets an affirmative answer stand: it is this account's and intact.
   * @return Whether it does.
   */
  bool journal_intact() const noexcept {
    return journal_.intact_;
  }

  /**
   * @brief Whether a torn last line of the journal or a damaged one stood in the way at open.
   * @return The refusal for a journal that is not intact, with its detail.
   */
  Reply journal_damaged_reply() const {
    return error_reply(
      Exit::UNKNOWN, "unknown.journal_damaged",
      "the account's journal is damaged or not this account's (" + journal_.detail_ + "); what was sent is unknown",
      "stop and report to the operator, who reads the broker's order list and holdings, then restores the journal; "
      "nothing was written"
    );
  }

private:
#if !defined(_WIN32)
  /**
   * @brief The account's directory made and its lock taken (waiting at most 10 s, printing who holds it once; at once
   *   when its owner record names a run).
   * @param config The operator's file.
   * @param output Where the waiting line goes.
   * @param interrupted Whether a stop was asked for.
   * @param refusal Set when it is not taken.
   * @return The account, its lock held, or null.
   */
  static std::unique_ptr<Account> locked(
    const OperatorConfig& config, Output& output, const std::function<bool()>& interrupted,
    std::optional<Reply>& refusal
  ) {
    const auto dir = account_dir(config);
    if (!dir) {
      refusal = error_reply(
        Exit::FAILED, "state.unavailable",
        "no state directory: neither XDG_STATE_HOME nor HOME is set as an absolute path",
        "the operator sets XDG_STATE_HOME or HOME (another state directory is another mock account)"
      );
      return nullptr;
    }
    if (const int error = make_private_dirs(*dir)) {
      refusal = error_reply(
        Exit::FAILED, "state.unavailable",
        "the account's directory " + *dir + " cannot be made: " + std::strerror(error),
        "check the state directory and its permissions"
      );
      return nullptr;
    }
    auto account = std::make_unique<Account>(AccountKey{}, config, *dir);
    const auto outcome = account->lock_.acquire(LOCK_WAIT, interrupted, [&output](const std::string& holder) {
      output.progress("waiting for the account: " + holder + " holds it");
    });
    switch (outcome) {
      case OwnerLock::Outcome::HELD:
        return account;
      case OwnerLock::Outcome::BUSY:
        refusal = error_reply(
          Exit::FAILED, "busy.locked", "the account is held by " + account->lock_.error() + "; nothing was done",
          "run the command again when it is free", true
        );
        return nullptr;
      case OwnerLock::Outcome::INTERRUPTED:
        refusal = error_reply(
          Exit::INTERRUPTED, "interrupted", "interrupted by a signal while waiting for the account; nothing was done",
          "run the command again", true
        );
        return nullptr;
      case OwnerLock::Outcome::FAILED:
        break;
    }
    refusal =
      error_reply(Exit::FAILED, "state.unavailable", account->lock_.error(), "check the account's directory " + *dir);
    return nullptr;
  }

  /**
   * @brief The refusal of a journal that is not a regular file with one name.
   * @param why Why.
   * @return The reply (exit 4).
   */
  static Reply journal_refused_reply(const std::string& why) {
    return error_reply(
      Exit::UNKNOWN, "unknown.journal_damaged", "the account's journal is refused: " + why,
      "stop and report to the operator, who restores the journal as a regular file; nothing was written"
    );
  }

  /** @brief What the directory holds, by entry. */
  struct Found {
    /** @brief Whether `journal.jsonl` is there and not empty (by `lstat`). */
    bool journal_{false};
    /** @brief Whether `journal.jsonl` is there at all. */
    bool journal_entry_{false};
    /** @brief Whether the state file is there and not empty. */
    bool state_{false};
    /** @brief Whether the state file is there at all. */
    bool state_entry_{false};
    /** @brief Whether `paper.jsonl`, an earlier version's account log, is there. */
    bool old_log_{false};
    /** @brief Every other entry but `lock`. */
    std::vector<std::string> others_;
  };

  /**
   * @brief Lists the directory.
   * @param found Where the entries go.
   * @return 0, or the error number.
   */
  int list(Found& found) const {
    DIR* raw = ::opendir(dir().c_str());
    if (raw == nullptr) {
      return errno;
    }
    std::unique_ptr<DIR, int (*)(DIR*)> handle(raw, &::closedir);
    errno = 0;
    while (const auto* entry = ::readdir(handle.get())) {
      const std::string name = entry->d_name;
      if (name == "." || name == ".." || name == "lock") {
        continue;
      }
      struct stat info{};
      const bool known = ::lstat((dir() + "/" + name).c_str(), &info) == 0;
      if (name == "journal.jsonl") {
        found.journal_entry_ = true;
        found.journal_ = !known || info.st_size > 0 || !S_ISREG(info.st_mode);
      } else if (name == MOCK_STATE_FILE) {
        found.state_entry_ = true;
        found.state_ = !known || info.st_size > 0 || !S_ISREG(info.st_mode);
      } else if (name == "paper.jsonl") {
        found.old_log_ = true;
      } else {
        found.others_.push_back(name);
      }
    }
    return errno;
  }

  /**
   * @brief Reads and checks the bars the operator's file names.
   * @param data Where the bars go.
   * @return The refusal, or nothing.
   */
  std::optional<Reply> read_bars(MarketData& data) const {
    const auto& path = config_.paper_.bars_;
    const auto bad = [&path](const std::string& what) {
      return error_reply(Exit::FAILED, "mock.data", "the mock bars " + path + " " + what, "fix the bars file");
    };
    ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
      return bad(std::string("cannot be opened: ") + std::strerror(errno));
    }
    std::string text;
    const auto read = read_whole(fd.get(), MAX_BARS_BYTES, text);
    if (read.status_ != FileRead::Status::OK) {
      return bad("were not read: " + read.text(MAX_BARS_BYTES));
    }
    if (const auto problem = parse_bars(text, data)) {
      return bad("line " + std::to_string(problem->line_) + ": " + problem->message_);
    }
    for (const auto& symbol : config_.limits_.symbols_) {
      if (data.bars_.count(symbol) == 0) {
        return bad("have no rows for " + symbol + ", which limits.symbols lists");
      }
    }
    std::int64_t first = LAST_DAY;
    std::int64_t last = FIRST_DAY;
    for (const auto& entry : data.bars_) {
      first = std::min(first, entry.second.front().day_);
      last = std::max(last, entry.second.back().day_);
    }
    const auto start = config_.paper_.start_at_;
    if (start < open_of(first) + SESSION_LENGTH) {
      return bad(
        "begin " + date_text(first) +
        "; mock.start must be at or after that date's 15:30 KST, so a symbol has a "
        "last price and a previous close"
      );
    }
    if (start >= open_of(last) + SESSION_LENGTH + std::chrono::hours(24)) {
      return bad("end " + date_text(last) + "; mock.start must be before that date's 15:30 KST plus a day");
    }
    return std::nullopt;
  }

  /**
   * @brief Opens a state file for a check, without following a link or blocking.
   * @param name The file's name.
   * @param why Set to why it is refused.
   * @return Whether it is a regular file with one name.
   */
  bool state_file_ok(const std::string& name, std::string& why) const {
    ScopedFd fd(open_lifted((dir() + "/" + name).c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0) {
      why = name + " cannot be opened: " + std::strerror(errno);
      return false;
    }
    int error = 0;
    if (!single_regular(fd.get(), error)) {
      why = error != 0 ?
        name + "'s status cannot be read: " + std::strerror(error) :
        name + " is not a regular file with one name";
      return false;
    }
    return true;
  }

  /**
   * @brief Whether the state file records any change: a whole first line with more after it.
   * @return Whether it does; a file that cannot be read counts as used.
   */
  bool state_used() const {
    std::ifstream in(dir() + "/" + MOCK_STATE_FILE, std::ios::binary);
    std::string header;
    if (!in) {
      return true;
    }
    return std::getline(in, header) && in.peek() != std::ifstream::traits_type::eof();
  }

  /**
   * @brief Whether the state file begins with a whole line, one ending in a newline.
   * @return Whether it does; a file that cannot be read does not.
   */
  bool state_header_whole() const {
    std::ifstream in(dir() + "/" + MOCK_STATE_FILE, std::ios::binary);
    std::string header;
    return in && std::getline(in, header) && !in.eof();
  }

  /**
   * @brief Everything `open` does once the lock is held.
   * @return The refusal, or nothing when the account is open.
   */
  std::optional<Reply> open_held() {
    MarketData data;
    if (auto refused = read_bars(data)) {
      return refused;
    }
    Found found;
    if (const int error = list(found)) {
      return error_reply(
        Exit::FAILED, "state.unavailable",
        "the account's directory " + dir() + " cannot be listed: " + std::strerror(error), "check its permissions"
      );
    }
    if (found.old_log_) {
      return error_reply(
        Exit::FAILED, "state.unavailable",
        "the account's directory " + dir() +
          " holds paper.jsonl, an account of an earlier cmdatl, which this version "
          "does not read",
        "create the account again: the operator moves the account's directory aside, and the next command starts it"
      );
    }
    const bool starting = !found.journal_;
    std::string why;
    if (!starting && !state_file_ok("journal.jsonl", why)) {
      return journal_refused_reply(why);
    }
    if (found.state_entry_ && !state_file_ok(MOCK_STATE_FILE, why)) {
      return paper_state_reply("the account's state file is refused: " + why);
    }
    // Only a fresh account, or a first initialization interrupted before the journal existed, may begin here: the
    // journal and the state file are a pair, and neither is started afresh beside the other.
    if (starting && (found.journal_entry_ || !found.others_.empty() || (found.state_ && state_used()))) {
      return journal_missing_reply(found.journal_entry_ ? "empty" : "missing");
    }
    if (!starting && !found.state_) {
      return paper_state_reply("the account's state file is missing or empty while the journal exists");
    }
    // The library rebuilds the original market over a header cut short, which only a first initialization may need.
    if (!starting && !state_header_whole()) {
      return paper_state_reply("the account's state file has no whole first line while the journal exists");
    }
    desk_ = std::make_unique<AccountDesk>(MockDeskKey{}, config_, lock_, data);
    if (!desk_->opened()) {
      return paper_state_reply(desk_->rejection());
    }
    if (starting) {
      if (!lock_.still_at_path()) {
        return lock_lost_reply();
      }
      algotradelib::runner::FileJournal journal(journal_path(), config_.scope());
      if (!journal.open()) {
        return error_reply(
          Exit::FAILED, "journal.unopened", "the account's journal could not be created: " + journal.rejection(),
          "check the account's directory"
        );
      }
    }
    if (starting && !state_file_ok("journal.jsonl", why)) {
      return journal_refused_reply(why);
    }
    struct stat identity{};
    if (::lstat(journal_path().c_str(), &identity) == 0) {
      journal_identity_ = std::make_pair(identity.st_dev, identity.st_ino);
    }
    replay_ = algotradelib::runner::read_journal_file(journal_path());
    journal_ = journal_view(desk_->session(), replay_, config_.scope(), kst_day(desk_->now()));
    return std::nullopt;
  }
#endif

  /**
   * @brief The readiness gates at the desk's clock.
   * @return The gates.
   */
  Readiness compute_readiness() {
    Readiness readiness;
    std::error_code ec;
    readiness.armed_ = std::filesystem::exists(config_.armed_path(), ec);
    readiness.orders_left_ = orders_left(config_, journal_);
    readiness.phase_ = phase_ ? *phase_ : phase_of(*desk_);
    const auto& phases = config_.limits_.phases_;
    readiness.phase_permitted_ = std::find(phases.begin(), phases.end(), readiness.phase_) != phases.end();
    return readiness;
  }

  OperatorConfig config_;
  OwnerLock lock_;
  algotradelib::runner::JournalReplay replay_;
  JournalView journal_;
  std::unique_ptr<AccountDesk> desk_;
  std::optional<Readiness> readiness_;
  std::optional<std::string> phase_;
  bool journal_absent_{false};
#if !defined(_WIN32)
  std::optional<std::pair<dev_t, ino_t>> journal_identity_;
#endif
};

} // namespace cmdatl
