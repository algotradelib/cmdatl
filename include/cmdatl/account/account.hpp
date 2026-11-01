#pragma once

// The account model: the state directory, the owner lock, the state files and their classification, the journal's
// read side, and the paper desk, opened in that order by `Account::open`, the one place an account is opened.

#include "../core/reply.hpp"
#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "./desk.hpp"
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
    if (!paper::in_interval(entry.recorded_at_)) {
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
    if (paper::kst_day(entry.second) == today) {
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
 * @brief The refusal of paper state that cannot be rebuilt.
 * @param what What is wrong.
 * @return The reply (exit 4).
 */
inline Reply paper_state_reply(const std::string& what) {
  return error_reply(
    Exit::UNKNOWN, "unknown.paper_state", what + "; the paper account's state cannot be rebuilt",
    "stop and report to the operator, who restores paper.jsonl from where it was, or moves the account's whole "
    "directory aside to abandon this paper account"
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

/**
 * @brief The market phase at a desk's clock: the library's KRX session window on the broker's calendar.
 * @tparam Desk The desk type.
 * @param desk The desk.
 * @return The phase's name, such as `open`.
 */
template <typename Desk>
std::string phase_of(Desk& desk) {
  const auto window = algotradelib::strategy::SessionWindow::krx();
  const auto now = desk.now();
  const auto calendar = desk.session().client().calendar(std::nullopt);
  std::optional<bool> trading;
  if (calendar) {
    trading = calendar.value().is_trading_day(window.local_date(now));
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
   *   the state files and their classification (initializing a fresh account), the journal, the desk, the journal's
   *   view, and the paper log's repair only when that view is this account's and intact, in that order.
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
    const auto dir = account_dir(config);
    if (!dir) {
      opened.refusal_ = error_reply(
        Exit::FAILED, "state.unavailable",
        "no state directory: neither XDG_STATE_HOME nor HOME is set as an absolute path",
        "the operator sets XDG_STATE_HOME or HOME (another state directory is another paper account)"
      );
      return opened;
    }
    if (const int error = make_private_dirs(*dir)) {
      opened.refusal_ = error_reply(
        Exit::FAILED, "state.unavailable",
        "the account's directory " + *dir + " cannot be made: " + std::strerror(error),
        "check the state directory and its permissions"
      );
      return opened;
    }
    auto account = std::make_unique<Account>(AccountKey{}, config, *dir);
    const auto outcome = account->lock_.acquire(LOCK_WAIT, interrupted, [&output](const std::string& holder) {
      output.progress("waiting for the account: " + holder + " holds it");
    });
    switch (outcome) {
      case OwnerLock::Outcome::HELD:
        break;
      case OwnerLock::Outcome::BUSY:
        opened.refusal_ = error_reply(
          Exit::FAILED, "busy.locked", "the account is held by " + account->lock_.error() + "; nothing was done",
          "run the command again when it is free", true
        );
        return opened;
      case OwnerLock::Outcome::INTERRUPTED:
        opened.refusal_ = error_reply(
          Exit::INTERRUPTED, "interrupted", "interrupted by a signal while waiting for the account; nothing was done",
          "run the command again", true
        );
        return opened;
      case OwnerLock::Outcome::FAILED:
        opened.refusal_ = error_reply(
          Exit::FAILED, "state.unavailable", account->lock_.error(), "check the account's directory " + *dir
        );
        return opened;
    }
    if (auto refused = account->open_held()) {
      opened.refusal_ = std::move(refused);
      return opened;
    }
    opened.account_ = std::move(account);
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
   * @brief The paper desk.
   * @return The desk.
   */
  paper::PaperDesk& desk() {
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
    journal_ = journal_view(desk_->session(), replay_, config_.scope(), paper::kst_day(desk_->now()));
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
      "the operator reads the broker's order list and holdings, then restores the journal; nothing was written"
    );
  }

private:
#if !defined(_WIN32)
  /** @brief What the directory holds, by entry. */
  struct Found {
    /** @brief Whether `journal.jsonl` is there and not empty (by `lstat`). */
    bool journal_{false};
    /** @brief Whether `journal.jsonl` is there at all. */
    bool journal_entry_{false};
    /** @brief Whether `paper.jsonl` is there and not empty. */
    bool paper_{false};
    /** @brief Whether `paper.jsonl` is there at all. */
    bool paper_entry_{false};
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
      } else if (name == "paper.jsonl") {
        found.paper_entry_ = true;
        found.paper_ = !known || info.st_size > 0 || !S_ISREG(info.st_mode);
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
  std::optional<Reply> read_bars(paper::MarketData& data) const {
    const auto& path = config_.paper_.bars_;
    const auto bad = [&path](const std::string& what) {
      return error_reply(Exit::FAILED, "paper.data", "the paper bars " + path + " " + what, "fix the bars file");
    };
    ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
      return bad(std::string("cannot be opened: ") + std::strerror(errno));
    }
    std::string text;
    const auto read = read_whole(fd.get(), paper::MAX_BARS_BYTES, text);
    if (read.status_ != FileRead::Status::OK) {
      return bad("were not read: " + read.text(paper::MAX_BARS_BYTES));
    }
    if (const auto problem = paper::parse_bars(text, data)) {
      return bad("line " + std::to_string(problem->line_) + ": " + problem->message_);
    }
    for (const auto& symbol : config_.limits_.symbols_) {
      if (data.bars_.count(symbol) == 0) {
        return bad("have no rows for " + symbol + ", which limits.symbols lists");
      }
    }
    std::int64_t first = paper::LAST_DAY;
    std::int64_t last = paper::FIRST_DAY;
    for (const auto& entry : data.bars_) {
      first = std::min(first, entry.second.front().day_);
      last = std::max(last, entry.second.back().day_);
    }
    const auto start = config_.paper_.start_at_;
    if (start < paper::open_of(first) + paper::SESSION_LENGTH) {
      return bad(
        "begin " + paper::date_text(first) +
        "; paper.start must be at or after that date's 15:30 KST, so a symbol has a "
        "last price and a previous close"
      );
    }
    if (start >= paper::open_of(last) + paper::SESSION_LENGTH + std::chrono::hours(24)) {
      return bad("end " + paper::date_text(last) + "; paper.start must be before that date's 15:30 KST plus a day");
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
   * @brief Initializes the paper log of a fresh account, or finishes an interrupted first initialization: a torn
   *   genesis cut to empty and written again, the file and the directory flushed, then the journal created.
   * @param data The bars, for the fingerprint.
   * @param exists Whether `paper.jsonl` exists.
   * @return The refusal, or nothing.
   */
  std::optional<Reply> initialize(const paper::MarketData& data, bool exists) {
    const auto path = dir() + "/paper.jsonl";
    const auto genesis =
      paper::genesis_line(paper::fingerprint_of(data, config_.paper_), paper::nanos(config_.paper_.start_at_));
    const auto unwritable = [&path](const std::string& what) {
      return error_reply(
        Exit::FAILED, "file.unwritable", "cannot write " + path + ": " + what, "check the account's directory"
      );
    };
    paper::OpsLog log(path, lock_.identity(), !exists);
    if (log.open_error() != 0) {
      return exists ?
        paper_state_reply(std::string("the paper log cannot be opened: ") + std::strerror(log.open_error())) :
        unwritable(std::strerror(log.open_error()));
    }
    std::string why;
    if (!log.check(why)) {
      return paper_state_reply("the paper log is refused: " + why);
    }
    std::string text;
    const auto read = log.read(text);
    if (read.status_ != FileRead::Status::OK) {
      return paper_state_reply("the paper log was not read: " + read.text(paper::MAX_LOG_BYTES));
    }
    if (text != genesis + "\n") {
      if (text.find('\n') != std::string::npos) {
        // More than an interrupted genesis: the account was used, and its journal is gone.
        return journal_missing_reply("missing");
      }
      try {
        if (!text.empty()) {
          log.truncate_to(0);
        }
        int error = 0;
        switch (log.append(genesis, "genesis", error)) {
          case paper::Appended::OK:
            break;
          case paper::Appended::LOCK_LOST:
            return lock_lost_reply();
          case paper::Appended::NOT_WRITTEN:
          case paper::Appended::NOT_SYNCED:
            return unwritable(std::strerror(error));
        }
      } catch (const paper::LockLost&) {
        return lock_lost_reply();
      } catch (const paper::PaperStateError& error) {
        return paper_state_reply(error.what());
      }
      if (const int error = sync_directory(dir())) {
        return unwritable(std::string("the directory could not be flushed: ") + std::strerror(error));
      }
    }
    if (!lock_.still_at_path()) {
      return lock_lost_reply();
    }
    algotradelib::runner::FileJournal journal(dir() + "/journal.jsonl", config_.scope());
    if (!journal.open()) {
      return error_reply(
        Exit::FAILED, "journal.unopened", "the account's journal could not be created: " + journal.rejection(),
        "check the account's directory"
      );
    }
    return std::nullopt;
  }

  /**
   * @brief Everything `open` does once the lock is held.
   * @return The refusal, or nothing when the account is open.
   */
  std::optional<Reply> open_held() {
    paper::MarketData data;
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
    if (!found.journal_) {
      // Only a fresh account, or a first initialization interrupted before the journal existed, may begin here.
      const bool fresh = !found.journal_entry_ && found.others_.empty();
      if (!fresh) {
        return journal_missing_reply(found.journal_entry_ ? "empty" : "missing");
      }
      if (auto refused = initialize(data, found.paper_entry_)) {
        return refused;
      }
    } else if (!found.paper_) {
      return paper_state_reply("the paper log is missing or empty while the journal exists");
    }
    std::string why;
    if (!state_file_ok("journal.jsonl", why)) {
      return error_reply(
        Exit::UNKNOWN, "unknown.journal_damaged", "the account's journal is refused: " + why,
        "the operator restores the journal as a regular file; nothing was written"
      );
    }
    struct stat identity{};
    if (::lstat(journal_path().c_str(), &identity) == 0) {
      journal_identity_ = std::make_pair(identity.st_dev, identity.st_ino);
    }
    replay_ = algotradelib::runner::read_journal_file(dir() + "/journal.jsonl");
    try {
      desk_ = std::make_unique<paper::PaperDesk>(paper::DeskKey{}, config_, lock_, std::move(data));
    } catch (const paper::LockLost&) {
      return lock_lost_reply();
    } catch (const paper::PaperStateError& error) {
      return paper_state_reply(error.what());
    } catch (const paper::OutOfRange& error) {
      return paper_state_reply(error.what());
    }
    journal_ = journal_view(desk_->session(), replay_, config_.scope(), paper::kst_day(desk_->now()));
    if (!journal_.intact_) {
      // A journal not this account's or damaged: the reads are built from the log as it is, and nothing is written.
      return std::nullopt;
    }
    try {
      desk_->repair();
    } catch (const paper::LockLost&) {
      return lock_lost_reply();
    } catch (const paper::PaperStateError& error) {
      return paper_state_reply(error.what());
    }
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
    readiness.phase_ = phase_of(*desk_);
    const auto& phases = config_.limits_.phases_;
    readiness.phase_permitted_ = std::find(phases.begin(), phases.end(), readiness.phase_) != phases.end();
    return readiness;
  }

  OperatorConfig config_;
  OwnerLock lock_;
  algotradelib::runner::JournalReplay replay_;
  JournalView journal_;
  std::unique_ptr<paper::PaperDesk> desk_;
  std::optional<Readiness> readiness_;
#if !defined(_WIN32)
  std::optional<std::pair<dev_t, ino_t>> journal_identity_;
#endif
};

} // namespace cmdatl
