#pragma once

#include "./reply.hpp"

#include <algotradelib/version>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef CMDATL_VERSION
#define CMDATL_VERSION "0.0.0.9000"
#endif

namespace cmdatl {

/** @brief The oldest atlc whose generated code and run spec cmdatl reads. */
inline constexpr const char* MINIMUM_ATLC = "0.0.0";

/** @brief Where cmdatl found atlc, and its version. */
struct Atlc {
  /** @brief What `CMDATL_ATLC` named, as written, or empty. */
  std::string requested_;
  /** @brief The executable, or empty when none was found. */
  std::string path_;
  /** @brief Where the path came from: `CMDATL_ATLC`, `PATH`, or empty. */
  std::string source_;
  /** @brief What `atlc --version` printed after the name, or empty. */
  std::string version_;
};

/**
 * @brief An environment variable's value, or nothing when it is unset or empty.
 * @param name The variable.
 * @return The value.
 */
inline std::optional<std::string> variable(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

/**
 * @brief The first executable file of this name on `PATH`.
 * @param name The program's name.
 * @return Its path, or nothing.
 */
inline std::optional<std::string> find_on_path(const std::string& name) {
  const auto path = variable("PATH");
  if (!path) {
    return std::nullopt;
  }
#if defined(_WIN32)
  const char separator = ';';
  const std::vector<std::string> names{name + ".exe", name};
#else
  const char separator = ':';
  const std::vector<std::string> names{name};
#endif
  std::string dir;
  for (std::size_t start = 0; start <= path->size();) {
    const auto end = path->find(separator, start);
    dir = path->substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (const auto& file : names) {
      const auto candidate = std::filesystem::path(dir.empty() ? "." : dir) / file;
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec)) {
#if defined(_WIN32)
        return candidate.string();
#else
        if (::access(candidate.c_str(), X_OK) == 0) {
          return candidate.string();
        }
#endif
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return std::nullopt;
}

/**
 * @brief Runs a program with arguments, no shell, and returns what it printed on stdout.
 *
 * Only for short probes such as `atlc --version`: there is no time limit, and a failure to run and a failing exit
 * read alike. Never use it for a command whose outcome must be known (a send).
 *
 * @param argv The program (a path) and its arguments.
 * @return The output, or nothing when it could not run or exited with an error.
 */
inline std::optional<std::string> capture(const std::vector<std::string>& argv) {
#if defined(_WIN32)
  (void)argv;
  return std::nullopt;
#else
  int raw[2];
  if (::pipe(raw) != 0) {
    return std::nullopt;
  }
  // Move both ends above the standard descriptors, so the child's dup2 onto stdout cannot close its own pipe end.
  const int read_fd = ::fcntl(raw[0], F_DUPFD_CLOEXEC, 3);
  const int write_fd = ::fcntl(raw[1], F_DUPFD_CLOEXEC, 3);
  ::close(raw[0]);
  ::close(raw[1]);
  if (read_fd < 0 || write_fd < 0) {
    if (read_fd >= 0) {
      ::close(read_fd);
    }
    if (write_fd >= 0) {
      ::close(write_fd);
    }
    return std::nullopt;
  }
  posix_spawn_file_actions_t actions{};
  const bool initialized = posix_spawn_file_actions_init(&actions) == 0;
  const bool ready = initialized && posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO) == 0;
  std::vector<char*> args;
  for (const auto& arg : argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);
  pid_t pid = 0;
  const int spawned = ready ? posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ) : -1;
  if (initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  ::close(write_fd);
  std::string out;
  if (spawned == 0) {
    char buffer[4096];
    while (true) {
      const auto n = ::read(read_fd, buffer, sizeof(buffer));
      if (n > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
  }
  ::close(read_fd);
  if (spawned != 0) {
    return std::nullopt;
  }
  int status = 0;
  pid_t waited = 0;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  return out;
#endif
}

/**
 * @brief Finds atlc: `CMDATL_ATLC`, else `atlc` on `PATH`; then asks it for its version.
 * @return What was found.
 */
inline Atlc find_atlc() {
  Atlc atlc;
  if (const auto set = variable("CMDATL_ATLC")) {
    // A path is used as given; a bare name is looked up on PATH only, never in the current directory. An override
    // that resolves to nothing stays unresolved: it is reported, never replaced by another atlc.
    atlc.requested_ = *set;
    atlc.source_ = "CMDATL_ATLC";
    const bool bare_name = set->find('/') == std::string::npos && set->find('\\') == std::string::npos;
    if (!bare_name) {
      atlc.path_ = *set;
    } else if (const auto found = find_on_path(*set)) {
      atlc.path_ = *found;
    } else {
      return atlc;
    }
  } else if (const auto found = find_on_path("atlc")) {
    atlc.path_ = *found;
    atlc.source_ = "PATH";
  } else {
    return atlc;
  }
  if (const auto printed = capture({atlc.path_, "--version"})) {
    // `atlc 0.0.0-9000`
    const auto line = printed->substr(0, printed->find('\n'));
    const auto space = line.find(' ');
    atlc.version_ = space == std::string::npos ? line : line.substr(space + 1);
  }
  return atlc;
}

/**
 * @brief The release part of a version (`0.1.2` of `0.1.2-9000`) as three numbers.
 * @param version The version.
 * @return Major, minor and patch, or nothing when it does not start with three numbers that fit.
 */
inline std::optional<std::vector<int>> release_of(const std::string& version) {
  const auto release = version.substr(0, version.find('-'));
  std::vector<int> parts;
  std::size_t start = 0;
  while (parts.size() < 3) {
    const auto dot = release.find('.', start);
    const auto text = release.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || value < 0) {
      return std::nullopt;
    }
    parts.push_back(value);
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  if (parts.size() != 3) {
    return std::nullopt;
  }
  return parts;
}

/**
 * @brief Whether a version is at least another, comparing releases only.
 * @param version The version found.
 * @param minimum The oldest accepted.
 * @return Whether it is accepted; false when either does not parse.
 */
inline bool at_least(const std::string& version, const std::string& minimum) {
  const auto found = release_of(version);
  const auto wanted = release_of(minimum);
  return found && wanted && *found >= *wanted;
}

/** @brief Where the operator's file is: named by `CMDATL_CONFIG` only, never by an argument. */
struct OperatorFile {
  /** @brief The path, or empty when `CMDATL_CONFIG` is unset. */
  std::string path_;
  /** @brief Whether the file exists. */
  bool exists_{false};
};

/**
 * @brief The operator's file's location. Its contents are read by the commands that need them.
 * @return Where it is.
 */
inline OperatorFile find_operator_file() {
  OperatorFile file;
  if (const auto set = variable("CMDATL_CONFIG")) {
    file.path_ = *set;
    std::error_code ec;
    file.exists_ = std::filesystem::is_regular_file(*set, ec);
  }
  return file;
}

/**
 * @brief `cmdatl env`: what cmdatl found, without secrets or the process environment.
 * @return The reply.
 */
inline Reply env_reply() {
  Reply reply;
  reply.body_["cmdatl"] = CMDATL_VERSION;
  reply.body_["algotradelib"] = ATL_VERSION_STRING;
  const auto atlc = find_atlc();
  const auto or_null = [](const std::string& text) { return text.empty() ? json::value(nullptr) : json::value(text); };
  json::object found;
  found["requested"] = or_null(atlc.requested_);
  found["path"] = or_null(atlc.path_);
  found["source"] = or_null(atlc.source_);
  found["version"] = or_null(atlc.version_);
  found["minimum"] = MINIMUM_ATLC;
  found["ok"] = !atlc.version_.empty() && at_least(atlc.version_, MINIMUM_ATLC);
  reply.body_["atlc"] = std::move(found);
  const auto file = find_operator_file();
  json::object config;
  config["path"] = or_null(file.path_);
  config["exists"] = file.exists_;
  reply.body_["config"] = std::move(config);
  reply.body_["modes"] = json::array{};
  return reply;
}

}  // namespace cmdatl
