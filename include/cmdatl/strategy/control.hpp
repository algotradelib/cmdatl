#pragma once

// `run log` and `run stop`: watching a run and stopping it, while it trades, without its lock or the account's.
//
// `run log` reads the run's `events.jsonl` through one byte cursor: complete lines only, each once, in file order; a
// follow finishes on the run's `end` line, or on a final state with evidence that the driver finished writing.
// `run stop` writes this run's line into the operator's stop file (`<stem>.stop`, one line per run), keeping every
// other run's line, under a writer lock beside it, then waits for the driver's acknowledgement in the run's log; it
// answers success only on evidence that trading ended.

#include "../account/operator.hpp"
#include "../core/errors.hpp"
#include "../core/reply.hpp"
#include "../core/runs.hpp"
#include "../core/table.hpp"
#include "../system/files.hpp"
#include "../system/process.hpp"
#include "../system/stop_file.hpp"
#include "./build.hpp"
#include "./runs.hpp"

#include <algotradelib/runner>

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace runs {

/** @brief The most bytes one read of the event log takes. */
inline constexpr std::size_t LOG_READ = 1u << 20;
/** @brief The longest unterminated tail kept while its newline is awaited; a longer line is skipped. */
inline constexpr std::size_t LOG_LINE = 64u << 10;
/** @brief The most events `run log` without `--follow` answers. */
inline constexpr std::size_t LOG_EVENTS = 4096;
/** @brief The most bytes of events `run log` without `--follow` answers. */
inline constexpr std::size_t LOG_BYTES = 4u << 20;
/** @brief How long a stop writer waits for the stop file's lock. */
inline constexpr std::chrono::seconds STOP_LOCK_WAIT{10};
/** @brief How often it looks again. */
inline constexpr std::chrono::milliseconds STOP_LOCK_STEP{20};

#if defined(CMDATL_RUN_TEST_HOOKS)
/**
 * @brief Test-only: how long `run stop` waits for the acknowledgement. Only the tests' executables define
 *   `CMDATL_RUN_TEST_HOOKS`; the shipped binary always waits 30 s.
 * @return The wait.
 */
inline std::chrono::milliseconds& stop_wait_hook() {
  static std::chrono::milliseconds wait{30000};
  return wait;
}
#endif

/**
 * @brief How long `run stop` waits for the acknowledgement: 30 s of real time.
 * @return The wait.
 */
inline std::chrono::milliseconds stop_wait() {
#if defined(CMDATL_RUN_TEST_HOOKS)
  return stop_wait_hook();
#else
  return std::chrono::milliseconds(30000);
#endif
}

/** @brief What a cursor read found. */
enum class CursorRead {
  /** @brief Bytes were read, or none were there. */
  OK,
  /** @brief The log is not there yet. */
  MISSING,
  /** @brief The log exists and is not a regular file that can be read. */
  UNREADABLE,
  /** @brief The log was replaced (another file at the path) or shrank below the cursor. */
  REPLACED,
};

#if !defined(_WIN32)
/**
 * @brief One byte cursor over a run's event log: each complete line once, in file order, unterminated tails held until
 *   their newline (at most `LOG_LINE` bytes), a longer line skipped with one note.
 */
class EventCursor {
public:
  /**
   * @brief Names the log; nothing is opened yet.
   * @param path The log.
   */
  explicit EventCursor(std::filesystem::path path) : path_(std::move(path)) {}

  /**
   * @brief Reads from the cursor to the end of the file, at most `LOG_READ` bytes a call, each complete line given to
   *   `on_line`, then checks the file is still the one opened.
   * @param on_line Called with each complete line, without its newline.
   * @param limit The file size to read up to, or nothing for the whole file.
   * @return What it found.
   */
  CursorRead read(
    const std::function<void(const std::string&)>& on_line, std::optional<std::uint64_t> limit = std::nullopt
  ) {
    if (fd_.get() < 0) {
      fd_.reset(open_lifted(path_.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
      if (fd_.get() < 0) {
        return errno == ENOENT ? CursorRead::MISSING : CursorRead::UNREADABLE;
      }
      struct stat info{};
      if (::fstat(fd_.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        fd_.reset();
        return CursorRead::UNREADABLE;
      }
      device_ = info.st_dev;
      inode_ = info.st_ino;
    }
    if (const auto replaced = check()) {
      return *replaced;
    }
    struct stat info{};
    if (::fstat(fd_.get(), &info) != 0) {
      return CursorRead::UNREADABLE;
    }
    auto end = static_cast<std::uint64_t>(info.st_size);
    if (limit && *limit < end) {
      end = *limit;
    }
    std::string chunk;
    while (cursor_ < end) {
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(end - cursor_, LOG_READ));
      chunk.resize(want);
      const auto got = ::pread(fd_.get(), chunk.data(), want, static_cast<off_t>(cursor_));
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        return CursorRead::UNREADABLE;
      }
      if (got == 0) {
        break;
      }
      cursor_ += static_cast<std::uint64_t>(got);
      take(chunk.data(), static_cast<std::size_t>(got), on_line);
    }
    return CursorRead::OK;
  }

  /**
   * @brief Whether the file was opened.
   * @return Whether it was.
   */
  bool opened() const noexcept {
    return fd_.get() >= 0;
  }

  /**
   * @brief Whether an unterminated tail is held.
   * @return Whether one is.
   */
  bool tail() const noexcept {
    return !pending_.empty() || skipping_;
  }

  /**
   * @brief How many over-long lines were skipped.
   * @return The count.
   */
  std::size_t skipped() const noexcept {
    return skipped_;
  }

private:
  std::optional<CursorRead> check() {
    struct stat at{};
    if (::lstat(path_.c_str(), &at) != 0 || at.st_dev != device_ || at.st_ino != inode_) {
      return CursorRead::REPLACED;
    }
    struct stat open{};
    if (::fstat(fd_.get(), &open) != 0 || static_cast<std::uint64_t>(open.st_size) < cursor_) {
      return CursorRead::REPLACED;
    }
    return std::nullopt;
  }

  void take(const char* data, std::size_t size, const std::function<void(const std::string&)>& on_line) {
    std::size_t start = 0;
    while (start < size) {
      const auto* newline = static_cast<const char*>(std::memchr(data + start, '\n', size - start));
      if (newline == nullptr) {
        if (!skipping_) {
          pending_.append(data + start, size - start);
          if (pending_.size() > LOG_LINE) {
            pending_.clear();
            skipping_ = true;
          }
        }
        return;
      }
      const auto end = static_cast<std::size_t>(newline - data);
      if (skipping_) {
        skipping_ = false;
        ++skipped_;
      } else {
        pending_.append(data + start, end - start);
        if (pending_.size() > LOG_LINE) {
          ++skipped_;
        } else {
          on_line(pending_);
        }
      }
      pending_.clear();
      start = end + 1;
    }
  }

  std::filesystem::path path_;
  ScopedFd fd_;
  dev_t device_{};
  ino_t inode_{};
  std::uint64_t cursor_{0};
  std::string pending_;
  bool skipping_{false};
  std::size_t skipped_{0};
};

/**
 * @brief Whether the run's driver has finished writing: the run lock free (needs a start line, which names it), a valid
 *   `exit.json` (the supervisor reaped the driver), or the driver's pid surely gone.
 * @param dir The run's directory.
 * @param records The records.
 * @return Whether it has.
 */
inline bool writer_finished(const std::filesystem::path& dir, const Records& records) {
  if (records.exit_valid_) {
    return true;
  }
  if (records.start_ && probe_lock(dir, *records.start_) == Probe::FREE) {
    return true;
  }
  return records.pid_ && alive(records.pid_) == Alive::GONE;
}
#endif

/**
 * @brief Whether an event passes `--symbol`: order events of that symbol, and every event that is not an order's.
 * @param event The event.
 * @param symbol The symbol, or empty for every event.
 * @return Whether it passes.
 */
inline bool symbol_selects(const json::object& event, const std::string& symbol) {
  if (symbol.empty()) {
    return true;
  }
  const auto* kind = event.if_contains("event");
  const std::string name = kind != nullptr && kind->is_string() ? std::string(kind->as_string()) : std::string();
  if (name != "placed" && name != "refused" && name != "filled" && name != "canceled" && name != "expired") {
    return true;
  }
  const auto* of = event.if_contains("symbol");
  return of != nullptr && of->is_string() && of->as_string() == symbol;
}

/**
 * @brief An event line as an object: one JSON object with a string `event`, or nothing.
 * @param line The line.
 * @return The event.
 */
inline std::optional<json::object> event_of(const std::string& line) {
  auto object = one_object(line);
  if (!object) {
    return std::nullopt;
  }
  const auto* kind = object->if_contains("event");
  if (kind == nullptr || !kind->is_string()) {
    return std::nullopt;
  }
  return object;
}

/**
 * @brief The run a command names, or the account's newest, and its directory.
 * @param invocation The arguments.
 * @param config The operator's file.
 * @param run Set to the run's id.
 * @param dir Set to its directory.
 * @return A refusal, or nothing.
 */
inline std::optional<Reply> named_run(
  const Invocation& invocation, const OperatorConfig& config, std::string& run, std::filesystem::path& dir
) {
  run = invocation.get("RUN");
  if (!run.empty() && !valid_run_id(run)) {
    return error_reply(
      Exit::USAGE, "usage.bad_value", "a run id is r and 12 lowercase hex digits, not '" + run.substr(0, 40) + "'"
    );
  }
  if (run.empty()) {
    const auto newest = latest_run(config);
    if (!newest) {
      return error_reply(Exit::FAILED, "run.unknown", "the account has no run", next_for("run.unknown"));
    }
    run = *newest;
  }
  const auto where = run_dir(config, run);
  std::error_code ec;
  if (!where || !std::filesystem::is_directory(*where, ec)) {
    return error_reply(Exit::FAILED, "run.unknown", "no run " + run + " for this account", next_for("run.unknown"));
  }
  dir = *where;
  return std::nullopt;
}

/**
 * @brief `run log [RUN] [--follow] [--symbol SYMBOL]`; see the file comment.
 * @param invocation The arguments.
 * @param snapshot The operator's file, read and `OK`.
 * @param output Where a follow's events go.
 * @return The reply.
 */
inline Reply log_reply(const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)snapshot;
  (void)output;
  return error_reply(Exit::FAILED, "unavailable.platform", "runs run on macOS and Linux only in this version");
#else
  std::string run;
  std::filesystem::path dir;
  if (auto refused = named_run(invocation, *snapshot.config_, run, dir)) {
    return std::move(*refused);
  }
  const bool follow = invocation.has("follow");
  const auto symbol = invocation.get("symbol");
  const auto path = dir / "events.jsonl";
  EventCursor cursor(path);
  bool noted_bad = false;
  bool noted_long = false;
  bool saw_start = false;
  bool saw_end = false;
  const auto finish_body = [&](Reply& reply, const State& state) {
    reply.body_["run"] = run;
    reply.body_["path"] = path.string();
    reply.body_["state"] = state.state_;
    reply.body_["run_exit"] = state.run_exit_ ? json::value(*state.run_exit_) : json::value(nullptr);
  };
  const auto replaced = [&](Exit exit) {
    auto reply = error_reply(
      exit, "run.log_replaced", "the run's event log was replaced or shrank while it was read",
      next_for("run.log_replaced")
    );
    reply.body_["run"] = run;
    reply.body_["path"] = path.string();
    return reply;
  };
  if (!follow) {
    struct stat info{};
    const auto [records, state] = evaluate(dir, run);
    if (::lstat(path.c_str(), &info) != 0) {
      // Without the log, a run that traded shows it by a reply carrying its summary.
      const bool started = records.reply_ && records.reply_->contains("outcome");
      if (errno == ENOENT && !started) {
        Reply reply;
        finish_body(reply, state);
        reply.body_["events"] = json::array{};
        reply.body_["truncated"] = false;
        return reply;
      }
      auto reply = error_reply(
        Exit::FAILED, "run.log_unreadable", "the run started and its event log cannot be read",
        next_for("run.log_unreadable")
      );
      reply.body_["run"] = run;
      return reply;
    }
    // Every selected event but the heartbeats, and the last heartbeat at its place in file order; the newest
    // `LOG_EVENTS` within `LOG_BYTES`.
    struct Kept {
      json::object event_;
      std::size_t bytes_;
      bool clock_;
    };
    std::deque<Kept> kept;
    std::size_t bytes = 0;
    bool truncated = false;
    const auto read = cursor.read(
      [&](const std::string& line) {
        auto event = event_of(line);
        if (!event) {
          noted_bad = true;
          return;
        }
        if (!symbol_selects(*event, symbol)) {
          return;
        }
        const bool clock = event->at("event").as_string() == "clock";
        if (clock) {
          for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            if (it->clock_) {
              bytes -= it->bytes_;
              kept.erase(std::next(it).base());
              break;
            }
          }
        }
        bytes += line.size();
        kept.push_back(Kept{std::move(*event), line.size(), clock});
        while (kept.size() > LOG_EVENTS || bytes > LOG_BYTES) {
          bytes -= kept.front().bytes_;
          kept.pop_front();
          truncated = true;
        }
      },
      static_cast<std::uint64_t>(info.st_size)
    );
    if (read == CursorRead::REPLACED) {
      return replaced(Exit::FAILED);
    }
    if (read != CursorRead::OK) {
      auto reply = error_reply(
        Exit::FAILED, "run.log_unreadable", "the run's event log is not a regular file that can be read",
        next_for("run.log_unreadable")
      );
      reply.body_["run"] = run;
      return reply;
    }
    json::array events;
    for (auto& entry : kept) {
      events.push_back(std::move(entry.event_));
    }
    Reply reply;
    finish_body(reply, state);
    reply.body_["events"] = std::move(events);
    reply.body_["truncated"] = truncated;
    if (noted_bad || cursor.skipped() > 0) {
      reply.body_["note"] = "lines that are not events were skipped";
    }
    return reply;
  }
  // The follow: one cursor from the start of the file, each selected event emitted once.
  const auto on_line = [&](const std::string& line) {
    auto event = event_of(line);
    if (!event) {
      if (!noted_bad) {
        noted_bad = true;
        output.emit(json::object{{"event", "note"}, {"message", "a line of the log is not an event; skipped"}});
      }
      return;
    }
    const std::string kind(event->at("event").as_string());
    saw_start = saw_start || kind == "start";
    saw_end = saw_end || kind == "end";
    if (symbol_selects(*event, symbol)) {
      output.emit(*event);
    }
  };
  while (true) {
    if (interrupted()) {
      return interrupted_reply();
    }
    auto read = cursor.read(on_line);
    if (cursor.skipped() > 0 && !noted_long) {
      noted_long = true;
      output.emit(json::object{{"event", "note"}, {"message", "a line of the log longer than 64 KiB was skipped"}});
    }
    if (read == CursorRead::REPLACED) {
      return replaced(Exit::UNKNOWN);
    }
    if (read == CursorRead::UNREADABLE) {
      auto reply = error_reply(
        Exit::FAILED, "run.log_unreadable", "the run's event log is not a regular file that can be read",
        next_for("run.log_unreadable")
      );
      reply.body_["run"] = run;
      return reply;
    }
    if (saw_end) {
      read = cursor.read(on_line);
      if (read == CursorRead::REPLACED) {
        return replaced(Exit::UNKNOWN);
      }
      const auto [records, state] = evaluate(dir, run, false);
      Reply reply;
      finish_body(reply, state);
      reply.body_["incomplete"] = false;
      return reply;
    }
    // Each poll reads the run's state without counting its events. A final state finishes the follow only with evidence
    // the driver finished writing; `unknown` then means a driver killed or crashed, and finishes incomplete.
    const auto [records, state] = evaluate(dir, run, false);
    if (terminal(state) && writer_finished(dir, records)) {
      // The final drain from the cursor to the end of the file.
      read = cursor.read(on_line);
      if (read == CursorRead::REPLACED) {
        return replaced(Exit::UNKNOWN);
      }
      Reply reply;
      finish_body(reply, state);
      // A log that began (its start line) and has no end may lack its last events; an unknown run's always may.
      const bool incomplete = !saw_end && (saw_start || cursor.tail() || state.state_ == "unknown");
      reply.body_["incomplete"] = incomplete;
      if (incomplete) {
        reply.exit_ = Exit::UNKNOWN;
        reply.body_["result"] = "unknown";
        reply.body_["error"] = json::object{
          {"code", "unknown.run_ended"},
          {"message", "the run finished without its end line: its log may lack its last events"},
          {"next", "cmdatl run status " + run},
        };
      }
      return reply;
    }
    std::this_thread::sleep_for(POLL);
  }
#endif
}

/** @brief The stop file's new text, or why it is refused. */
struct StopRewrite {
  /** @brief The text to write, or nothing when this run's line already holds an equal or stronger intent. */
  std::optional<std::string> text_;
  /** @brief The other runs whose lines stay. */
  std::vector<std::string> kept_runs_;
  /** @brief Whether the result is above `MAX_STOP_BYTES`. */
  bool full_{false};
};

/**
 * @brief The stop file rewritten for one run's request: every other line kept byte for byte and newline-terminated,
 *   except the lines of finished runs (only those with the run id's grammar), this run's own valid lines replaced by one
 *   line with the requested intent, unless they already hold an equal or stronger one.
 * @param text The current file.
 * @param run This run's id.
 * @param intent The requested intent.
 * @param finished Whether another run (by its id, of the run id's grammar) is finished.
 * @return The rewrite.
 */
inline StopRewrite rewrite_stop(
  const std::string& text, const std::string& run, atl::runner::StopIntent intent,
  const std::function<bool(const std::string&)>& finished
) {
  StopRewrite out;
  const auto current = stop_intent_of(text, run);
  std::string kept;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.empty() || stop_line_intent(line, run)) {
      continue;
    }
    // The line's words as the driver splits them: a hand-written tab or carriage return still names the run.
    const auto words = stop_line_words(line);
    const auto other = words.size() == 2 ? words[1] : std::string();
    if (valid_run_id(other) && finished(other)) {
      continue;
    }
    kept += line + "\n";
    if (valid_run_id(other)) {
      out.kept_runs_.push_back(other);
    }
  }
  if (current && atl::runner::stop_intent_strength(*current) >= atl::runner::stop_intent_strength(intent)) {
    return out;
  }
  kept += std::string(atl::runner::stop_intent_name(intent)) + " " + run + "\n";
  out.full_ = kept.size() > MAX_STOP_BYTES;
  out.text_ = std::move(kept);
  return out;
}

#if !defined(_WIN32)
/** @brief The stop file's writer lock: `flock` on `<stem>.stop.lock`, a persistent file, held while this lives. */
class StopLock {
public:
  /** @brief How taking it ended. */
  enum class Outcome {
    /** @brief Held. */
    HELD,
    /** @brief Another writer held it past the deadline. */
    BUSY,
    /** @brief Interrupted. */
    INTERRUPTED,
    /** @brief The file cannot be opened, is not a regular file at the path, or was replaced. */
    FAILED,
  };

  /**
   * @brief Takes the lock, trying every 20 ms for at most `wait`.
   * @param path The lock file.
   * @param wait The deadline.
   * @return How it ended.
   */
  Outcome take(const std::filesystem::path& path, std::chrono::milliseconds wait) {
    fd_.reset(open_lifted(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    int error = 0;
    if (fd_.get() < 0 || !single_regular(fd_.get(), error)) {
      fd_.reset();
      return Outcome::FAILED;
    }
    const auto until = std::chrono::steady_clock::now() + wait;
    while (::flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EINTR) {
        fd_.reset();
        return Outcome::FAILED;
      }
      if (interrupted()) {
        fd_.reset();
        return Outcome::INTERRUPTED;
      }
      if (std::chrono::steady_clock::now() >= until) {
        fd_.reset();
        return Outcome::BUSY;
      }
      std::this_thread::sleep_for(STOP_LOCK_STEP);
    }
    if (!same_file(fd_.get(), path.string())) {
      fd_.reset();
      return Outcome::FAILED;
    }
    return Outcome::HELD;
  }

  /** @brief Releases it. */
  void release() noexcept {
    fd_.reset();
  }

private:
  ScopedFd fd_;
};

/**
 * @brief Writes the stop file through a temporary with a unique name beside it, synced, then renamed.
 * @param path The stop file.
 * @param text The bytes.
 * @return 0, or the error number.
 */
inline int publish_stop(const std::filesystem::path& path, const std::string& text) {
  const auto temporary = build::temporary_for(path);
  ScopedFd fd(open_lifted(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (fd.get() < 0) {
    return errno;
  }
  std::size_t written = 0;
  int error = write_whole(fd.get(), text.data(), text.size(), written);
  if (error == 0 && ::fsync(fd.get()) != 0) {
    error = errno;
  }
  fd.reset();
  if (error == 0 && ::rename(temporary.c_str(), path.c_str()) != 0) {
    error = errno;
  }
  if (error != 0) {
    ::unlink(temporary.c_str());
  }
  return error;
}

/**
 * @brief The stop file a run reads: beside the operator file its record names, or the current one when the record
 *   gives none.
 * @param records The run's records.
 * @param config The current operator file.
 * @param recorded Set to the operator file the run recorded, when it did.
 * @return The operator file whose stop file the run reads.
 */
inline std::string control_config(
  const Records& records, const OperatorConfig& config, std::optional<std::string>& recorded
) {
  if (records.run_) {
    if (
      const auto* path = records.run_->if_contains("config");
      path != nullptr && path->is_string() && !path->as_string().empty()
    ) {
      recorded = std::string(path->as_string());
      return *recorded;
    }
  }
  return config.path_;
}
#endif

/**
 * @brief Whether a run's log or reply acknowledges a stop of at least an intent: a `stop` event with reason `operator`
 *   and an intent as strong, or a validated reply with `stopped: operator`.
 * @param event An event of the log.
 * @param intent The requested intent.
 * @return Whether it does.
 */
inline bool acknowledges(const json::object& event, atl::runner::StopIntent intent) {
  const auto* kind = event.if_contains("event");
  const auto* reason = event.if_contains("reason");
  const auto* named = event.if_contains("intent");
  if (
    kind == nullptr || !kind->is_string() || kind->as_string() != "stop" || reason == nullptr || !reason->is_string() ||
    reason->as_string() != "operator" || named == nullptr || !named->is_string()
  ) {
    return false;
  }
  for (
    const auto candidate :
    {atl::runner::StopIntent::QUIESCE, atl::runner::StopIntent::WITHDRAW, atl::runner::StopIntent::FLATTEN}
  ) {
    if (atl::runner::stop_intent_name(candidate) == named->as_string()) {
      return atl::runner::stop_intent_strength(candidate) >= atl::runner::stop_intent_strength(intent);
    }
  }
  return false;
}

#if !defined(_WIN32)
/**
 * @brief Whether another run is finished, from its `reply.json` and `exit.json` alone (not its event log): a validated
 *   reply, or a valid `exit.json`.
 * @param dir The run's directory.
 * @param run The run's id.
 * @return Whether it is.
 */
inline bool run_finished(const std::filesystem::path& dir, const std::string& run) {
  const auto replied = read_bounded(dir / "reply.json", SMALL_RECORD);
  if (replied.ok_ && !replied.text_.empty() && replied.text_.back() == '\n') {
    if (const auto object = record_of(replied.text_, true); object && valid_reply(*object, run)) {
      return true;
    }
  }
  const auto ended = read_bounded(dir / "exit.json", SMALL_RECORD);
  if (ended.ok_) {
    if (const auto object = record_of(ended.text_, false)) {
      return exit_record_of(*object).has_value();
    }
  }
  return false;
}
#endif

/**
 * @brief Whether a run's driver is gone without a reply: `lost` (its lock free, no reply), or a valid `exit.json`
 *   without a reply (the supervisor reaped it, or nothing was spawned).
 * @param records The records.
 * @param state The state.
 * @return Whether it is.
 */
inline bool driver_gone(const Records& records, const State& state) {
  return state.state_ == "lost" || (records.exit_valid_ && !records.reply_ && !records.reply_damaged_);
}

#if !defined(_WIN32)
/**
 * @brief The answer for a run whose driver is gone without a reply: exit 0 with its state, `run_exit` 1 when nothing
 *   was spawned, else 4 (its market ended with it; a demo-account run will need another rule).
 * @param dir The run's directory.
 * @param run The run's id.
 * @param records The records.
 * @param state The state.
 * @return The reply.
 */
inline Reply gone_reply(
  const std::filesystem::path& dir, const std::string& run, const Records& records, const State& state
) {
  auto reply = status_body(dir, run, records, state);
  reply.body_["run_exit"] = records.exit_valid_ && !records.spawned_ ? 1 : 4;
  return reply;
}
#endif

/**
 * @brief `run stop [RUN] [--withdraw]`; see the file comment and the outcomes below.
 * @param invocation The arguments.
 * @param snapshot The operator's file, read and `OK`.
 * @param output Where progress goes.
 * @return The reply.
 */
inline Reply stop_reply(const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)snapshot;
  (void)output;
  return error_reply(Exit::FAILED, "unavailable.platform", "runs run on macOS and Linux only in this version");
#else
  const auto& config = *snapshot.config_;
  std::string run;
  std::filesystem::path dir;
  if (auto refused = named_run(invocation, config, run, dir)) {
    return std::move(*refused);
  }
  const auto intent = invocation.has("withdraw") ? atl::runner::StopIntent::WITHDRAW : atl::runner::StopIntent::QUIESCE;
  const auto [before, state] = evaluate(dir, run);
  // Evidence that trading ended, before anything is written.
  if (before.reply_ && (state.state_ == "ended" || state.state_ == "refused" || state.state_ == "failed")) {
    auto reply = status_body(dir, run, before, state);
    reply.body_["summary"] = *before.reply_;
    reply.body_["acknowledged"] = nullptr;
    reply.body_["note"] = "the run had ended; nothing was written";
    return reply;
  }
  if (driver_gone(before, state)) {
    auto reply = gone_reply(dir, run, before, state);
    reply.body_["acknowledged"] = nullptr;
    reply.body_["note"] = "the run's driver is gone, and its market with it; nothing was written";
    return reply;
  }
  // The control path: the stop file beside the operator file the run recorded.
  std::optional<std::string> recorded;
  const auto control = control_config(before, config, recorded);
  const auto normal = [](const std::string& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
  };
  if (recorded && normal(*recorded) != normal(config.path_)) {
    return error_reply(
      Exit::USAGE, "usage.bad_value",
      "this run reads its stop from " + *recorded + "; run cmdatl run stop with that operator file",
      "CMDATL_CONFIG=" + *recorded + " cmdatl run stop " + run
    );
  }
  const std::filesystem::path file(control);
  const auto stop_path = file.parent_path() / (file.stem().string() + ".stop");
  const auto lock_path = file.parent_path() / (file.stem().string() + ".stop.lock");
  output.progress("cmdatl: stopping run " + run + " (" + std::string(atl::runner::stop_intent_name(intent)) + ")");
  StopLock lock;
  switch (lock.take(lock_path, std::chrono::duration_cast<std::chrono::milliseconds>(STOP_LOCK_WAIT))) {
    case StopLock::Outcome::HELD:
      break;
    case StopLock::Outcome::BUSY:
      return error_reply(
        Exit::FAILED, "busy.locked", "another run stop holds " + lock_path.string(), "run cmdatl run stop again", true
      );
    case StopLock::Outcome::INTERRUPTED:
      return interrupted_reply();
    case StopLock::Outcome::FAILED:
      return error_reply(
        Exit::FAILED, "file.unwritable", "the stop file's lock " + lock_path.string() + " cannot be taken"
      );
  }
  // The driver's own reader: an existing file it could not read is refused, never replaced.
  const auto current = read_stop_file_checked(stop_path.string());
  if (!current.problem_.empty()) {
    return error_reply(
      Exit::FAILED, "file.unwritable", current.problem_ + " (" + stop_path.string() + "); it was not replaced"
    );
  }
  const auto text = current.text_.value_or(std::string());
  const auto rewrite = rewrite_stop(text, run, intent, [&config](const std::string& other) {
    const auto where = run_dir(config, other);
    if (!where) {
      return false;
    }
    return run_finished(*where, other);
  });
  if (rewrite.full_) {
    std::string names;
    for (const auto& other : rewrite.kept_runs_) {
      names += (names.empty() ? "" : ", ") + other;
    }
    return error_reply(
      Exit::REFUSED, "refused.stop_file_full",
      "the stop file would be above 4096 bytes; it keeps the lines of " +
        (names.empty() ? std::string("no run") : names),
      next_for("refused.stop_file_full")
    );
  }
  if (rewrite.text_) {
    // Before the rename nothing is published; when this run's line already holds the intent, the request stood from
    // the read, and an interruption below answers exit 4.
    if (interrupted()) {
      return interrupted_reply();
    }
    if (const int error = publish_stop(stop_path, *rewrite.text_)) {
      return error_reply(
        Exit::FAILED, "file.unwritable",
        "the stop file " + stop_path.string() + " could not be written: " + std::strerror(error)
      );
    }
  }
  lock.release();
  // Published: the request stands from here, and nothing below may answer that nothing was sent.
  const auto deadline = std::chrono::steady_clock::now() + stop_wait();
  EventCursor cursor(dir / "events.jsonl");
  bool acknowledged = false;
  while (true) {
    (void)cursor.read([&acknowledged, intent](const std::string& line) {
      if (const auto event = event_of(line)) {
        acknowledged = acknowledged || acknowledges(*event, intent);
      }
    });
    const auto [read, final] = evaluate(dir, run, false);
    if (!read.reply_ && driver_gone(read, final)) {
      // The driver is gone without a reply, and its market with it: answered as before any write.
      auto reply = gone_reply(dir, run, read, final);
      reply.body_["acknowledged"] = acknowledged;
      reply.body_["control"] = stop_path.string();
      return reply;
    }
    if (read.reply_) {
      if (
        const auto* stopped = read.reply_->if_contains("stopped");
        stopped != nullptr && stopped->is_string() && stopped->as_string() == "operator"
      ) {
        acknowledged = true;
      }
      if (final.state_ != "ended" && final.state_ != "refused" && final.state_ != "failed") {
        // A reply the records contradict proves nothing: keep waiting for evidence, until the deadline.
        if (std::chrono::steady_clock::now() >= deadline) {
          auto reply = error_reply(
            Exit::UNKNOWN, "unknown.run_stop", "the run's records contradict each other", next_for("unknown.run_stop")
          );
          reply.body_["run"] = run;
          reply.body_["acknowledged"] = acknowledged;
          reply.body_["control"] = stop_path.string();
          return reply;
        }
        std::this_thread::sleep_for(POLL);
        continue;
      }
      auto reply = status_body(dir, run, read, final);
      if (read.reply_) {
        reply.body_["summary"] = *read.reply_;
      }
      reply.body_["state"] = acknowledged ? "stopped" : final.state_;
      reply.body_["acknowledged"] = acknowledged;
      reply.body_["control"] = stop_path.string();
      return reply;
    }
    if (interrupted()) {
      auto reply = error_reply(
        Exit::UNKNOWN, acknowledged ? "unknown.run_stopping" : "unknown.run_stop",
        "interrupted: the stop request stands", next_for(acknowledged ? "unknown.run_stopping" : "unknown.run_stop")
      );
      reply.body_["run"] = run;
      reply.body_["acknowledged"] = acknowledged;
      reply.body_["control"] = stop_path.string();
      return reply;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      const auto code = acknowledged ? std::string("unknown.run_stopping") : std::string("unknown.run_stop");
      auto reply = error_reply(
        Exit::UNKNOWN, code,
        acknowledged ? "the run acknowledged the stop and has not ended" : "the run did not acknowledge the stop",
        next_for(code)
      );
      reply.body_["run"] = run;
      reply.body_["acknowledged"] = acknowledged;
      reply.body_["control"] = stop_path.string();
      return reply;
    }
    std::this_thread::sleep_for(POLL);
  }
#endif
}

} // namespace runs
} // namespace cmdatl
