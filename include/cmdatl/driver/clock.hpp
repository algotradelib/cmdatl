#pragma once

// A run's clock and its step guard.
//
// The clock is the mock market's: waiting moves the market, never past a bound one day after the last bar (no bar is
// left to fill against there); asked to wait at the bound it throws `RunBound`, which ends the loop, so a stop whose
// closing pass cannot finish never spins. After each step the run scans what the market did and the guard runs.
//
// The guard latches, at every step: the armed file missing, the run's lock no longer at its path, SIGINT or SIGTERM,
// an event line not written, the operator's stop file naming this run, and the run's end, where it withdraws.

#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "../system/process.hpp"
#include "./events.hpp"

#include <algotradelib/mock>
#include <algotradelib/runner>

#include <boost/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;

/** @brief The most bytes of the stop file read. */
inline constexpr std::size_t MAX_STOP_BYTES = 4096;

#if !defined(_WIN32)
/**
 * @brief Reads the operator's stop file: a regular file, not followed through a link, at most `MAX_STOP_BYTES`.
 * @param path The file.
 * @return Its text, or nothing when it is missing or not such a file.
 */
inline std::optional<std::string> read_stop_file(const std::string& path) {
  ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    return std::nullopt;
  }
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return std::nullopt;
  }
  std::string text;
  if (read_whole(fd.get(), MAX_STOP_BYTES, text).status_ != FileRead::Status::OK) {
    return std::nullopt;
  }
  return text;
}
#endif

/**
 * @brief A stop line: `WORD RUN_ID`, `WORD` one of `quiesce` and `withdraw`, an optional trailing newline.
 * @param text The stop file's text.
 * @param run This run's id.
 * @return The intent it names for this run, or nothing (another run's line, or not a stop line).
 */
inline std::optional<atl::runner::StopIntent> stop_intent_of(const std::string& text, const std::string& run) {
  auto line = text;
  if (!line.empty() && line.back() == '\n') {
    line.pop_back();
  }
  const auto space = line.find(' ');
  if (space == std::string::npos || line.find_first_of(" \n\r\t", space + 1) != std::string::npos) {
    return std::nullopt;
  }
  if (line.substr(space + 1) != run) {
    return std::nullopt;
  }
  const auto word = line.substr(0, space);
  if (word == "quiesce") {
    return atl::runner::StopIntent::QUIESCE;
  }
  if (word == "withdraw") {
    return atl::runner::StopIntent::WITHDRAW;
  }
  return std::nullopt;
}

#if !defined(_WIN32)
/** @brief The run's step guard; each check latches, and a later step never undoes it. */
struct Guard {
  /** @brief The operator's armed file. */
  std::string armed_;
  /** @brief The operator's stop file. */
  std::string stop_file_;
  /** @brief The run's id. */
  std::string run_;
  /** @brief The run's lock. */
  const OwnerLock* lock_{nullptr};
  /** @brief The stop the loop reads. */
  atl::runner::StopSource* stop_{nullptr};
  /** @brief The event log. */
  EventLog* events_{nullptr};
  /** @brief The run's end: one tick after the last bar's period end. */
  std::chrono::system_clock::time_point end_{};
  /** @brief Why the run stopped itself, the first reason: `end`, `not_armed`, `lock_lost`, `signal`, `event_log` or
   *  `operator`; empty while it has not. */
  std::string stopped_;
  /** @brief Whether the lock was lost at a step. */
  bool lock_lost_{false};
  /** @brief The strongest intent the stop file has named for this run. */
  std::optional<atl::runner::StopIntent> operator_intent_;
  /** @brief The stop file's texts already noted as ignored. */
  std::set<std::string> noted_;
  /** @brief Events held until the start line is written. */
  std::vector<json::object> held_;
  /** @brief Whether events are held (before the start line). */
  bool holding_{true};

  /**
   * @brief Writes an event, or holds it before the start line; a failed write quiesces the run.
   * @param event The event.
   */
  void emit(json::object event) {
    if (holding_) {
      held_.push_back(std::move(event));
      return;
    }
    if (!events_->write(event)) {
      latch("event_log", atl::runner::StopIntent::QUIESCE, std::nullopt);
    }
  }

  /**
   * @brief Writes the held events after the start line.
   */
  void release_held() {
    holding_ = false;
    auto held = std::move(held_);
    held_.clear();
    for (auto& event : held) {
      emit(std::move(event));
    }
  }

  /**
   * @brief Latches a stop: the first reason is kept, and the library keeps the strongest intent.
   * @param reason The reason.
   * @param intent The intent.
   * @param at The instant, for the event.
   */
  void latch(const std::string& reason, atl::runner::StopIntent intent, std::optional<std::chrono::system_clock::time_point> at) {
    const auto held = stop_->intent();
    const bool stronger = !held || atl::runner::stop_intent_strength(intent) > atl::runner::stop_intent_strength(*held);
    stop_->request(intent);
    if (stopped_.empty()) {
      stopped_ = reason;
    }
    if (reason != "event_log" && events_->ok() && (stronger || reason == "operator")) {
      json::object event{{"event", "stop"}, {"reason", reason}, {"intent", atl::runner::stop_intent_name(intent)}};
      if (at) {
        event["at"] = at_text(*at);
      }
      emit(std::move(event));
    }
  }

  /**
   * @brief Reads the operator's stop file: this run's line requests its intent (one event per stronger intent);
   *   anything else is noted once.
   * @param now The instant.
   */
  void read_stop(std::chrono::system_clock::time_point now) {
    const auto text = read_stop_file(stop_file_);
    if (!text || text->empty()) {
      return;
    }
    const auto intent = stop_intent_of(*text, run_);
    if (!intent) {
      if (noted_.insert(*text).second) {
        emit(json::object{{"event", "note"}, {"at", at_text(now)}, {"message", "the stop file names no stop for this run; ignored"}});
      }
      return;
    }
    if (!operator_intent_ ||
        atl::runner::stop_intent_strength(*intent) > atl::runner::stop_intent_strength(*operator_intent_)) {
      operator_intent_ = intent;
      latch("operator", *intent, now);
    }
  }

  /**
   * @brief The checks of one step.
   * @param now The instant.
   */
  void check(std::chrono::system_clock::time_point now) {
    std::error_code ec;
    if (!std::filesystem::exists(armed_, ec)) {
      latch("not_armed", atl::runner::StopIntent::QUIESCE, now);
    }
    if (!lock_->still_at_path()) {
      lock_lost_ = true;
      latch("lock_lost", atl::runner::StopIntent::QUIESCE, now);
    }
    if (interrupted()) {
      latch("signal", atl::runner::StopIntent::QUIESCE, now);
    }
    read_stop(now);
    if (!(now < end_)) {
      latch("end", atl::runner::StopIntent::WITHDRAW, now);
    }
  }
};
#endif

/** @brief The clock was asked to move past the run's bound: the loop ends and the outcome is unknown. */
struct RunBound : std::runtime_error {
  RunBound() : std::runtime_error("the run's clock reached its bound, one day after the last bar") {}
};

/**
 * @brief The run's clock: the mock market's time; waiting moves the market up to the bound, then calls `after_step`.
 */
class RunClock {
public:
  /**
   * @brief Builds the clock.
   * @param market The market; borrowed.
   * @param bound The latest instant the market may be moved to.
   * @param after_step Called after every step (the order scan, the heartbeat and the guard).
   */
  RunClock(atl::mock::Market& market, std::chrono::system_clock::time_point bound, std::function<void()> after_step)
  : market_(&market), bound_(bound), after_step_(std::move(after_step)) {}

  /**
   * @brief The market's time.
   * @return It.
   */
  std::chrono::system_clock::time_point now() const noexcept { return market_->now(); }

  /**
   * @brief The market's time only moves forward, so it is its own monotonic reading.
   * @return It, as a steady-clock time point.
   */
  std::chrono::steady_clock::time_point monotonic_now() const noexcept {
    return std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(market_->now().time_since_epoch())
    );
  }

  /**
   * @brief One step: the market moved by `duration`, never past the bound, then `after_step`.
   * @param duration How long the loop waits.
   * @throws RunBound When the market is already at the bound.
   * @throws std::runtime_error When the market halts (a fill it cannot cost or record).
   */
  void wait_for(std::chrono::nanoseconds duration) {
    if (!(market_->now() < bound_)) {
      throw RunBound();
    }
    if (duration > std::chrono::nanoseconds::zero()) {
      auto target = market_->now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
      if (bound_ < target) {
        target = bound_;
      }
      market_->advance_to(target);
    }
    if (after_step_) {
      after_step_();
    }
  }

private:
  atl::mock::Market* market_;
  std::chrono::system_clock::time_point bound_;
  std::function<void()> after_step_;
};

}  // namespace driver
}  // namespace cmdatl
