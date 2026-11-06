#pragma once

#include "./process.hpp"

#include <cerrno>
#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace sys {

/** @brief How a read from the terminal ended. */
enum class ReadStatus {
  /** @brief A line was read. */
  OK,
  /** @brief The input ended before a line did. */
  END,
  /** @brief SIGINT, SIGTERM or SIGHUP arrived. */
  INTERRUPTED,
  /** @brief The read failed, the line was too long, or echo could not be turned off for a secret. */
  FAILED,
  /** @brief An answer that is not one of the choices. */
  INVALID,
};

/** @brief One line read from the terminal. */
struct LineRead {
  /** @brief How the read ended. */
  ReadStatus status_{ReadStatus::FAILED};
  /** @brief The line, without its line break; empty unless `status_` is `OK`. */
  std::string text_;
};

/** @brief The longest line or document read, in bytes. */
inline constexpr std::size_t INPUT_LIMIT = 65536;

/**
 * @brief Whether a descriptor is a terminal.
 * @param fd The descriptor.
 * @return Whether it is.
 */
inline bool is_terminal(int fd) {
#if defined(_WIN32)
  (void)fd;
  return false;
#else
  return ::isatty(fd) != 0;
#endif
}

#if !defined(_WIN32)
/**
 * @brief Turns a terminal's echo off for its lifetime and restores the whole terminal state when it ends, on every
 *   path out (return, exception, interruption).
 */
class EchoOff {
public:
  /**
   * @brief Saves the terminal's state and turns echo off (the line break still echoes).
   * @param fd The terminal.
   */
  explicit EchoOff(int fd) : fd_(fd) {
    if (::tcgetattr(fd_, &saved_) != 0) {
      return;
    }
    termios quiet = saved_;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    quiet.c_lflag |= ECHONL;
    if (::tcsetattr(fd_, TCSANOW, &quiet) != 0) {
      return;
    }
    termios now{};
    // tcsetattr succeeds when any one change applies, so the result is read back.
    on_ = ::tcgetattr(fd_, &now) == 0 && (now.c_lflag & ECHO) == 0;
    restore_ = true;
  }

  EchoOff(const EchoOff&) = delete;
  EchoOff& operator=(const EchoOff&) = delete;

  /** @brief Restores the saved state. */
  ~EchoOff() {
    if (restore_) {
      ::tcsetattr(fd_, TCSANOW, &saved_);
    }
  }

  /**
   * @brief Whether echo is off.
   * @return Whether it is.
   */
  bool on() const noexcept {
    return on_;
  }

private:
  int fd_;
  termios saved_{};
  bool on_{false};
  bool restore_{false};
};
#endif

/**
 * @brief Reads up to a line break or the end of input, a byte at a time, waking every 100 ms to check for SIGINT,
 *   SIGTERM or SIGHUP, so a signal that arrives just before the read cannot leave it waiting.
 * @param fd The descriptor.
 * @param whole Whether to read to the end of input instead of to a line break.
 * @return The read.
 */
inline LineRead read_input(int fd, bool whole) {
  LineRead result;
#if defined(_WIN32)
  (void)fd;
  (void)whole;
  return result;
#else
  std::string text;
  for (;;) {
    if (interrupted()) {
      result.status_ = ReadStatus::INTERRUPTED;
      return result;
    }
    pollfd ready{fd, POLLIN, 0};
    const int polled = ::poll(&ready, 1, 100);
    if (polled < 0) {
      if (errno == EINTR) {
        continue;
      }
      return result;
    }
    if (polled == 0) {
      continue;
    }
    char byte = 0;
    const auto got = ::read(fd, &byte, 1);
    if (got < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return result;
    }
    if (got == 0) {
      if (whole) {
        result.status_ = ReadStatus::OK;
        result.text_ = std::move(text);
      } else {
        result.status_ = ReadStatus::END;
      }
      return result;
    }
    if (!whole && byte == '\n') {
      if (!text.empty() && text.back() == '\r') {
        text.pop_back();
      }
      result.status_ = ReadStatus::OK;
      result.text_ = std::move(text);
      return result;
    }
    if (text.size() >= INPUT_LIMIT) {
      return result;
    }
    text.push_back(byte);
  }
#endif
}

/**
 * @brief Asks for one line: writes the prompt, then reads the answer.
 * @param fd The terminal.
 * @param prompts Where the prompt goes (stderr).
 * @param prompt The prompt.
 * @param hidden Whether to read with echo off (a secret); the secret is not read if echo stays on.
 * @return The read.
 */
inline LineRead ask(int fd, std::ostream& prompts, const std::string& prompt, bool hidden) {
#if defined(_WIN32)
  (void)fd;
  (void)hidden;
  prompts << prompt << std::flush;
  return {};
#else
  if (!hidden) {
    prompts << prompt << std::flush;
    return read_input(fd, false);
  }
  // Echo goes off before the prompt shows, so an answer typed as soon as it appears is not echoed.
  const EchoOff quiet(fd);
  prompts << prompt << std::flush;
  if (!quiet.on()) {
    prompts << "\n";
    return {};
  }
  return read_input(fd, false);
#endif
}

/**
 * @brief Asks for one of several choices by number: lists them, then reads the answer; Enter takes the default.
 * @param fd The terminal.
 * @param prompts Where the list and the prompt go (stderr).
 * @param question The question.
 * @param choices The choices, listed from 1.
 * @param fallback The choice Enter takes, or none.
 * @param chosen Where the chosen index (from 0) goes.
 * @return The read; `OK` with an empty `text_` when a choice was made, `INVALID` for an answer that is not one.
 */
inline LineRead choose(
  int fd, std::ostream& prompts, const std::string& question, const std::vector<std::string>& choices,
  std::optional<std::size_t> fallback, std::size_t& chosen
) {
  prompts << question << "\n";
  for (std::size_t i = 0; i < choices.size(); ++i) {
    prompts << "  " << i + 1 << ") " << choices[i] << (fallback && *fallback == i ? " (default)" : "") << "\n";
  }
  auto answer = ask(fd, prompts, "Choose 1-" + std::to_string(choices.size()) + ": ", false);
  if (answer.status_ != ReadStatus::OK) {
    return answer;
  }
  if (answer.text_.empty() && fallback) {
    chosen = *fallback;
    answer.text_.clear();
    return answer;
  }
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (answer.text_ == std::to_string(i + 1)) {
      chosen = i;
      answer.text_.clear();
      return answer;
    }
  }
  answer.status_ = ReadStatus::INVALID;
  answer.text_.clear();
  return answer;
}

} // namespace sys
} // namespace cmdatl
