#pragma once

// The operator's stop file for runs: `<stem>.stop` beside the operator's file, one line `WORD RUN_ID` per run, `WORD`
// one of `quiesce` and `withdraw`. A run's driver reads it at every step and never writes it; `run stop` is the only
// writer. Both read and parse it with these functions, so they agree on every line.

#include "./files.hpp"

#include <algotradelib/runner>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace cmdatl {

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

/** @brief The stop file as read: its text, or why a file that exists was not read. */
struct StopFileRead {
  /** @brief The text, when read. */
  std::optional<std::string> text_;
  /** @brief Why an existing file was not read (a link, not a regular file, above `MAX_STOP_BYTES`, unreadable); empty
   *  when it was read or does not exist. */
  std::string problem_;
};

/**
 * @brief Reads the operator's stop file as `read_stop_file` does, and says why a file that exists was not read, so
 *   a stop the run cannot read is never ignored without a trace.
 * @param path The file.
 * @return The text, or the problem.
 */
inline StopFileRead read_stop_file_checked(const std::string& path) {
  StopFileRead read;
  ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    const int error = errno;
    if (error == ELOOP) {
      read.problem_ = "the stop file is a symbolic link, which a run never follows";
    } else if (error != ENOENT) {
      read.problem_ = std::string("the stop file could not be opened: ") + std::strerror(error);
    }
    return read;
  }
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    read.problem_ = "the stop file is not a regular file";
    return read;
  }
  std::string text;
  const auto status = read_whole(fd.get(), MAX_STOP_BYTES, text).status_;
  if (status == FileRead::Status::TOO_LARGE) {
    read.problem_ = "the stop file is above " + std::to_string(MAX_STOP_BYTES) + " bytes: remove the lines of runs that have ended";
    return read;
  }
  if (status != FileRead::Status::OK) {
    read.problem_ = "the stop file could not be read whole";
    return read;
  }
  read.text_ = std::move(text);
  return read;
}
#endif

/**
 * @brief A stop line's words, split at spaces, tabs and carriage returns, as the driver reads a line: `run stop` splits
 *   the lines it keeps or retires the same way.
 * @param line The line, without its newline.
 * @return The words.
 */
inline std::vector<std::string> stop_line_words(const std::string& line) {
  std::vector<std::string> words;
  std::size_t at = 0;
  while (at < line.size()) {
    const auto start = line.find_first_not_of(" \t\r", at);
    if (start == std::string::npos) {
      break;
    }
    const auto end = line.find_first_of(" \t\r", start);
    words.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
    at = end == std::string::npos ? line.size() : end;
  }
  return words;
}

/**
 * @brief One stop line: `WORD RUN_ID`, `WORD` one of `quiesce` and `withdraw`; spaces, tabs and a carriage return
 *   around or between the two words are allowed (a tolerance that can only add stops), the run id must match exactly.
 * @param line The line, without its newline.
 * @param run This run's id.
 * @return The intent it names for this run, or nothing (another run's line, or not a stop line).
 */
inline std::optional<atl::runner::StopIntent> stop_line_intent(const std::string& line, const std::string& run) {
  const auto words = stop_line_words(line);
  if (words.size() != 2 || words[1] != run) {
    return std::nullopt;
  }
  const auto& word = words[0];
  if (word == "quiesce") {
    return atl::runner::StopIntent::QUIESCE;
  }
  if (word == "withdraw") {
    return atl::runner::StopIntent::WITHDRAW;
  }
  return std::nullopt;
}

/**
 * @brief The stop file's intent for this run: the strongest of the lines naming it. The file holds one line per run
 *   (`WORD RUN_ID`, each ending in a newline, the last one optionally not), so runs started from one operator file
 *   never erase each other's requests.
 * @param text The stop file's text.
 * @param run This run's id.
 * @return The strongest intent its lines name, or nothing.
 */
inline std::optional<atl::runner::StopIntent> stop_intent_of(const std::string& text, const std::string& run) {
  std::optional<atl::runner::StopIntent> strongest;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (const auto intent = stop_line_intent(line, run)) {
      if (!strongest || atl::runner::stop_intent_strength(*intent) > atl::runner::stop_intent_strength(*strongest)) {
        strongest = intent;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return strongest;
}

}  // namespace cmdatl
