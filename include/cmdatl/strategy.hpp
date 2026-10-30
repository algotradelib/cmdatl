#pragma once

#include "./environment.hpp"
#include "./table.hpp"

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
 * @brief The reply after SIGINT or SIGTERM: atlc's group was stopped and nothing more was run.
 * @return The reply.
 */
inline Reply interrupted_reply() {
  return error_reply(
    Exit::INTERRUPTED,
    "interrupted",
    "interrupted by a signal: atlc was stopped and nothing more was run",
    "run the command again",
    true
  );
}

/**
 * @brief How a child ended, for a person.
 * @param result The child's end.
 * @return The text, such as `atlc exited with 101`.
 */
inline std::string ending_of(const ChildResult& result) {
  switch (result.kind_) {
    case ChildResult::Kind::NOT_STARTED:
      return "atlc could not be started: " + result.error_;
    case ChildResult::Kind::IO_FAILED:
      return "atlc's output was lost: " + result.error_;
    case ChildResult::Kind::INTERRUPTED:
      return "atlc was interrupted";
    case ChildResult::Kind::FINISHED:
      break;
  }
  if (result.signaled_) {
#if defined(_WIN32)
    const char* name = nullptr;
#else
    const char* name = ::strsignal(result.code_);
#endif
    return "atlc was ended by signal " + std::to_string(result.code_) + (name == nullptr ? "" : std::string(" (") + name + ")");
  }
  return "atlc exited with " + std::to_string(result.code_);
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
  Exit exit,
  const std::string& code,
  const std::string& message,
  const ChildResult& result,
  const std::string& next
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
 * @brief Whether a value is one of atlc's diagnostics as far as cmdatl reads it: an object with a `severity` of
 *   `error` or `warning`, and a `code` and a `message` that are strings.
 * @param value The value.
 * @return Whether it is.
 */
inline bool is_diagnostic(const json::value& value) {
  const auto* object = value.if_object();
  if (object == nullptr) {
    return false;
  }
  for (const auto* key : {"severity", "code", "message"}) {
    const auto* field = object->if_contains(key);
    if (field == nullptr || !field->is_string()) {
      return false;
    }
  }
  const auto& severity = object->at("severity").as_string();
  return severity == "error" || severity == "warning";
}

/**
 * @brief `atlc spec --json`'s reply, checked for what cmdatl reads: `atlc_spec` 1, `warnings` and `errors` arrays of
 *   diagnostics, and `status` `ok` exactly when `errors` is empty. The program, build and order fields are not read.
 * @param text atlc's stdout.
 * @return The reply, or nothing when it is not one.
 */
inline std::optional<json::object> spec_envelope(const std::string& text) {
  auto object = one_object(text);
  if (!object) {
    return std::nullopt;
  }
  const auto* format = object->if_contains("atlc_spec");
  if (format == nullptr || !format->is_int64() || format->as_int64() != 1) {
    return std::nullopt;
  }
  for (const auto* key : {"warnings", "errors"}) {
    const auto* list = object->if_contains(key);
    if (list == nullptr || !list->is_array()) {
      return std::nullopt;
    }
    const auto& items = list->as_array();
    if (!std::all_of(items.begin(), items.end(), is_diagnostic)) {
      return std::nullopt;
    }
  }
  const auto* status = object->if_contains("status");
  if (status == nullptr || !status->is_string()) {
    return std::nullopt;
  }
  const bool ok = status->as_string() == "ok";
  if ((!ok && status->as_string() != "error") || ok != object->at("errors").as_array().empty()) {
    return std::nullopt;
  }
  return object;
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

/**
 * @brief What `atlc spec SPEC --json` answered, when it does not let the backtest go on.
 * @param result The child's end.
 * @return The failure reply, or nothing when the spec is valid.
 */
inline std::optional<Reply> spec_failure(const ChildResult& result) {
  if (result.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply();
  }
  if (result.kind_ != ChildResult::Kind::FINISHED) {
    return atlc_failure(Exit::FAILED, "atlc.failed", ending_of(result), result, "check atlc with cmdatl env");
  }
  const auto envelope = result.signaled_ ? std::nullopt : spec_envelope(result.stdout_);
  if (envelope) {
    const auto& errors = envelope->at("errors").as_array();
    const bool ok = errors.empty();
    if (result.code_ == 0 && ok) {
      return std::nullopt;
    }
    if ((result.code_ == 1 || result.code_ == 2) && !ok) {
      const bool program = result.code_ == 1;
      auto reply = error_reply(
        program ? Exit::PROGRAM : Exit::USAGE,
        program ? "program.invalid" : "spec.invalid",
        first_error(errors, program ? "the program has errors" : "the run spec has errors"),
        program ? "fix the program; atlc check FILE.atls checks it alone" : "fix the run spec"
      );
      reply.body_["errors"] = errors;
      return reply;
    }
  }
  const bool usage = std::any_of(result.stderr_tail_.begin(), result.stderr_tail_.end(), [](const std::string& line) {
    return line.rfind("usage: atlc", 0) == 0;
  });
  if (!result.signaled_ && result.code_ == 2 && result.stdout_.find('{') == std::string::npos && usage) {
    return atlc_failure(
      Exit::FAILED,
      "atlc.unsupported",
      "this atlc has no atlc spec: it is older than the atlc cmdatl reads",
      result,
      "install a newer atlc"
    );
  }
  const auto why = envelope ? ending_of(result) + " with a contradictory reply" :
    ending_of(result) + " without a valid atlc spec --json reply";
  return atlc_failure(Exit::FAILED, "atlc.failed", "atlc spec: " + why, result, "check atlc with cmdatl env");
}

/**
 * @brief `strategy backtest SPEC [-P k=v]... [--fills FILE]`: `atlc spec` checks the spec without building, then
 *   `atlc run` replays it; the reply carries atlc's report unchanged.
 * @param invocation The arguments.
 * @param output Where progress goes: cmdatl's first line, then atlc's stderr lines as they arrive.
 * @return The reply.
 */
inline Reply backtest_reply(const Invocation& invocation, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)output;
  return error_reply(
    Exit::FAILED,
    "unavailable.platform",
    "strategy backtest runs atlc as a child process, which this version does on macOS and Linux only",
    "run atlc run SPEC directly"
  );
#else
  const auto spec_given = invocation.get("SPEC");
  const auto found = invocation.values_.find("param");
  const auto params = found == invocation.values_.end() ? std::vector<std::string>{} : found->second;
  // Before atlc is looked for: finding it runs `atlc --version`, which may take a while.
  output.progress("cmdatl: replaying " + spec_given);
  if (interrupted()) {
    return interrupted_reply();
  }
  const auto atlc = find_atlc();
  if (interrupted()) {
    return interrupted_reply();
  }
  const std::string install = "install atlc, or name it with CMDATL_ATLC; cmdatl env shows what was found";
  if (atlc.path_.empty()) {
    const auto where = atlc.requested_.empty() ? std::string("atlc was not found on PATH") :
      "CMDATL_ATLC names " + atlc.requested_ + ", which was not found on PATH";
    return error_reply(Exit::FAILED, "atlc.missing", where, install);
  }
  if (atlc.version_.empty()) {
    return error_reply(Exit::FAILED, "atlc.missing", atlc.path_ + " did not answer --version", install);
  }
  if (!at_least(atlc.version_, MINIMUM_ATLC)) {
    return error_reply(
      Exit::FAILED,
      "atlc.unsupported",
      "atlc " + atlc.version_ + " is not a version cmdatl reads (at least " + MINIMUM_ATLC + ")",
      "install a newer atlc"
    );
  }
  // Absolute, so atlc never reads the path as an option and the reply names the file the report came from.
  const auto spec = std::filesystem::absolute(spec_given).string();
  const auto forward = [&output](const std::string& line) { output.progress(line); };
  std::vector<std::string> argv{atlc.path_, "spec", spec};
  for (const auto& param : params) {
    argv.push_back("-P");
    argv.push_back(param);
  }
  argv.push_back("--json");
  if (auto failure = spec_failure(run_child(argv, forward))) {
    return std::move(*failure);
  }
  if (interrupted()) {
    return interrupted_reply();
  }
  argv = {atlc.path_, "run", spec};
  for (const auto& param : params) {
    argv.push_back("-P");
    argv.push_back(param);
  }
  std::string fills;
  if (invocation.has("fills")) {
    fills = std::filesystem::absolute(invocation.get("fills")).string();
    argv.push_back("--fills");
    argv.push_back(fills);
  }
  const auto ran = run_child(argv, forward);
  if (ran.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply();
  }
  const std::string check = "check atlc with cmdatl env";
  if (ran.kind_ != ChildResult::Kind::FINISHED || ran.signaled_) {
    return atlc_failure(Exit::FAILED, "atlc.failed", "atlc run: " + ending_of(ran), ran, check);
  }
  switch (ran.code_) {
    case 0:
      break;
    case 1:
      return atlc_failure(Exit::PROGRAM, "program.invalid", stderr_text(ran, "the program has errors"), ran, "fix the program");
    case 2:
      return atlc_failure(Exit::USAGE, "spec.invalid", stderr_text(ran, "the run spec has errors"), ran, "fix the run spec");
    case 3:
      return atlc_failure(
        Exit::FAILED,
        "build.failed",
        "atlc could not build the strategy: " + last_line(ran),
        ran,
        "read atlc's build log named in stderr"
      );
    case 4:
      return atlc_failure(
        Exit::FAILED,
        "replay.failed",
        "the replay failed: " + last_line(ran),
        ran,
        "check the data and the parameters' bounds"
      );
    default:
      return atlc_failure(Exit::FAILED, "atlc.failed", "atlc run: " + ending_of(ran), ran, check);
  }
  auto report = one_object(ran.stdout_);
  if (!report) {
    return atlc_failure(Exit::FAILED, "atlc.failed", "atlc run exited 0 without exactly one JSON report", ran, check);
  }
  Reply reply;
  reply.body_["report"] = std::move(*report);
  reply.body_["spec"] = spec;
  json::array given;
  for (const auto& param : params) {
    given.push_back(json::value(param));
  }
  reply.body_["params_given"] = std::move(given);
  reply.body_["atlc"] = atlc.version_;
  if (!fills.empty()) {
    reply.body_["fills"] = fills;
  }
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
  const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  if (name.empty() || name.size() > MAX_NAME) {
    return "a name has 1 to " + std::to_string(MAX_NAME) + " characters";
  }
  if (!letter(name.front())) {
    return "a name starts with an ASCII letter";
  }
  const auto allowed = [&letter](char c) { return letter(c) || (c >= '0' && c <= '9') || c == '-' || c == '_'; };
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
      Exit::FAILED,
      "strategy.exists",
      path.string() + " is a directory, a link or a special file; --force replaces regular files only",
      "choose another NAME or --dir"
    );
  }
  return error_reply(
    Exit::FAILED,
    "strategy.exists",
    path.string() + " already exists; nothing was written",
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
 * @return The refusal, or nothing when it is open.
 */
inline std::optional<Reply> open_destination(
  const std::filesystem::path& path,
  bool force,
  ScopedFd& fd,
  bool& created
) {
  const int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
  fd.reset(::open(path.c_str(), flags | O_CREAT | O_EXCL, 0666));
  if (fd.get() >= 0) {
    created = true;
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
  struct stat info{};
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
  const std::filesystem::path& program,
  const std::string& program_text,
  const std::filesystem::path& spec,
  const std::string& spec_text,
  bool force
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
  if (auto refused = open_destination(program, force, program_fd, program_created)) {
    return refused;
  }
  if (auto refused = open_destination(spec, force, spec_fd, spec_created)) {
    discard();
    return refused;
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
    return shell_quoted((relative.empty() ? path : relative).string());
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

}  // namespace cmdatl
