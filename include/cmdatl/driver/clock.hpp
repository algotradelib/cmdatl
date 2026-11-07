#pragma once

// A run's clock and its step guard.
//
// The clock is the mock market's: waiting moves the market, never past a bound one day after the last bar (no bar is
// left to fill against there); asked to wait at the bound it throws `RunBound`, which ends the loop, so a stop whose
// closing pass cannot finish never spins. After each step the run scans what the market did and the guard runs.
//
// The guard latches, at every step: the armed file missing (quiesce), the run's lock no longer at its path (quiesce),
// SIGINT, SIGTERM or SIGHUP (withdraw), an event line not written (withdraw), and the run's end (withdraw).

#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "../system/process.hpp"
#include "./events.hpp"

#include <algotradelib/mock>
#include <algotradelib/runner>

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;

#if !defined(_WIN32)
/** @brief The run's step guard; each check latches, and a later step never undoes it. */
struct Guard {
  /** @brief The operator's armed file. */
  std::string armed_;
  /** @brief The run's lock. */
  const OwnerLock* lock_{nullptr};
  /** @brief The stop the loop reads. */
  atl::runner::StopSource* stop_{nullptr};
  /** @brief The event log. */
  EventLog* events_{nullptr};
  /** @brief The run's end: one tick after the last bar's period end. */
  std::chrono::system_clock::time_point end_{};
  /** @brief Why the run stopped itself, the first reason: `end`, `not_armed`, `lock_lost`, `signal` or `event_log`;
   *  empty while it has not. */
  std::string stopped_;
  /** @brief Whether the lock was lost at a step. */
  bool lock_lost_{false};
  /** @brief Events held until the start line is written. */
  std::vector<json::object> held_;
  /** @brief Whether events are held (before the start line). */
  bool holding_{true};

  /**
   * @brief Writes an event, or holds it before the start line; a failed write withdraws the run, since nobody reads
   *   it any more.
   * @param event The event.
   */
  void emit(json::object event) {
    if (holding_) {
      held_.push_back(std::move(event));
      return;
    }
    if (!events_->write(event)) {
      latch("event_log", atl::runner::StopIntent::WITHDRAW, std::nullopt);
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
  void latch(
    const std::string& reason, atl::runner::StopIntent intent, std::optional<std::chrono::system_clock::time_point> at
  ) {
    const auto held = stop_->intent();
    const bool stronger = !held || atl::runner::stop_intent_strength(intent) > atl::runner::stop_intent_strength(*held);
    stop_->request(intent);
    if (stopped_.empty()) {
      stopped_ = reason;
    }
    if (reason != "event_log" && events_->ok() && stronger) {
      json::object event{{"event", "stop"}, {"reason", reason}, {"intent", atl::runner::stop_intent_name(intent)}};
      if (at) {
        event["at"] = at_text(*at);
      }
      emit(std::move(event));
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
      latch("signal", atl::runner::StopIntent::WITHDRAW, now);
    }
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
  std::chrono::system_clock::time_point now() const noexcept {
    return market_->now();
  }

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

/**
 * @brief A run's clock on a broker's account: the wall clock. A wait sleeps in slices of at most one second and calls
 *   `after_slice` after each, and the first wait during which a signal arrives returns at the end of that slice, so
 *   the loop takes up the stop within about a second; later waits (the closing wait, finalization) run their length.
 *
 * Copies share their state, as the loop and the journal each hold the clock.
 */
class WallClock {
public:
  /** @brief How long one slice of a wait sleeps at most. */
  static constexpr std::chrono::seconds SLICE{1};

  /**
   * @brief Builds the clock on the system's clocks.
   * @param after_slice Called after every slice (the heartbeat and the guard); may be empty.
   */
  explicit WallClock(std::function<void()> after_slice)
  : WallClock(
      std::move(after_slice),
      [] {
        return std::chrono::system_clock::now();
      },
      [] {
        return std::chrono::steady_clock::now();
      },
      [](std::chrono::nanoseconds duration) {
        std::this_thread::sleep_for(duration);
      },
      [] {
        return interrupted();
      }
    ) {}

  /**
   * @brief Builds the clock on given clocks, for tests.
   * @param after_slice Called after every slice; may be empty.
   * @param now The wall clock.
   * @param monotonic The steady clock.
   * @param sleep Sleeps for a duration.
   * @param signalled Whether a stop signal has arrived.
   */
  WallClock(
    std::function<void()> after_slice, std::function<std::chrono::system_clock::time_point()> now,
    std::function<std::chrono::steady_clock::time_point()> monotonic,
    std::function<void(std::chrono::nanoseconds)> sleep, std::function<bool()> signalled
  )
  : state_(std::make_shared<State>()) {
    state_->after_slice_ = std::move(after_slice);
    state_->now_ = std::move(now);
    state_->monotonic_ = std::move(monotonic);
    state_->sleep_ = std::move(sleep);
    state_->signalled_ = std::move(signalled);
  }

  /**
   * @brief The wall clock.
   * @return It.
   */
  std::chrono::system_clock::time_point now() const {
    return state_->now_();
  }

  /**
   * @brief The steady clock.
   * @return It.
   */
  std::chrono::steady_clock::time_point monotonic_now() const {
    return state_->monotonic_();
  }

  /**
   * @brief Sleeps for `duration` in slices, calling `after_slice` after each; returns early once, at the end of the
   *   first slice after which a signal is seen.
   * @param duration How long to wait.
   */
  void wait_for(std::chrono::nanoseconds duration) {
    auto& st = *state_;
    const auto until = st.monotonic_() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
    do {
      const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(until - st.monotonic_());
      const auto slice = std::min<std::chrono::nanoseconds>(std::max(left, std::chrono::nanoseconds::zero()), SLICE);
      if (slice > std::chrono::nanoseconds::zero()) {
        st.sleep_(slice);
      }
      if (st.after_slice_) {
        st.after_slice_();
      }
      if (!st.woke_ && st.signalled_()) {
        st.woke_ = true;
        return;
      }
    } while (st.monotonic_() < until);
  }

private:
  struct State {
    std::function<void()> after_slice_;
    std::function<std::chrono::system_clock::time_point()> now_;
    std::function<std::chrono::steady_clock::time_point()> monotonic_;
    std::function<void(std::chrono::nanoseconds)> sleep_;
    std::function<bool()> signalled_;
    bool woke_{false};
  };
  std::shared_ptr<State> state_;
};

} // namespace driver
} // namespace cmdatl
