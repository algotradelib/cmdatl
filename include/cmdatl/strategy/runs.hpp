#pragma once

// `run start` and `run status`.
//
// `run start` checks the spec through `atlc spec`, builds the program's driver (`build.hpp`), writes the run's
// description, and starts the driver detached under a small supervisor, then waits for its start line. A run trades on
// its own mock market inside the driver's process and opens no account; the run's directory
// (`<state root>/runs/<account>/<run id>`) holds everything about it: `run.json`, the driver's copy, `pid`,
// `supervisor`, `events.jsonl`, `reply.json`, `stderr.log`, `exit.json`, the journal and the lock.
//
// `run status` reads those files, bounded and never trusted unless whole and valid, and tells a driver that died from
// one still running by probing the run's lock, which a driver past its start line holds for its whole life.

#include "../account/account.hpp"
#include "../account/operator.hpp"
#include "../core/errors.hpp"
#include "../core/reply.hpp"
#include "../core/runs.hpp"
#include "../core/table.hpp"
#include "../system/files.hpp"
#include "../system/process.hpp"
#include "./atlc.hpp"
#include "./build.hpp"
#include "./strategy.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#include <sys/syscall.h>
#endif
#endif

namespace cmdatl {
namespace runs {

namespace json = boost::json;

/** @brief How long `run start` waits for the start line, the driver's own lock wait included with a margin. */
inline constexpr std::chrono::seconds START_WAIT{15};
/** @brief How often the waits look again. */
inline constexpr std::chrono::milliseconds POLL{100};
/** @brief The supervisor's exit when it failed before spawning anything, used by no other path. */
inline constexpr int SUPERVISOR_NOT_SPAWNED = 125;
/** @brief The largest `exit.json` and `reply.json` read. */
inline constexpr std::size_t SMALL_RECORD = 64u << 10;
/** @brief The largest `pid` and `supervisor` read. */
inline constexpr std::size_t PID_RECORD = 32;
/** @brief The largest first line of `events.jsonl` read (the start line). */
inline constexpr std::size_t FIRST_LINE = 4096;
/** @brief How much of the end of `events.jsonl` is read for the last heartbeat and the end. */
inline constexpr std::size_t EVENTS_TAIL = 64u << 10;
/** @brief The largest event log whose events are counted. */
inline constexpr std::uint64_t COUNT_LIMIT = 64ull << 20;
/** @brief The most `[key, value]` pairs a run takes from `atlc spec`. */
inline constexpr std::size_t MAX_ARGS = 1000;
/** @brief The longest argument value. */
inline constexpr std::size_t MAX_VALUE = 4096;

/**
 * @brief The catalog's `next` for a code.
 * @param code The code.
 * @return The text, or empty.
 */
inline std::string next_for(const std::string& code) {
  for (const auto& entry : error_catalog()) {
    if (entry.code_ == code) {
      return entry.next_;
    }
  }
  return {};
}

/**
 * @brief An instant in UTC, `YYYY-MM-DDTHH:MM:SSZ`, for wall-clock fields.
 * @param at The instant.
 * @return The text.
 */
inline std::string utc_text(std::chrono::system_clock::time_point at) {
  const auto seconds = std::chrono::system_clock::to_time_t(at);
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &seconds);
#else
  gmtime_r(&seconds, &parts);
#endif
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
  return text;
}

/** @brief What `atlc spec --json` gave that a run consumes, checked. */
struct SpecFields {
  /** @brief The program, absolute. */
  std::string program_;
  /** @brief Its struct. */
  std::string struct_;
  /** @brief The indicators setting. */
  std::string indicators_;
  /** @brief The order type. */
  std::string order_type_;
  /** @brief The limit offset, as written. */
  std::string offset_;
  /** @brief atlc's arguments, each `key=value`. */
  std::vector<std::string> args_;
  /** @brief The same, as `[key, value]` pairs. */
  json::array pairs_;
};

/**
 * @brief Whether a text is an argument key: 1 to 128 of `A-Z a-z 0-9 _ . -`.
 * @param key The text.
 * @return Whether it is.
 */
inline bool valid_key(const std::string& key) {
  if (key.empty() || key.size() > 128) {
    return false;
  }
  return std::all_of(key.begin(), key.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
  });
}

/**
 * @brief Whether a text is a struct atlc can have named a program: `[A-Z][A-Za-z0-9]{0,63}`, neither `Params` nor `Lane`.
 * @param name The text.
 * @return Whether it is.
 */
inline bool valid_struct(const std::string& name) {
  if (name.empty() || name.size() > 64 || name[0] < 'A' || name[0] > 'Z' || name == "Params" || name == "Lane") {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  });
}

/**
 * @brief Whether a text is an offset as a run takes it: digits, a point, digits, below 1 (such as `0.01`).
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_offset(const std::string& text) {
  static const std::regex grammar("0(\\.[0-9]{1,8})?|\\.[0-9]{1,8}");
  return text.size() <= 10 && std::regex_match(text, grammar);
}

/**
 * @brief Reads and checks every field of `atlc spec --json` a run consumes; a failure names the field (exit 1
 *   `atlc.failed`, or exit 2 `spec.invalid` for a store run).
 * @param envelope The reply.
 * @param fields Set to the fields.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> spec_fields(const json::object& envelope, SpecFields& fields) {
  const auto bad = [](const std::string& field, const std::string& why) {
    return error_reply(Exit::FAILED, "atlc.failed", "atlc spec gave a " + field + " a run cannot use: " + why, "check atlc with cmdatl env");
  };
  const auto* program = envelope.if_contains("program");
  if (program == nullptr || !program->is_object()) {
    return bad("program", "missing");
  }
  const auto* path = program->as_object().if_contains("path");
  const auto* name = program->as_object().if_contains("struct");
  if (path == nullptr || !path->is_string() || name == nullptr || !name->is_string()) {
    return bad("program", "no path or struct");
  }
  fields.program_ = std::string(path->as_string());
  fields.struct_ = std::string(name->as_string());
  std::error_code ec;
  if (fields.program_.empty() || fields.program_.front() != '/' || !std::filesystem::is_regular_file(fields.program_, ec)) {
    return bad("program.path", "not an absolute path to a file");
  }
  if (!valid_struct(fields.struct_)) {
    return bad("program.struct", fields.struct_);
  }
  const auto* build = envelope.if_contains("build");
  const auto* indicators = build != nullptr && build->is_object() ? build->as_object().if_contains("indicators") : nullptr;
  if (indicators == nullptr || !indicators->is_string()) {
    return bad("build.indicators", "missing");
  }
  fields.indicators_ = std::string(indicators->as_string());
  if (fields.indicators_ != "incremental" && fields.indicators_ != "naive" && fields.indicators_ != "exact") {
    return bad("build.indicators", fields.indicators_);
  }
  const auto* store = envelope.if_contains("store");
  if (store != nullptr && store->is_bool() && store->as_bool()) {
    return error_reply(Exit::USAGE, "spec.invalid", "a run reads files, not a store: give [data] a file", "fix the run spec");
  }
  const auto* orders = envelope.if_contains("orders");
  if (orders == nullptr || !orders->is_object()) {
    return bad("orders", "missing");
  }
  const auto* type = orders->as_object().if_contains("type");
  const auto* offset = orders->as_object().if_contains("offset");
  if (type == nullptr || !type->is_string() || (type->as_string() != "market" && type->as_string() != "limit")) {
    return bad("orders.type", "not market or limit");
  }
  fields.order_type_ = std::string(type->as_string());
  if (offset != nullptr && !offset->is_null()) {
    if (!offset->is_string() || !valid_offset(std::string(offset->as_string()))) {
      return bad("orders.offset", "not a decimal below 1");
    }
    fields.offset_ = std::string(offset->as_string());
  }
  const auto* args = envelope.if_contains("args");
  if (args == nullptr || !args->is_array()) {
    return bad("args", "missing");
  }
  if (args->as_array().size() > MAX_ARGS) {
    return bad("args", "more than " + std::to_string(MAX_ARGS) + " pairs");
  }
  std::set<std::string> seen;
  for (const auto& pair : args->as_array()) {
    if (!pair.is_array() || pair.as_array().size() != 2 || !pair.as_array()[0].is_string() || !pair.as_array()[1].is_string()) {
      return bad("args", "a pair that is not two strings");
    }
    const std::string key(pair.as_array()[0].as_string());
    const std::string value(pair.as_array()[1].as_string());
    if (!valid_key(key)) {
      return bad("args", "the key '" + key.substr(0, 140) + "'");
    }
    if (value.size() > MAX_VALUE || value.find('\0') != std::string::npos) {
      return bad("args", "the value of " + key + " is longer than 4096 bytes or holds a NUL");
    }
    if (!seen.insert(key).second) {
      return bad("args", "the key " + key + " twice");
    }
    fields.args_.push_back(key + "=" + value);
    fields.pairs_.push_back(pair);
  }
  return std::nullopt;
}

/** @brief How a file of the run's directory read. */
struct FileText {
  /** @brief Whether it exists. */
  bool present_{false};
  /** @brief Whether it is a regular file within its bound and was read whole. */
  bool ok_{false};
  /** @brief Its bytes. */
  std::string text_;
};

#if !defined(_WIN32)
/**
 * @brief Reads a file as state files are read: not followed through a link, without blocking on a FIFO, a regular
 *   file, at most `limit` bytes.
 * @param path The file.
 * @param limit The bound.
 * @return What was read.
 */
inline FileText read_bounded(const std::filesystem::path& path, std::size_t limit) {
  FileText out;
  ScopedFd fd(open_lifted(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    out.present_ = errno != ENOENT;
    return out;
  }
  out.present_ = true;
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_size) > limit) {
    return out;
  }
  out.ok_ = read_whole(fd.get(), limit, out.text_).status_ == FileRead::Status::OK;
  return out;
}
#endif

/**
 * @brief A whole JSON object from a record's text: it must end with a newline when `line` (a writer may be mid-line).
 * @param text The text.
 * @param line Whether the record is a line that must be terminated.
 * @return The object, or nothing.
 */
inline std::optional<json::object> record_of(const std::string& text, bool line) {
  if (line && (text.empty() || text.back() != '\n')) {
    return std::nullopt;
  }
  return one_object(text);
}

/**
 * @brief A positive process id from a record's text.
 * @param text The text: digits, an optional newline.
 * @return The pid, or nothing.
 */
inline std::optional<long> pid_of(const std::string& text) {
  auto digits = text;
  if (!digits.empty() && digits.back() == '\n') {
    digits.pop_back();
  }
  if (digits.empty() || digits.size() > 10 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  const long value = std::stol(digits);
  if (value <= 0 || value > 2147483647L) {
    return std::nullopt;
  }
  return value;
}

/** @brief What the run's directory holds, each record whole and valid or reported as damaged. */
struct Records {
  /** @brief `run.json`. */
  std::optional<json::object> run_;
  /** @brief Whether `run.json` exists but failed its checks. */
  bool run_damaged_{false};
  /** @brief Whether `exit.json` exists. */
  bool exit_present_{false};
  /** @brief Whether `exit.json` is whole and valid. */
  bool exit_valid_{false};
  /** @brief `exit.json`'s `spawned`. */
  bool spawned_{false};
  /** @brief `exit.json`'s `exit`. */
  std::optional<int> exit_code_;
  /** @brief `exit.json`'s `signal`. */
  std::optional<int> signal_;
  /** @brief `reply.json`, whole and valid. */
  std::optional<json::object> reply_;
  /** @brief `reply.json`'s `exit`. */
  std::optional<int> reply_exit_;
  /** @brief Whether `reply.json` holds a whole line that failed its checks: damaged evidence, never success. */
  bool reply_damaged_{false};
  /** @brief The driver's pid. */
  std::optional<long> pid_;
  /** @brief The supervisor's pid. */
  std::optional<long> supervisor_;
  /** @brief The start line. */
  std::optional<json::object> start_;
  /** @brief The last heartbeat's instant. */
  std::optional<std::string> clock_;
  /** @brief The end line. */
  std::optional<json::object> end_;
  /** @brief Events by kind, when counted. */
  std::map<std::string, std::int64_t> counts_;
  /** @brief Whether they were counted (the log within `COUNT_LIMIT`). */
  bool counted_{false};
};

/** @brief `exit.json`, checked. */
struct ExitRecord {
  /** @brief Whether the driver was spawned. */
  bool spawned_{false};
  /** @brief Its exit code, when it exited. */
  std::optional<int> exit_;
  /** @brief The signal that ended it, when one did. */
  std::optional<int> signal_;
};

/**
 * @brief An integer field that must be present: an integer within bounds, or `null` when allowed.
 * @param object The record.
 * @param key The field.
 * @param low The least value.
 * @param high The greatest value.
 * @param out Set to the value, or reset for `null`.
 * @return Whether the field is valid.
 */
inline bool bounded_field(const json::object& object, const char* key, std::int64_t low, std::int64_t high, std::optional<int>& out) {
  const auto* value = object.if_contains(key);
  if (value == nullptr) {
    return false;
  }
  if (value->is_null()) {
    out.reset();
    return true;
  }
  if (!value->is_int64() || value->as_int64() < low || value->as_int64() > high) {
    return false;
  }
  out = static_cast<int>(value->as_int64());
  return true;
}

/**
 * @brief `exit.json` as the supervisor writes it, every field present with its type: `format` 1, `pid`, `exit` and
 *   `signal` (each a number or `null`), `spawned`, `ended`; a record not spawned names no pid, exit or signal, and one
 *   spawned names a pid and not both an exit and a signal.
 * @param object The record.
 * @return It, or nothing when it is not one.
 */
inline std::optional<ExitRecord> exit_record_of(const json::object& object) {
  ExitRecord record;
  const auto* format = object.if_contains("format");
  const auto* pid = object.if_contains("pid");
  const auto* spawned = object.if_contains("spawned");
  const auto* ended = object.if_contains("ended");
  if (format == nullptr || !format->is_int64() || format->as_int64() != 1 || pid == nullptr || !pid->is_int64() ||
      pid->as_int64() < 0 || pid->as_int64() > 2147483647LL || spawned == nullptr || !spawned->is_bool() || ended == nullptr ||
      !ended->is_string()) {
    return std::nullopt;
  }
  if (!bounded_field(object, "exit", 0, 255, record.exit_) || !bounded_field(object, "signal", 1, 64, record.signal_)) {
    return std::nullopt;
  }
  record.spawned_ = spawned->as_bool();
  if (!record.spawned_ && (pid->as_int64() != 0 || record.exit_ || record.signal_)) {
    return std::nullopt;
  }
  if (record.spawned_ && (pid->as_int64() == 0 || (record.exit_ && record.signal_))) {
    return std::nullopt;
  }
  return record;
}

/**
 * @brief Whether `reply.json` is the driver's reply for this run: `run` naming it, `exit` from 0 to 5 and `result`
 *   as the driver words it for that exit; a success (`exit` 0) carries a run's summary (`outcome`, `executions`,
 *   `owed_at_end`) and no error, any other exit an error with a code.
 * @param object The record.
 * @param run The run's id.
 * @return Whether it is.
 */
inline bool valid_reply(const json::object& object, const std::string& run) {
  const auto* id = object.if_contains("run");
  const auto* code = object.if_contains("exit");
  const auto* result = object.if_contains("result");
  if (id == nullptr || !id->is_string() || id->as_string() != run || code == nullptr || !code->is_int64() ||
      code->as_int64() < 0 || code->as_int64() > 5 || result == nullptr || !result->is_string()) {
    return false;
  }
  const auto exit = code->as_int64();
  const auto* expected = exit == 0 ? "ended" : exit == 3 ? "refused" : exit == 4 ? "unknown" : "failed";
  if (result->as_string() != expected) {
    return false;
  }
  const auto* error = object.if_contains("error");
  if (exit == 0) {
    const auto* outcome = object.if_contains("outcome");
    const auto* executions = object.if_contains("executions");
    const auto* owed = object.if_contains("owed_at_end");
    return error == nullptr && outcome != nullptr && outcome->is_string() && executions != nullptr && executions->is_int64() &&
      owed != nullptr && owed->is_array() && owed->as_array().empty();
  }
  if (error == nullptr || !error->is_object()) {
    return false;
  }
  const auto* name = error->as_object().if_contains("code");
  return name != nullptr && name->is_string() && !name->as_string().empty();
}

/**
 * @brief The event kind of a complete event line, as the driver writes it (`{"event":"KIND",...`).
 * @param line The line, without its newline.
 * @return The kind, or empty.
 */
inline std::string event_kind(const std::string& line) {
  const std::string prefix = "{\"event\":\"";
  if (line.compare(0, prefix.size(), prefix) != 0) {
    return {};
  }
  const auto end = line.find('"', prefix.size());
  return end == std::string::npos ? std::string() : line.substr(prefix.size(), end - prefix.size());
}

#if !defined(_WIN32)
/**
 * @brief Reads the run's directory.
 * @param dir The directory.
 * @param run The run's id, which every record must name.
 * @return The records.
 */
inline Records read_records(const std::filesystem::path& dir, const std::string& run) {
  Records records;
  const auto described = read_bounded(dir / "run.json", RUN_RECORD_MAX);
  if (described.present_) {
    const auto object = described.ok_ ? record_of(described.text_, false) : std::nullopt;
    const auto* id = object ? object->if_contains("run") : nullptr;
    const auto* format = object ? object->if_contains("format") : nullptr;
    if (id != nullptr && id->is_string() && id->as_string() == run && format != nullptr && format->is_int64() && format->as_int64() == 1) {
      records.run_ = object;
    } else {
      records.run_damaged_ = true;
    }
  }
  const auto ended = read_bounded(dir / "exit.json", SMALL_RECORD);
  records.exit_present_ = ended.present_;
  if (ended.ok_) {
    if (const auto object = record_of(ended.text_, false)) {
      if (const auto record = exit_record_of(*object)) {
        records.spawned_ = record->spawned_;
        records.exit_code_ = record->exit_;
        records.signal_ = record->signal_;
        records.exit_valid_ = true;
      }
    }
  }
  const auto replied = read_bounded(dir / "reply.json", SMALL_RECORD);
  if (replied.ok_) {
    // An unterminated reply is a writer mid-line, not evidence; a whole line that fails its checks is damaged.
    if (!replied.text_.empty() && replied.text_.back() == '\n') {
      const auto object = record_of(replied.text_, true);
      if (object && valid_reply(*object, run)) {
        records.reply_ = object;
        records.reply_exit_ = static_cast<int>(object->at("exit").as_int64());
      } else {
        records.reply_damaged_ = true;
      }
    }
  } else if (replied.present_) {
    records.reply_damaged_ = true;
  }
  for (const auto& [name, field] : {std::make_pair("pid", &records.pid_), std::make_pair("supervisor", &records.supervisor_)}) {
    const auto text = read_bounded(dir / name, PID_RECORD);
    if (text.ok_) {
      *field = pid_of(text.text_);
    }
  }
  ScopedFd events(open_lifted((dir / "events.jsonl").c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  struct stat info{};
  if (events.get() >= 0 && ::fstat(events.get(), &info) == 0 && S_ISREG(info.st_mode)) {
    const auto size = static_cast<std::uint64_t>(info.st_size);
    std::string head(std::min<std::uint64_t>(size, FIRST_LINE), '\0');
    const auto got = ::pread(events.get(), head.data(), head.size(), 0);
    if (got > 0) {
      head.resize(static_cast<std::size_t>(got));
      const auto newline = head.find('\n');
      if (newline != std::string::npos) {
        const auto first = record_of(head.substr(0, newline + 1), true);
        const auto* kind = first ? first->if_contains("event") : nullptr;
        const auto* id = first ? first->if_contains("run") : nullptr;
        if (kind != nullptr && kind->is_string() && kind->as_string() == "start" && id != nullptr && id->is_string() && id->as_string() == run) {
          records.start_ = first;
        }
      }
    }
    const auto from = size > EVENTS_TAIL ? size - EVENTS_TAIL : 0;
    std::string tail(static_cast<std::size_t>(size - from), '\0');
    const auto read_tail = tail.empty() ? 0 : ::pread(events.get(), tail.data(), tail.size(), static_cast<off_t>(from));
    if (read_tail > 0) {
      tail.resize(static_cast<std::size_t>(read_tail));
      std::size_t start = from == 0 ? 0 : tail.find('\n') + 1;
      while (start < tail.size()) {
        const auto end = tail.find('\n', start);
        if (end == std::string::npos) {
          break;
        }
        const auto line = tail.substr(start, end - start);
        const auto kind = event_kind(line);
        if (kind == "clock" || kind == "end") {
          if (const auto object = one_object(line)) {
            if (kind == "end") {
              records.end_ = object;
            } else if (const auto* at = object->if_contains("at"); at != nullptr && at->is_string()) {
              records.clock_ = std::string(at->as_string());
            }
          }
        }
        start = end + 1;
      }
    }
    if (size <= COUNT_LIMIT) {
      std::string all;
      if (read_whole(events.get(), static_cast<std::size_t>(COUNT_LIMIT) + 1, all).status_ == FileRead::Status::OK || !all.empty()) {
        std::size_t start = 0;
        while (start < all.size()) {
          const auto end = all.find('\n', start);
          if (end == std::string::npos) {
            break;
          }
          const auto kind = event_kind(all.substr(start, std::min<std::size_t>(end - start, 64)));
          if (!kind.empty()) {
            ++records.counts_[kind];
          }
          start = end + 1;
        }
        records.counted_ = true;
      }
    }
  }
  return records;
}
#endif

/** @brief What the lock probe found. */
enum class Probe {
  /** @brief Another process holds the run's lock: a driver may be alive. */
  HELD,
  /** @brief Nobody holds it: the driver is gone. */
  FREE,
  /** @brief Nothing could be established (missing, replaced, not a regular file, an error). */
  UNKNOWN,
};

#if !defined(_WIN32)
/**
 * @brief Probes the run's lock: the file the start line names (its device and inode), opened without creating or
 *   following anything, `flock` tried without waiting and released at once; nothing is written.
 * @param dir The run's directory.
 * @param start The start line.
 * @return What it found.
 */
inline Probe probe_lock(const std::filesystem::path& dir, const json::object& start) {
  const auto* lock = start.if_contains("lock");
  if (lock == nullptr || !lock->is_object()) {
    return Probe::UNKNOWN;
  }
  const auto* device = lock->as_object().if_contains("device");
  const auto* inode = lock->as_object().if_contains("inode");
  if (device == nullptr || !device->is_int64() || inode == nullptr || !inode->is_int64()) {
    return Probe::UNKNOWN;
  }
  ScopedFd fd(open_lifted((dir / "lock").c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    return Probe::UNKNOWN;
  }
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::int64_t>(info.st_dev) != device->as_int64() ||
      static_cast<std::int64_t>(info.st_ino) != inode->as_int64()) {
    return Probe::UNKNOWN;
  }
  if (::flock(fd.get(), LOCK_EX | LOCK_NB) == 0) {
    ::flock(fd.get(), LOCK_UN);
    return Probe::FREE;
  }
  return errno == EWOULDBLOCK ? Probe::HELD : Probe::UNKNOWN;
}
#endif

/** @brief A run's state, as `run status` reports it. */
struct State {
  /** @brief `starting`, `running`, `ended`, `refused`, `failed`, `lost` or `unknown`. */
  std::string state_;
  /** @brief The run's own exit, when known. */
  std::optional<int> run_exit_;
  /** @brief The run's error code, when it has one. */
  std::string run_error_;
};

/** @brief What a process id says about a process. */
enum class Alive {
  /** @brief It may be alive (a pid can be reused, so this claims nothing about identity). */
  MAYBE,
  /** @brief It is surely gone. */
  GONE,
  /** @brief There is no pid to ask about. */
  NO_PID,
};

/**
 * @brief The run's state from its records, the processes and the probe: a pure function.
 * @param records The records (read again after a probe that found the lock free).
 * @param supervisor What the supervisor's pid says.
 * @param driver What the driver's pid says.
 * @param probe The probe, meaningful only after a start line with no ending.
 * @return The state.
 */
inline State run_state(const Records& records, Alive supervisor, Alive driver, Probe probe) {
  State state;
  const auto unknown = [&state](const std::string& code) {
    state.state_ = "unknown";
    state.run_error_ = code;
    return state;
  };
  if ((records.exit_present_ && !records.exit_valid_) || records.reply_damaged_) {
    return unknown("unknown.run_ended");
  }
  if (records.reply_) {
    const int code = *records.reply_exit_;
    // A reply beside a record saying no driver was spawned, or one whose exit differs from the record's: contradictory.
    if (records.exit_valid_ && (!records.spawned_ || (records.exit_code_ && *records.exit_code_ != code))) {
      return unknown("unknown.run_ended");
    }
    state.run_exit_ = code;
    if (const auto* error = records.reply_->if_contains("error"); error != nullptr && error->is_object()) {
      if (const auto* name = error->as_object().if_contains("code"); name != nullptr && name->is_string()) {
        state.run_error_ = std::string(name->as_string());
      }
    }
    if (code == 3) {
      state.state_ = "refused";
    } else if (code == 0 || code == 4) {
      state.state_ = "ended";
    } else {
      state.state_ = "failed";
    }
    return state;
  }
  if (records.exit_present_) {
    if (!records.spawned_) {
      state.state_ = "failed";
      state.run_exit_ = 1;
      state.run_error_ = "run.failed";
      return state;
    }
    return unknown("unknown.run_ended");
  }
  if (!records.pid_) {
    if (supervisor == Alive::MAYBE) {
      state.state_ = "starting";
      return state;
    }
    return unknown("unknown.run_ended");
  }
  if (!records.start_) {
    if (driver == Alive::MAYBE || supervisor == Alive::MAYBE) {
      state.state_ = "starting";
      return state;
    }
    return unknown("unknown.run_ended");
  }
  switch (probe) {
    case Probe::FREE:
      state.state_ = "lost";
      state.run_error_ = "unknown.run_ended";
      return state;
    case Probe::HELD:
      if (driver == Alive::GONE) {
        return unknown("unknown.run_ended");
      }
      state.state_ = "running";
      return state;
    case Probe::UNKNOWN:
      break;
  }
  state.state_ = "unknown";
  return state;
}

/** @brief What `run start`'s wait learns from a run's ending records, before any start line. */
enum class EarlyEnd {
  /** @brief No ending yet: keep waiting. */
  NONE,
  /** @brief A valid `exit.json` says no driver was spawned, and no reply contradicts it: nothing was launched. */
  NOT_LAUNCHED,
  /** @brief A valid reply: relay it under the state `run_state` gives. */
  RELAY,
  /** @brief Anything else that ended (a spawned driver without a reply, a damaged record): unknown. */
  UNKNOWN,
};

/**
 * @brief The wait's decision on a run's ending records; only a validated `spawned: false` record says nothing was
 *   launched.
 * @param records The records.
 * @return The decision.
 */
inline EarlyEnd early_end(const Records& records) {
  if (!records.exit_present_ && !records.reply_ && !records.reply_damaged_) {
    return EarlyEnd::NONE;
  }
  if (records.exit_present_ && records.exit_valid_ && !records.spawned_ && !records.reply_ && !records.reply_damaged_) {
    return EarlyEnd::NOT_LAUNCHED;
  }
  if (records.reply_ && !records.reply_damaged_) {
    return EarlyEnd::RELAY;
  }
  return EarlyEnd::UNKNOWN;
}

/**
 * @brief Whether a state is final: nothing more will change it.
 * @param state The state.
 * @return Whether it is.
 */
inline bool terminal(const State& state) {
  return state.state_ != "starting" && state.state_ != "running";
}

/**
 * @brief The exit a waiting command answers for a final state: the run's own, or 4 when it is not known.
 * @param state The state.
 * @return The exit.
 */
inline Exit exit_for(const State& state) {
  if (state.state_ == "lost" || state.state_ == "unknown" || !state.run_exit_) {
    return Exit::UNKNOWN;
  }
  const int code = *state.run_exit_;
  if (code >= 0 && code <= 5) {
    return static_cast<Exit>(code);
  }
  return Exit::UNKNOWN;
}

#if !defined(_WIN32)
/**
 * @brief What a pid says, by `kill(pid, 0)`.
 * @param pid The pid.
 * @return Maybe alive, surely gone, or no pid.
 */
inline Alive alive(std::optional<long> pid) {
  if (!pid) {
    return Alive::NO_PID;
  }
  if (::kill(static_cast<pid_t>(*pid), 0) == 0 || errno == EPERM) {
    return Alive::MAYBE;
  }
  return Alive::GONE;
}

/**
 * @brief Reads the run and decides its state: after a start line with no ending, the lock is probed, and a probe that
 *   finds it free is followed by a second read, since the driver writes its reply before it releases the lock.
 * @param dir The run's directory.
 * @param run The run's id.
 * @return The records and the state.
 */
inline std::pair<Records, State> evaluate(const std::filesystem::path& dir, const std::string& run) {
  auto records = read_records(dir, run);
  auto probe = Probe::UNKNOWN;
  if (records.start_ && !records.exit_present_ && !records.reply_) {
    probe = probe_lock(dir, *records.start_);
    if (probe == Probe::FREE) {
      records = read_records(dir, run);
    }
  }
  const auto state = run_state(records, alive(records.supervisor_), alive(records.pid_), probe);
  return {std::move(records), state};
}

/**
 * @brief The reply `run status` gives for a run, and `run start --wait` once it ends.
 * @param dir The run's directory.
 * @param run The run's id.
 * @param records The records.
 * @param state The state.
 * @return The reply (exit 0: it read the run).
 */
inline Reply status_body(const std::filesystem::path& dir, const std::string& run, const Records& records, const State& state) {
  Reply reply;
  reply.body_["run"] = run;
  reply.body_["state"] = state.state_;
  reply.body_["run_exit"] = state.run_exit_ ? json::value(*state.run_exit_) : json::value(nullptr);
  reply.body_["run_error"] = state.run_error_.empty() ? json::value(nullptr) : json::value(state.run_error_);
  json::object progress;
  if (records.counted_) {
    json::object counts;
    for (const auto& [kind, count] : records.counts_) {
      counts[kind] = count;
    }
    progress["events"] = counts;
  } else {
    progress["events"] = nullptr;
  }
  progress["counted"] = records.counted_;
  progress["clock"] = records.clock_ ? json::value(*records.clock_) : json::value(nullptr);
  reply.body_["progress"] = progress;
  const auto* at = records.start_ ? records.start_->if_contains("at") : nullptr;
  reply.body_["from"] = at != nullptr ? *at : json::value(nullptr);
  const auto* to = records.start_ ? records.start_->if_contains("to") : nullptr;
  reply.body_["to"] = to != nullptr ? *to : json::value(nullptr);
  reply.body_["summary"] = records.reply_ && state.state_ == "ended" ? json::value(*records.reply_) : json::value(nullptr);
  if (records.run_damaged_) {
    reply.body_["run_record"] = "damaged";
  }
  reply.body_["dir"] = dir.string();
  return reply;
}
#endif

/**
 * @brief A fresh run id: `r` and 12 lowercase hex digits from 6 random bytes; no clock in it.
 * @return The id, or nothing when no random bytes could be read.
 */
inline std::optional<std::string> new_run_id() {
#if defined(_WIN32)
  return std::nullopt;
#else
  ScopedFd fd(open_lifted("/dev/urandom", O_RDONLY | O_CLOEXEC));
  unsigned char bytes[6];
  if (fd.get() < 0 || ::read(fd.get(), bytes, sizeof bytes) != static_cast<ssize_t>(sizeof bytes)) {
    return std::nullopt;
  }
  char text[16];
  std::snprintf(text, sizeof text, "r%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return std::string(text);
#endif
}

#if !defined(_WIN32)
/** @brief Everything the supervisor needs, prepared before the fork so the child allocates nothing to find it. */
struct Supervise {
  /** @brief The driver's argv, null-terminated. */
  std::vector<char*> argv_;
  /** @brief The driver's environment, null-terminated. */
  std::vector<char*> envp_;
  /** @brief Paths in the run's directory. */
  std::string driver_, reply_, stderr_, pid_, pid_tmp_, exit_, exit_tmp_;
};

/**
 * @brief Writes a small record by a temporary and a rename, with no allocation.
 * @param tmp The temporary.
 * @param path The record.
 * @param text The bytes.
 * @param size How many.
 * @return Whether it was written and renamed.
 */
inline bool write_record_raw(const char* tmp, const char* path, const char* text, std::size_t size) noexcept {
  const int fd = ::open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t written = 0;
  const bool ok = write_whole(fd, text, size, written) == 0;
  ::close(fd);
  return ok && ::rename(tmp, path) == 0;
}

/**
 * @brief Writes `exit.json`.
 * @param s The paths.
 * @param pid The driver's pid, or 0.
 * @param status `waitpid`'s status, or -1 when there is none.
 * @param spawned Whether the driver was spawned.
 * @return Whether it was written.
 */
inline bool write_exit_record(const Supervise& s, long pid, int status, bool spawned) noexcept {
  char code[16] = "null";
  char signal[16] = "null";
  if (status >= 0 && WIFEXITED(status)) {
    std::snprintf(code, sizeof code, "%d", WEXITSTATUS(status));
  } else if (status >= 0 && WIFSIGNALED(status)) {
    std::snprintf(signal, sizeof signal, "%d", WTERMSIG(status));
  }
  const std::time_t now = std::time(nullptr);
  std::tm parts{};
  gmtime_r(&now, &parts);
  char ended[32];
  std::strftime(ended, sizeof ended, "%Y-%m-%dT%H:%M:%SZ", &parts);
  char text[256];
  const int size = std::snprintf(
    text, sizeof text, "{\"format\":1,\"pid\":%ld,\"exit\":%s,\"signal\":%s,\"spawned\":%s,\"ended\":\"%s\"}\n", pid, code,
    signal, spawned ? "true" : "false", ended
  );
  return size > 0 && write_record_raw(s.exit_tmp_.c_str(), s.exit_.c_str(), text, static_cast<std::size_t>(size));
}

/**
 * @brief Closes every descriptor from 3 up, whatever `RLIMIT_NOFILE` says: one inherited above a lowered soft limit
 *   would otherwise survive into the supervisor and the driver. macOS lists the process's open descriptors; Linux
 *   closes the range at once, or lists `/proc/self/fd`; a loop to the hard limit is the last resort.
 */
inline void close_inherited_descriptors() noexcept {
#if defined(__APPLE__)
  const int bytes = proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
  if (bytes > 0) {
    // Room for descriptors opened since the size was asked; the child of a fork may allocate.
    const std::size_t room = static_cast<std::size_t>(bytes) + 64 * sizeof(proc_fdinfo);
    auto* listed = static_cast<proc_fdinfo*>(std::malloc(room));
    if (listed != nullptr) {
      const int got = proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, listed, static_cast<int>(room));
      if (got > 0) {
        const auto count = static_cast<std::size_t>(got) / sizeof(proc_fdinfo);
        for (std::size_t k = 0; k < count; ++k) {
          if (listed[k].proc_fd >= 3) {
            ::close(listed[k].proc_fd);
          }
        }
        std::free(listed);
        return;
      }
      std::free(listed);
    }
  }
#elif defined(__linux__)
#if defined(SYS_close_range)
  if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) {
    return;
  }
#endif
  if (DIR* dir = ::opendir("/proc/self/fd")) {
    std::vector<int> open;
    const int own = ::dirfd(dir);
    while (const auto* entry = ::readdir(dir)) {
      char* end = nullptr;
      const long fd = std::strtol(entry->d_name, &end, 10);
      if (end != entry->d_name && *end == '\0' && fd >= 3 && fd != own) {
        open.push_back(static_cast<int>(fd));
      }
    }
    ::closedir(dir);
    for (const int fd : open) {
      ::close(fd);
    }
    return;
  }
#endif
  struct rlimit limit{};
  long top = 1 << 16;
  if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
    const rlim_t hard = limit.rlim_max == RLIM_INFINITY ? (1 << 20) : limit.rlim_max;
    top = static_cast<long>(std::min<rlim_t>(std::max(hard, limit.rlim_cur == RLIM_INFINITY ? hard : limit.rlim_cur), 1 << 20));
  }
  for (long fd = 3; fd < top; ++fd) {
    ::close(static_cast<int>(fd));
  }
}

/**
 * @brief The supervisor, the forked child's whole life: detached from the caller's session, signals and descriptors,
 *   it spawns the run's driver, records its pid, waits for it, records how it ended, and exits. A failure before the
 *   spawn exits `SUPERVISOR_NOT_SPAWNED`; nothing returns into cmdatl.
 * @param s What it needs.
 */
[[noreturn]] inline void supervise(const Supervise& s) noexcept {
  struct sigaction ignored{};
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  if (::sigaction(SIGINT, &ignored, nullptr) != 0 || ::sigaction(SIGTERM, &ignored, nullptr) != 0 ||
      ::sigaction(SIGHUP, &ignored, nullptr) != 0) {
    ::_exit(SUPERVISOR_NOT_SPAWNED);
  }
  if (::setsid() < 0) {
    ::_exit(SUPERVISOR_NOT_SPAWNED);
  }
  const int null = ::open("/dev/null", O_RDWR);
  if (null < 0 || ::dup2(null, STDIN_FILENO) < 0 || ::dup2(null, STDOUT_FILENO) < 0 || ::dup2(null, STDERR_FILENO) < 0) {
    ::_exit(SUPERVISOR_NOT_SPAWNED);
  }
  close_inherited_descriptors();
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  if (posix_spawn_file_actions_init(&actions) != 0 || posix_spawnattr_init(&attr) != 0) {
    ::_exit(SUPERVISOR_NOT_SPAWNED);
  }
  sigset_t defaults;
  sigset_t empty;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGINT);
  sigaddset(&defaults, SIGTERM);
  sigaddset(&defaults, SIGHUP);
  sigaddset(&defaults, SIGPIPE);
  sigemptyset(&empty);
  // Opened by the spawn itself, so each survives POSIX_SPAWN_CLOEXEC_DEFAULT.
  int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (error == 0) {
    error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, s.reply_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  }
  if (error == 0) {
    error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, s.stderr_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  }
  if (error == 0) {
    error = posix_spawnattr_setsigdefault(&attr, &defaults);
  }
  if (error == 0) {
    error = posix_spawnattr_setsigmask(&attr, &empty);
  }
  if (error == 0) {
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    // The driver inherits its three standard descriptors and nothing else, whatever the supervisor still holds.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    error = posix_spawnattr_setflags(&attr, flags);
  }
  if (error != 0) {
    ::_exit(SUPERVISOR_NOT_SPAWNED);
  }
  pid_t pid = 0;
  error = posix_spawn(&pid, s.driver_.c_str(), &actions, &attr, s.argv_.data(), s.envp_.data());
  if (error != 0) {
    const int log = ::open(s.stderr_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log >= 0) {
      const char* message = std::strerror(error);
      std::size_t written = 0;
      (void)write_whole(log, "the driver could not be spawned: ", 33, written);
      (void)write_whole(log, message, std::strlen(message), written);
      (void)write_whole(log, "\n", 1, written);
      ::close(log);
    }
    ::_exit(write_exit_record(s, 0, -1, false) ? 0 : SUPERVISOR_NOT_SPAWNED);
  }
  char text[32];
  const int size = std::snprintf(text, sizeof text, "%ld\n", static_cast<long>(pid));
  (void)write_record_raw(s.pid_tmp_.c_str(), s.pid_.c_str(), text, static_cast<std::size_t>(size));
  int status = -1;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      status = -1;
      break;
    }
  }
  (void)write_exit_record(s, static_cast<long>(pid), status, true);
  ::_exit(0);
}

/** @brief Removes a run's directory when `run start` fails before anything could have started. */
class DirGuard {
public:
  /**
   * @brief Guards a directory.
   * @param dir It.
   */
  explicit DirGuard(std::filesystem::path dir) : dir_(std::move(dir)) {}

  DirGuard(const DirGuard&) = delete;
  DirGuard& operator=(const DirGuard&) = delete;

  /** @brief Removes the directory unless disarmed. */
  ~DirGuard() {
    if (armed_) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }

  /** @brief Keeps the directory: a run may now exist. */
  void disarm() noexcept { armed_ = false; }

private:
  std::filesystem::path dir_;
  bool armed_{true};
};
#endif

/**
 * @brief A driver's reply relayed as `run start`'s, under its own state.
 * @param run The run's id.
 * @param records The records.
 * @param state The state.
 * @param dir The run's directory.
 * @return The reply.
 */
inline Reply relayed(const std::string& run, const Records& records, const State& state, const std::filesystem::path& dir) {
#if defined(_WIN32)
  (void)run;
  (void)records;
  (void)state;
  (void)dir;
  return Reply{};
#else
  auto reply = status_body(dir, run, records, state);
  reply.exit_ = exit_for(state);
  if (records.reply_) {
    if (const auto* error = records.reply_->if_contains("error"); error != nullptr && error->is_object()) {
      reply.body_["error"] = *error;
    }
  }
  if (reply.exit_ != Exit::OK && !reply.body_.contains("error")) {
    reply.body_["error"] = json::object{
      {"code", state.run_error_.empty() ? "unknown.run_ended" : state.run_error_},
      {"message", "the run is " + state.state_},
      {"next", "cmdatl run status " + run},
    };
  }
  reply.body_["result"] = state.state_ == "refused" ? "refused" : reply.exit_ == Exit::OK ? "ok" : reply.exit_ == Exit::UNKNOWN ? "unknown" : "failed";
  return reply;
#endif
}

/**
 * @brief An unknown start: the run may be trading.
 * @param run The run's id.
 * @param message What happened.
 * @param state The state to report.
 * @return The reply (exit 4).
 */
inline Reply unknown_start(const std::string& run, const std::string& message, const std::string& state = "starting") {
  auto reply = error_reply(Exit::UNKNOWN, "unknown.run_start", message, "the run may be trading: cmdatl run status " + run + "; do not start it again");
  reply.body_["run"] = run;
  reply.body_["state"] = state;
  return reply;
}

/**
 * @brief `run start SPEC [-P k=v]... [--wait]`; see the file comment.
 * @param invocation The arguments.
 * @param snapshot The operator's file, read and `OK`.
 * @param output Where progress goes.
 * @return The reply.
 */
inline Reply start_reply(const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)snapshot;
  (void)output;
  return error_reply(Exit::FAILED, "unavailable.platform", "runs run on macOS and Linux only in this version");
#else
  const auto& config = *snapshot.config_;
  const auto spec_given = invocation.get("SPEC");
  const auto found = invocation.values_.find("param");
  const auto params = found == invocation.values_.end() ? std::vector<std::string>{} : found->second;
  const bool wait = invocation.has("wait");
  output.progress("cmdatl: starting a run of " + spec_given);
  if (interrupted()) {
    return interrupted_reply();
  }
  // Before anything could have started, a failure is the command's own (exit 1) and the run's directory is removed.
  std::string run;
  std::filesystem::path dir;
  std::optional<DirGuard> guard;
  Supervise s;
  std::vector<std::string> argv_owned;
  std::vector<std::string> envp_owned;
  pid_t supervisor = -1;
  try {
    const auto atlc = find_atlc(&snapshot);
    const std::string install = "install atlc, or name it with CMDATL_ATLC; cmdatl env shows what was found";
    if (atlc.path_.empty() || atlc.version_.empty()) {
      return error_reply(Exit::FAILED, "atlc.missing", atlc.path_.empty() ? "atlc was not found" : atlc.path_ + " did not answer --version", install);
    }
    if (!at_least(atlc.version_, MINIMUM_ATLC)) {
      return error_reply(Exit::FAILED, "atlc.unsupported", "atlc " + atlc.version_ + " is not a version cmdatl reads", "install a newer atlc");
    }
    const auto spec = std::filesystem::absolute(spec_given).string();
    std::vector<std::string> argv{atlc.path_, "spec", spec};
    for (const auto& param : params) {
      argv.push_back("-P");
      argv.push_back(param);
    }
    argv.push_back("--json");
    const auto checked = run_child(argv, [&output](const std::string& line) { output.progress(line); });
    if (auto failure = spec_failure(checked)) {
      return std::move(*failure);
    }
    SpecFields fields;
    if (auto refused = spec_fields(*spec_envelope(checked.stdout_), fields)) {
      return std::move(*refused);
    }
    // The early refusals, with nothing built; the driver checks each again, and its checks are the ones that count.
    if (const auto refusal = spec_refusal(fields.args_)) {
      auto reply = error_reply(Exit::USAGE, "spec.invalid", refusal->second, "fix the run spec");
      reply.body_["key"] = refusal->first;
      return reply;
    }
    for (const auto& arg : fields.args_) {
      if (arg.rfind("instrument.", 0) == 0) {
        const auto symbol = arg.substr(arg.find('=') + 1);
        if (!config.allows(symbol)) {
          auto reply = error_reply(Exit::REFUSED, "refused.symbol_not_allowed", symbol + " is not in the operator's limits.symbols", next_for("refused.symbol_not_allowed"));
          reply.body_["symbol"] = symbol;
          return reply;
        }
      }
    }
    if (config.limits_.order_type_ == "limit" && fields.order_type_ != "limit") {
      return error_reply(Exit::REFUSED, "refused.order_type", "the run spec's [orders] type is " + fields.order_type_ + "; the operator allows limit orders only",
                         next_for("refused.order_type"));
    }
    if (!fields.offset_.empty() && !(std::stod(fields.offset_) <= config.limits_.collar_)) {
      return error_reply(Exit::REFUSED, "refused.collar", "the run spec's [orders] offset " + fields.offset_ + " is above the operator's collar " +
                         decimal_text(config.limits_.collar_), next_for("refused.collar"));
    }
    std::error_code ec;
    if (!std::filesystem::exists(config.armed_path(), ec)) {
      return error_reply(Exit::REFUSED, "refused.not_armed", "the operator's armed file does not exist", next_for("refused.not_armed"), true);
    }
    const auto id = new_run_id();
    if (!id) {
      return error_reply(Exit::FAILED, "run.failed", "no random bytes for the run's id", next_for("run.failed"), true);
    }
    run = *id;
    json::object described{
      {"format", 1},
      {"run", run},
      {"account", json::object{{"broker", config.broker_}, {"env", config.env_}, {"number", config.number_}}},
      {"config", config.path_},
      {"spec", spec},
      {"program", json::object{{"path", fields.program_}, {"struct", fields.struct_}}},
      {"args", fields.pairs_},
      {"atlc", atlc.version_},
      {"cmdatl", CMDATL_VERSION},
      {"started", utc_text(std::chrono::system_clock::now())},
    };
    json::array given;
    for (const auto& param : params) {
      given.push_back(json::value(param));
    }
    described["params_given"] = given;
    const auto record = json::serialize(described) + "\n";
    if (record.size() > RUN_RECORD_MAX) {
      return error_reply(Exit::USAGE, "spec.invalid", "the run's description would be " + std::to_string(record.size()) +
                         " bytes, above 64 KiB; fewer or shorter parameters", "fix the run spec");
    }
    const auto where = run_dir(config, run);
    if (!where) {
      return error_reply(Exit::FAILED, "state.unavailable", "neither XDG_STATE_HOME nor HOME names an absolute directory");
    }
    dir = *where;
    if (make_private_dirs(dir.parent_path()) != 0 || ::mkdir(dir.c_str(), 0700) != 0) {
      return error_reply(Exit::FAILED, "file.unwritable", "the run's directory " + dir.string() + " cannot be made");
    }
    guard.emplace(dir);
    if (auto failed = build::build_driver(
          build::Request{atlc.path_, atlc.version_, fields.program_, fields.struct_, fields.indicators_}, dir,
          [&output](const std::string& line) { output.progress(line); }
        )) {
      return std::move(*failed);
    }
    if (interrupted()) {
      return interrupted_reply();
    }
    if (build::replace_file(dir / "run.json", record) != 0) {
      return error_reply(Exit::FAILED, "file.unwritable", "the run's description cannot be written in " + dir.string());
    }
    // Everything the supervisor needs, before the fork.
    s.driver_ = (dir / "driver").string();
    s.reply_ = (dir / "reply.json").string();
    s.stderr_ = (dir / "stderr.log").string();
    s.pid_ = (dir / "pid").string();
    s.pid_tmp_ = (dir / ".pid.tmp").string();
    s.exit_ = (dir / "exit.json").string();
    s.exit_tmp_ = (dir / ".exit.json.tmp").string();
    argv_owned = {s.driver_, "run.id=" + run};
    argv_owned.insert(argv_owned.end(), fields.args_.begin(), fields.args_.end());
    envp_owned.clear();
    for (auto& entry : build::own_environment()) {
      if (entry.rfind("CMDATL_CONFIG=", 0) != 0) {
        envp_owned.push_back(std::move(entry));
      }
    }
    envp_owned.push_back("CMDATL_CONFIG=" + config.path_);
    for (auto& arg : argv_owned) {
      s.argv_.push_back(arg.data());
    }
    s.argv_.push_back(nullptr);
    for (auto& entry : envp_owned) {
      s.envp_.push_back(entry.data());
    }
    s.envp_.push_back(nullptr);
    output.progress("cmdatl: starting run " + run);
    supervisor = ::fork();
    if (supervisor < 0) {
      return error_reply(Exit::FAILED, "run.failed", "the run's supervisor could not be started: " + std::string(std::strerror(errno)), next_for("run.failed"), true);
    }
    if (supervisor == 0) {
      supervise(s);
    }
    guard->disarm();
  } catch (const std::exception& error) {
    if (supervisor > 0) {
      return unknown_start(run, error.what());
    }
    return error_reply(Exit::FAILED, "internal.error", error.what(), "nothing was started; report the defect with the command line");
  }
  // After the fork a run may exist: every failure is unknown (exit 4), and nothing kills the run.
  try {
    char text[32];
    const int size = std::snprintf(text, sizeof text, "%ld\n", static_cast<long>(supervisor));
    (void)build::replace_file(dir / "supervisor", std::string(text, static_cast<std::size_t>(size)));
    const auto deadline = std::chrono::steady_clock::now() + START_WAIT;
    std::optional<int> supervisor_status;
    while (true) {
      if (!supervisor_status) {
        int status = 0;
        const auto reaped = ::waitpid(supervisor, &status, WNOHANG);
        if (reaped == supervisor) {
          supervisor_status = status;
        }
      }
      auto records = read_records(dir, run);
      if (records.start_ && early_end(records) == EarlyEnd::NONE) {
        // The start line says the run began, not that it still runs: the same evidence as run status decides.
        const auto [read, state] = evaluate(dir, run);
        if (state.state_ != "running") {
          return relayed(run, read, state, dir);
        }
        if (!wait) {
          Reply reply;
          reply.body_["run"] = run;
          reply.body_["state"] = "running";
          for (const auto* key : {"symbols", "cap", "to"}) {
            if (const auto* value = records.start_->if_contains(key)) {
              reply.body_[key] = *value;
            }
          }
          if (const auto* at = records.start_->if_contains("at")) {
            reply.body_["from"] = *at;
          }
          reply.body_["next"] = "cmdatl run status " + run;
          return reply;
        }
        break;
      }
      switch (early_end(records)) {
        case EarlyEnd::NONE:
          break;
        case EarlyEnd::NOT_LAUNCHED: {
          auto reply = error_reply(Exit::FAILED, "run.failed", "the driver could not be spawned; nothing was sent", next_for("run.failed"), true);
          reply.body_["run"] = run;
          reply.body_["state"] = "failed";
          return reply;
        }
        case EarlyEnd::RELAY: {
          const auto [read, state] = evaluate(dir, run);
          return relayed(run, read, state, dir);
        }
        case EarlyEnd::UNKNOWN: {
          auto reply = error_reply(Exit::UNKNOWN, "unknown.run_ended", "the run ended without a reply it could be read from; it may have sent orders",
                                   next_for("unknown.run_ended"));
          reply.body_["run"] = run;
          reply.body_["state"] = "unknown";
          return reply;
        }
      }
      if (supervisor_status && !records.pid_) {
        // A second read: a fast driver and a failed `pid` write can leave exit.json there already.
        records = read_records(dir, run);
        if (!records.exit_present_ && !records.pid_ && WIFEXITED(*supervisor_status) && WEXITSTATUS(*supervisor_status) == SUPERVISOR_NOT_SPAWNED) {
          Supervise paths;
          paths.exit_ = s.exit_;
          paths.exit_tmp_ = s.exit_tmp_;
          (void)write_exit_record(paths, 0, -1, false);
          auto reply = error_reply(Exit::FAILED, "run.failed", "the run's supervisor failed before starting the driver; nothing was sent", next_for("run.failed"), true);
          reply.body_["run"] = run;
          reply.body_["state"] = "failed";
          return reply;
        }
        if (!records.exit_present_ && !records.pid_ && !records.start_) {
          return unknown_start(run, "the run's supervisor ended without recording the driver", "unknown");
        }
      }
      if (interrupted()) {
        return unknown_start(run, "interrupted: the run goes on in its own session");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return unknown_start(run, "no start line within " + std::to_string(START_WAIT.count()) + " s");
      }
      std::this_thread::sleep_for(POLL);
    }
    // --wait: until the run's state is final, the heartbeat as progress.
    std::string last_clock;
    auto last_print = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    while (true) {
      if (!supervisor_status) {
        int status = 0;
        if (::waitpid(supervisor, &status, WNOHANG) == supervisor) {
          supervisor_status = status;
        }
      }
      const auto [records, state] = evaluate(dir, run);
      if (terminal(state)) {
        return relayed(run, records, state, dir);
      }
      if (interrupted()) {
        auto reply = status_body(dir, run, records, state);
        reply.body_["note"] = "stopped waiting; the run goes on";
        return reply;
      }
      const auto now = std::chrono::steady_clock::now();
      if (records.clock_ && *records.clock_ != last_clock && now - last_print >= std::chrono::seconds(1)) {
        last_clock = *records.clock_;
        last_print = now;
        output.progress("cmdatl: run " + run + " at " + last_clock);
      }
      std::this_thread::sleep_for(POLL);
    }
  } catch (const std::exception& error) {
    return unknown_start(run, error.what());
  }
#endif
}

/**
 * @brief The newest run of the configured account: the latest `started` among its runs' descriptions.
 * @param config The operator's file.
 * @return Its id, or nothing.
 */
inline std::optional<std::string> latest_run(const OperatorConfig& config) {
#if defined(_WIN32)
  (void)config;
  return std::nullopt;
#else
  const auto root = state_root();
  if (!root) {
    return std::nullopt;
  }
  const auto dir = std::filesystem::path(*root) / "runs" / config.account_name();
  std::optional<std::pair<std::string, std::string>> newest;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const auto id = it->path().filename().string();
    if (!valid_run_id(id)) {
      continue;
    }
    const auto text = read_bounded(it->path() / "run.json", RUN_RECORD_MAX);
    const auto object = text.ok_ ? record_of(text.text_, false) : std::nullopt;
    const auto* started = object ? object->if_contains("started") : nullptr;
    if (started == nullptr || !started->is_string()) {
      continue;
    }
    const std::string at(started->as_string());
    if (!newest || newest->first < at || (newest->first == at && newest->second < id)) {
      newest = std::make_pair(at, id);
    }
  }
  if (!newest) {
    return std::nullopt;
  }
  return newest->second;
#endif
}

/**
 * @brief `run status [RUN]`: reads the run's directory, takes no lock and opens no account.
 * @param invocation The arguments.
 * @param snapshot The operator's file, read and `OK`.
 * @param output Unused: the reply is the whole answer.
 * @return The reply (exit 0 whenever the run was read).
 */
inline Reply status_reply(const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output) {
  (void)output;
#if defined(_WIN32)
  (void)invocation;
  (void)snapshot;
  return error_reply(Exit::FAILED, "unavailable.platform", "runs run on macOS and Linux only in this version");
#else
  const auto& config = *snapshot.config_;
  std::string run = invocation.get("RUN");
  if (!run.empty() && !valid_run_id(run)) {
    return error_reply(Exit::USAGE, "usage.bad_value", "a run id is r and 12 lowercase hex digits, not '" + run.substr(0, 40) + "'");
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
  const auto [records, state] = evaluate(*where, run);
  return status_body(*where, run, records, state);
#endif
}

}  // namespace runs
}  // namespace cmdatl
