#pragma once

#include "../account/operator.hpp"
#include "../core/table.hpp"
#include "../system/process.hpp"
#include "./atlc.hpp"

#include <cmdatl/guides.hpp>
#include <cmdatl/templates.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fstream>
#else
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {

/**
 * @brief The reply after SIGINT or SIGTERM: the child's group was stopped and nothing more was run.
 * @param who The child that was stopped.
 * @return The reply.
 */
inline Reply interrupted_reply(const std::string& who = "atlc") {
  return error_reply(
    Exit::INTERRUPTED, "interrupted", "interrupted by a signal: " + who + " was stopped and nothing more was run",
    "run the command again", true
  );
}

/**
 * @brief How a child ended, for a person.
 * @param result The child's end.
 * @param who The child, as the text names it.
 * @return The text, such as `atlc exited with 101`.
 */
inline std::string ending_of(const ChildResult& result, const std::string& who = "atlc") {
  switch (result.kind_) {
    case ChildResult::Kind::NOT_STARTED:
      return who + " could not be started: " + result.error_;
    case ChildResult::Kind::IO_FAILED:
      return who + "'s output was lost: " + result.error_;
    case ChildResult::Kind::INTERRUPTED:
      return who + " was interrupted";
    case ChildResult::Kind::FINISHED:
      break;
  }
  if (result.signaled_) {
#if defined(_WIN32)
    const char* name = nullptr;
#else
    const char* name = ::strsignal(result.code_);
#endif
    return who + " was ended by signal " + std::to_string(result.code_) +
      (name == nullptr ? "" : std::string(" (") + name + ")");
  }
  return who + " exited with " + std::to_string(result.code_);
}

/**
 * @brief A failure reply about atlc, carrying atlc's last stderr lines as `stderr`.
 * @param exit The exit code.
 * @param code The error code.
 * @param message What went wrong.
 * @param result The child's end.
 * @param next What to do next.
 * @return The reply.
 */
inline Reply atlc_failure(
  Exit exit, const std::string& code, const std::string& message, const ChildResult& result, const std::string& next
) {
  auto reply = error_reply(exit, code, message, next);
  json::array lines;
  for (const auto& line : result.stderr_tail_) {
    lines.push_back(json::value(line));
  }
  reply.body_["stderr"] = std::move(lines);
  return reply;
}

/**
 * @brief A text that is exactly one JSON object, whitespace around it allowed.
 * @param text The text.
 * @return The object, or nothing when the text is anything else (empty, malformed, two values, an array).
 */
inline std::optional<json::object> one_object(const std::string& text) {
  boost::system::error_code error;
  auto value = json::parse(text, error);
  if (error || !value.is_object()) {
    return std::nullopt;
  }
  return std::move(value.as_object());
}

/**
 * @brief The message of a failure that carries atlc's diagnostics: the first error's, and how many follow.
 * @param errors The diagnostics.
 * @param fallback The message when there is none.
 * @return The message.
 */
inline std::string first_error(const json::array& errors, const std::string& fallback) {
  if (errors.empty()) {
    return fallback;
  }
  auto message = std::string(errors.front().as_object().at("message").as_string());
  if (errors.size() > 1) {
    message += " (and " + std::to_string(errors.size() - 1) + " more)";
  }
  return message;
}

/**
 * @brief atlc's stderr lines joined, the message of a failure atlc printed as text.
 * @param result The child's end.
 * @param fallback The message when it printed nothing.
 * @return The message.
 */
inline std::string stderr_text(const ChildResult& result, const std::string& fallback) {
  std::string text;
  for (const auto& line : result.stderr_tail_) {
    text += (text.empty() ? "" : "\n") + line;
  }
  return text.empty() ? fallback : text;
}

/**
 * @brief atlc's last stderr line, the message of a build or replay failure (the whole tail is in `stderr`).
 * @param result The child's end.
 * @return The line, or `no message`.
 */
inline std::string last_line(const ChildResult& result) {
  return result.stderr_tail_.empty() ? std::string("no message") : result.stderr_tail_.back();
}

/** @brief atlc as a strategy command finds it, or why it cannot run it. */
struct LocatedAtlc {
  /** @brief What was found. */
  Atlc atlc_;
  /** @brief The reply when atlc cannot be run: missing, too old, the operator's file invalid, or a signal. */
  std::optional<Reply> refusal_;
};

/**
 * @brief Finds atlc for a strategy command: `CMDATL_ATLC`, else the operator's file's `atlc` (read once, only then),
 *   else `PATH`, with its version checked.
 * @return What was found, or the refusal.
 */
inline LocatedAtlc locate_atlc() {
  LocatedAtlc located;
  std::optional<ConfigSnapshot> snapshot;
  if (!variable("CMDATL_ATLC")) {
    snapshot = load_operator_config();
  }
  located.atlc_ = find_atlc(snapshot ? &*snapshot : nullptr);
  const auto& atlc = located.atlc_;
  if (atlc.config_error_) {
    auto reply = config_reply(*snapshot);
    reply.body_["error"].as_object()["code"] = atlc.config_error_->code_;
    located.refusal_ = std::move(reply);
    return located;
  }
  if (interrupted()) {
    located.refusal_ = interrupted_reply();
    return located;
  }
  const std::string install = "install atlc, or name it with CMDATL_ATLC; cmdatl env shows what was found";
  if (atlc.path_.empty()) {
    const auto where = atlc.requested_.empty() ?
      std::string("atlc was not found on PATH") :
      "CMDATL_ATLC names " + atlc.requested_ + ", which was not found on PATH";
    located.refusal_ = error_reply(Exit::FAILED, "atlc.missing", where, install);
  } else if (atlc.version_.empty()) {
    const auto from = atlc.source_ == "config" ? " (the operator's file's atlc)" : std::string();
    located.refusal_ =
      error_reply(Exit::FAILED, "atlc.missing", atlc.path_ + from + " did not answer --version", install);
  } else if (!at_least(atlc.version_, MINIMUM_ATLC)) {
    located.refusal_ = error_reply(
      Exit::FAILED, "atlc.unsupported",
      "atlc " + atlc.version_ + " is not a version cmdatl reads (at least " + MINIMUM_ATLC + ")", "install a newer atlc"
    );
  }
  return located;
}

/** @brief The longest chapter name `strategy reference` passes to atlc. */
inline constexpr std::size_t MAX_TOPIC = 64;

/**
 * @brief Whether a chapter name follows atlc's rule for them: a lowercase letter, then lowercase letters, digits and
 *   `-`, at most `MAX_TOPIC`; anything else is refused before atlc runs, so atlc never reads it as an option.
 * @param topic The name.
 * @return Whether it may be passed.
 */
inline bool valid_topic(const std::string& topic) {
  if (topic.empty() || topic.size() > MAX_TOPIC || topic.front() < 'a' || topic.front() > 'z') {
    return false;
  }
  return std::all_of(topic.begin(), topic.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

/**
 * @brief `strategy reference [TOPIC]`: `atlc reference [TOPIC]`, its text unchanged, the language reference of the
 *   atlc cmdatl runs, never a copy; but `run-spec`, the run settings, which are cmdatl's own, from cmdatl.
 * @param invocation The arguments.
 * @return The reply, atlc's text as `text` (printed bare in text format), or atlc's refusal.
 */
inline Reply reference_reply(const Invocation& invocation) {
#if defined(_WIN32)
  (void)invocation;
  return error_reply(
    Exit::FAILED, "unavailable.platform",
    "strategy reference runs atlc as a child process, which this version does on macOS and Linux only",
    "run atlc reference directly"
  );
#else
  const auto topic = invocation.has("TOPIC") ? invocation.get("TOPIC") : std::string();
  const std::string chapters = "cmdatl strategy reference lists the chapters";
  if (invocation.has("TOPIC") && !valid_topic(topic)) {
    return error_reply(
      Exit::USAGE, "usage.bad_value", "TOPIC is a chapter's name: lowercase letters, digits and -", chapters
    );
  }
  if (topic == "run-spec") {
    Reply reply;
    reply.text_ = RUN_SPEC_REFERENCE;
    return reply;
  }
  if (interrupted()) {
    return interrupted_reply();
  }
  const auto located = locate_atlc();
  if (located.refusal_) {
    return *located.refusal_;
  }
  std::vector<std::string> argv{located.atlc_.path_, "reference"};
  if (!topic.empty()) {
    argv.push_back(topic);
  }
  const auto ran = run_child(argv, [](const std::string&) {});
  if (ran.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply();
  }
  const std::string check = "check atlc with cmdatl env";
  if (ran.kind_ != ChildResult::Kind::FINISHED || ran.signaled_) {
    return atlc_failure(Exit::FAILED, "atlc.failed", "atlc reference: " + ending_of(ran), ran, check);
  }
  // An atlc without the command answers it as any unknown command: exit 2 and its usage.
  const bool usage = std::any_of(ran.stderr_tail_.begin(), ran.stderr_tail_.end(), [](const std::string& line) {
    return line.rfind("usage: atlc", 0) == 0;
  });
  switch (ran.code_) {
    case 0:
      break;
    case 1:
      return atlc_failure(Exit::USAGE, "usage.bad_value", last_line(ran), ran, chapters);
    case 2:
      if (usage) {
        return atlc_failure(
          Exit::FAILED, "atlc.unsupported", "this atlc has no atlc reference: it is older than the atlc cmdatl reads",
          ran, "install a newer atlc"
        );
      }
      return atlc_failure(Exit::FAILED, "atlc.failed", "atlc reference: " + ending_of(ran), ran, check);
    default:
      return atlc_failure(Exit::FAILED, "atlc.failed", "atlc reference: " + ending_of(ran), ran, check);
  }
  Reply reply;
  reply.text_ = ran.stdout_;
  return reply;
#endif
}

/** @brief The longest name `strategy new` takes. */
inline constexpr std::size_t MAX_NAME = 64;

/**
 * @brief The C++ struct atlc names after a file stem: split at `-` and `_`, each part capitalized
 *   (`mean-reversion` gives `MeanReversion`).
 * @param name A name made of ASCII letters, digits, `-` and `_`.
 * @return The struct's name.
 */
inline std::string camel_case(const std::string& name) {
  std::string out;
  bool part_starts = true;
  for (const char c : name) {
    if (c == '-' || c == '_') {
      part_starts = true;
      continue;
    }
    out.push_back(part_starts && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    part_starts = false;
  }
  return out;
}

/**
 * @brief Why a name cannot be a program's: ASCII letters, digits, `-` and `_`, starting with a letter, at most 64
 *   characters, and not making a struct atlc reserves (`Params`, `Lane`).
 * @param name The name.
 * @return The reason, or nothing when it can.
 */
inline std::optional<std::string> name_problem(const std::string& name) {
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };
  if (name.empty() || name.size() > MAX_NAME) {
    return "a name has 1 to " + std::to_string(MAX_NAME) + " characters";
  }
  if (!letter(name.front())) {
    return "a name starts with an ASCII letter";
  }
  const auto allowed = [&letter](char c) {
    return letter(c) || (c >= '0' && c <= '9') || c == '-' || c == '_';
  };
  if (!std::all_of(name.begin(), name.end(), allowed)) {
    return "a name holds ASCII letters, digits, - and _ only";
  }
  const auto struct_name = camel_case(name);
  if (struct_name == "Params" || struct_name == "Lane") {
    return name + " makes the C++ struct " + struct_name + ", a name atlc reserves";
  }
  return std::nullopt;
}

/**
 * @brief A word as a POSIX shell reads it back: as it is when it has no special character, else in single quotes.
 * @param word The word.
 * @return The quoted word.
 */
inline std::string shell_quoted(const std::string& word) {
  const auto plain = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
      std::strchr("_-./+,:@%=", c) != nullptr;
  };
  if (!word.empty() && std::all_of(word.begin(), word.end(), plain)) {
    return word;
  }
  std::string out = "'";
  for (const char c : word) {
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  }
  return out + "'";
}

/**
 * @brief The refusal of a destination that exists.
 * @param path The destination.
 * @param force Whether `--force` was given (then it is not a regular file).
 * @return The reply.
 */
inline Reply exists_reply(const std::filesystem::path& path, bool force) {
  if (force) {
    return error_reply(
      Exit::FAILED, "strategy.exists",
      path.string() + " is a directory, a link or a special file; --force replaces regular files only",
      "choose another NAME or --dir"
    );
  }
  return error_reply(
    Exit::FAILED, "strategy.exists", path.string() + " already exists; nothing was written",
    "choose another NAME or --dir, or add --force to replace the files"
  );
}

/**
 * @brief The refusal of a destination that cannot be written.
 * @param path The destination.
 * @param error The error number.
 * @return The reply.
 */
inline Reply unwritable_reply(const std::filesystem::path& path, int error) {
  return error_reply(Exit::FAILED, "file.unwritable", "cannot write " + path.string() + ": " + std::strerror(error));
}

#if !defined(_WIN32)
/**
 * @brief Opens a destination of `strategy new` for writing, without truncating it: created exclusively; with
 *   `force`, an existing regular file is opened instead (a link, a directory or a special file is refused without
 *   blocking, and the type is checked on the open descriptor).
 * @param path The destination.
 * @param force Whether an existing regular file may be replaced.
 * @param fd Where the descriptor goes.
 * @param created Set when this call created the file.
 * @param info The open file's status, read once from the descriptor; its device and inode tell two names for one
 *   file apart.
 * @return The refusal, or nothing when it is open.
 */
inline std::optional<Reply> open_destination(
  const std::filesystem::path& path, bool force, ScopedFd& fd, bool& created, struct stat& info
) {
  const int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
  fd.reset(::open(path.c_str(), flags | O_CREAT | O_EXCL, 0666));
  if (fd.get() >= 0) {
    created = true;
    if (::fstat(fd.get(), &info) != 0) {
      return unwritable_reply(path, errno);
    }
    return std::nullopt;
  }
  if (errno != EEXIST) {
    return unwritable_reply(path, errno);
  }
  if (!force) {
    return exists_reply(path, false);
  }
  fd.reset(::open(path.c_str(), flags));
  if (fd.get() < 0) {
    const int error = errno;
    // A final link (ELOOP; EMLINK on some systems), a directory, a FIFO without a reader or a socket.
    if (error == ELOOP || error == EMLINK || error == EISDIR || error == ENXIO || error == EOPNOTSUPP) {
      return exists_reply(path, true);
    }
    return unwritable_reply(path, error);
  }
  if (::fstat(fd.get(), &info) != 0) {
    return unwritable_reply(path, errno);
  }
  if (!S_ISREG(info.st_mode)) {
    return exists_reply(path, true);
  }
  return std::nullopt;
}

/**
 * @brief Writes a whole text to a descriptor.
 * @param fd The descriptor.
 * @param text The text.
 * @return 0, or the error number.
 */
inline int write_all(int fd, const std::string& text) noexcept {
  std::size_t done = 0;
  while (done < text.size()) {
    const auto n = ::write(fd, text.data() + done, text.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno;
    }
    done += static_cast<std::size_t>(n);
  }
  return 0;
}
#endif

/**
 * @brief Writes a program and its spec, both or neither: both destinations are opened and checked before either
 *   is written, and a file this call created is removed when the other cannot be.
 * @param program The program's path.
 * @param program_text The program.
 * @param spec The spec's path.
 * @param spec_text The spec.
 * @param force Whether existing regular files may be replaced.
 * @return The refusal, or nothing when both were written.
 */
inline std::optional<Reply> write_pair(
  const std::filesystem::path& program, const std::string& program_text, const std::filesystem::path& spec,
  const std::string& spec_text, bool force
) {
#if defined(_WIN32)
  // No exclusive creation here: the paths are checked, then written (Windows is not a supported platform yet).
  for (const auto* path : {&program, &spec}) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(*path, ec);
    if (std::filesystem::exists(status)) {
      if (!force || !std::filesystem::is_regular_file(status)) {
        return exists_reply(*path, force);
      }
    }
  }
  for (const auto& [path, text] : {std::make_pair(&program, &program_text), std::make_pair(&spec, &spec_text)}) {
    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
    out << *text;
    out.flush();
    if (!out) {
      return unwritable_reply(*path, errno);
    }
  }
  return std::nullopt;
#else
  ScopedFd program_fd;
  ScopedFd spec_fd;
  bool program_created = false;
  bool spec_created = false;
  const auto discard = [&]() noexcept {
    if (program_created) {
      ::unlink(program.c_str());
    }
    if (spec_created) {
      ::unlink(spec.c_str());
    }
  };
  struct stat program_info{};
  struct stat spec_info{};
  if (auto refused = open_destination(program, force, program_fd, program_created, program_info)) {
    discard();
    return refused;
  }
  if (auto refused = open_destination(spec, force, spec_fd, spec_created, spec_info)) {
    discard();
    return refused;
  }
  // Two names for one file (hard links): writing the spec would destroy the program just written. The statuses
  // are the ones each open read, so no separate lookup can fail and let the pair through.
  if (program_info.st_dev == spec_info.st_dev && program_info.st_ino == spec_info.st_ino) {
    discard();
    return error_reply(
      Exit::FAILED, "strategy.exists", program.string() + " and " + spec.string() + " are one file",
      "remove one of the two names, or choose another NAME"
    );
  }
  const auto write = [](const ScopedFd& fd, bool created, const std::string& text) -> int {
    if (!created && ::ftruncate(fd.get(), 0) != 0) {
      return errno;
    }
    return write_all(fd.get(), text);
  };
  if (const int error = write(program_fd, program_created, program_text)) {
    discard();
    return unwritable_reply(program, error);
  }
  if (const int error = write(spec_fd, spec_created, spec_text)) {
    discard();
    return unwritable_reply(spec, error);
  }
  return std::nullopt;
#endif
}

/**
 * @brief `strategy new NAME [--template T] [--dir DIR] [--force]`: a starter program and a run spec beside it.
 * @param invocation The arguments.
 * @return The reply.
 */
inline Reply new_reply(const Invocation& invocation) {
  const auto name = invocation.get("NAME");
  if (const auto problem = name_problem(name)) {
    return error_reply(Exit::USAGE, "strategy.name", *problem, "a letter, then letters, digits, - or _ (at most 64)");
  }
  const auto chosen = invocation.get("template", TEMPLATES[0].name_);
  const TemplateFile* starter = nullptr;
  std::string names;
  for (const auto& entry : TEMPLATES) {
    names += (names.empty() ? "" : ", ") + std::string(entry.name_);
    if (chosen == entry.name_) {
      starter = &entry;
    }
  }
  if (starter == nullptr) {
    return error_reply(Exit::USAGE, "strategy.template", "no template " + chosen, "the templates are: " + names);
  }
  std::error_code ec;
  const auto given = std::filesystem::absolute(invocation.get("dir", "."));
  auto dir = std::filesystem::weakly_canonical(given, ec);
  if (ec) {
    dir = given;
  }
  const auto program = dir / (name + ".atls");
  const auto spec = dir / (name + ".toml");
  std::string spec_text = SPEC_TEMPLATE;
  const std::string placeholder = "@NAME@";
  for (auto at = spec_text.find(placeholder); at != std::string::npos; at = spec_text.find(placeholder, at)) {
    spec_text.replace(at, placeholder.size(), name);
    at += name.size();
  }
  if (interrupted()) {
    return interrupted_reply();
  }
  if (auto refused = write_pair(program, starter->text_, spec, spec_text, invocation.has("force"))) {
    return std::move(*refused);
  }
  // The paths as typed from the working directory.
  std::error_code cwd_error;
  const auto cwd = std::filesystem::current_path(cwd_error);
  const auto typed = [&](const std::filesystem::path& path) {
    const auto relative = cwd_error ? std::filesystem::path() : path.lexically_relative(cwd);
    auto text = (relative.empty() ? path : relative).string();
    // A relative path starting with `-` would read as an option.
    if (!text.empty() && text.front() == '-') {
      text = "./" + text;
    }
    return shell_quoted(text);
  };
  Reply reply;
  reply.body_["program"] = program.string();
  reply.body_["spec"] = spec.string();
  reply.body_["next"] = json::array{
    json::value("cmdatl strategy backtest " + typed(spec)),
    json::value("atlc check " + typed(program)),
  };
  return reply;
}

} // namespace cmdatl
