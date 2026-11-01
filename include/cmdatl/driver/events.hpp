#pragma once

// A run's event log and its reply.
//
// The event log is `events.jsonl` in the run's directory: created exclusively, one JSON object per line, each line one
// whole write, not synced (the journal is the authority; the events describe the run). The reply is one JSON object
// on stdout in one write, always with `env`, `run`, `result` and `exit`.

#include "../core/errors.hpp"
#include "../core/reply.hpp"
#include "../paper/time.hpp"
#include "../system/files.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace driver {

namespace json = boost::json;

/** @brief The event log's format. */
inline constexpr int EVENT_FORMAT = 1;

/**
 * @brief An instant as a run writes it: KST with `+09:00`.
 * @param at The instant.
 * @return The text.
 */
inline std::string at_text(std::chrono::system_clock::time_point at) {
  return paper::kst_iso(at);
}

/**
 * @brief The `next` the catalog gives a code.
 * @param code The code.
 * @return The text, or empty.
 */
inline std::string next_of(const std::string& code) {
  for (const auto& entry : error_catalog()) {
    if (entry.code_ == code) {
      return entry.next_;
    }
  }
  return {};
}

#if !defined(_WIN32)
/** @brief The run's event log; a line that cannot be written is remembered, and the run stops placing. */
class EventLog {
public:
  /**
   * @brief Creates the log; it must not exist.
   * @param path The file.
   */
  explicit EventLog(const std::string& path) {
    fd_.reset(open_lifted(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    failed_ = fd_.get() < 0;
  }

  /**
   * @brief Whether every line so far was written whole.
   * @return Whether it was.
   */
  bool ok() const noexcept {
    return !failed_;
  }

  /**
   * @brief Writes one event.
   * @param event The event.
   * @return Whether it was written whole.
   */
  bool write(const json::object& event) {
    if (failed_) {
      return false;
    }
    const auto line = json::serialize(event) + "\n";
    std::size_t written = 0;
    if (write_whole(fd_.get(), line.data(), line.size(), written) != 0) {
      failed_ = true;
    }
    return !failed_;
  }

private:
  ScopedFd fd_;
  bool failed_{false};
};

/**
 * @brief Writes the reply on stdout in one write, always with `exit` and `result`.
 * @param body The reply's body, with `env` and `run`.
 * @param exit The exit.
 * @return The exit, as an int.
 */
inline int write_run_reply(json::object body, Exit exit) {
  body["exit"] = static_cast<int>(exit);
  if (!body.contains("result")) {
    body["result"] = exit == Exit::OK ?
      "ended" :
      exit == Exit::REFUSED ?
      "refused" :
      exit == Exit::UNKNOWN ?
      "unknown" :
      "failed";
  }
  const auto line = json::serialize(body) + "\n";
  std::size_t written = 0;
  (void)write_whole(STDOUT_FILENO, line.data(), line.size(), written);
  return static_cast<int>(exit);
}

/**
 * @brief A refusal or failure as the run's reply.
 * @param base The fields every reply carries (`env`, `run`).
 * @param exit The exit.
 * @param code The code.
 * @param message What happened.
 * @param next What to do; the catalog's when empty.
 * @param extra The refusal's own fields.
 * @return The exit, as an int.
 */
inline int run_error(
  const json::object& base, Exit exit, const std::string& code, const std::string& message, std::string next = "",
  json::object extra = {}
) {
  json::object body = base;
  if (next.empty()) {
    next = next_of(code);
  }
  body["error"] = json::object{{"code", code}, {"message", message}, {"next", next}};
  for (const auto& [key, value] : extra) {
    body[key] = value;
  }
  return write_run_reply(std::move(body), exit);
}

/**
 * @brief A command's reply (a refusal) as the run's reply: its error and fields under the run's base.
 * @param base The fields every reply carries.
 * @param reply The reply.
 * @return The exit, as an int.
 */
inline int relay(const json::object& base, const Reply& reply) {
  json::object body = base;
  for (const auto& [key, value] : reply.body_) {
    body[key] = value;
  }
  return write_run_reply(std::move(body), reply.exit_);
}
#endif

} // namespace driver
} // namespace cmdatl
