#pragma once

#include "./reply.hpp"

#include <algotradelib/version>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

#if !defined(_WIN32)
/** @brief A file descriptor, closed when its owner goes, on every path, an exception's included. */
class ScopedFd {
public:
  /**
   * @brief Takes a descriptor.
   * @param fd The descriptor, or -1 for none.
   */
  explicit ScopedFd(int fd = -1) noexcept
  : fd_(fd) {}

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  /** @brief Closes the descriptor, if any. */
  ~ScopedFd() { reset(); }

  /**
   * @brief The descriptor.
   * @return It, or -1.
   */
  int get() const noexcept { return fd_; }

  /** @brief Closes the descriptor now. */
  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  /**
   * @brief Closes the descriptor, if any, and takes another.
   * @param fd The descriptor, or -1 for none.
   */
  void reset(int fd) noexcept {
    reset();
    fd_ = fd;
  }

private:
  int fd_;
};

/** @brief `posix_spawn_file_actions_t`, destroyed when its owner goes if it was initialized. */
class ScopedSpawnActions {
public:
  /** @brief Initializes the actions; `ok()` says whether that worked. */
  ScopedSpawnActions() noexcept
  : ok_(posix_spawn_file_actions_init(&actions_) == 0) {}

  ScopedSpawnActions(const ScopedSpawnActions&) = delete;
  ScopedSpawnActions& operator=(const ScopedSpawnActions&) = delete;

  /** @brief Destroys the actions, only if they were initialized. */
  ~ScopedSpawnActions() {
    if (ok_) {
      posix_spawn_file_actions_destroy(&actions_);
    }
  }

  /**
   * @brief Whether the actions were initialized.
   * @return Whether they were.
   */
  bool ok() const noexcept { return ok_; }

  /**
   * @brief The actions.
   * @return A pointer to them.
   */
  posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
  posix_spawn_file_actions_t actions_{};
  bool ok_;
};

/** @brief `posix_spawnattr_t`, destroyed when its owner goes if it was initialized. */
class ScopedSpawnAttr {
public:
  /** @brief Initializes the attributes; `status()` is the error number of that, 0 when it worked. */
  ScopedSpawnAttr() noexcept
  : status_(posix_spawnattr_init(&attr_)) {}

  ScopedSpawnAttr(const ScopedSpawnAttr&) = delete;
  ScopedSpawnAttr& operator=(const ScopedSpawnAttr&) = delete;

  /** @brief Destroys the attributes, only if they were initialized. */
  ~ScopedSpawnAttr() {
    if (status_ == 0) {
      posix_spawnattr_destroy(&attr_);
    }
  }

  /**
   * @brief The error number of the initialization.
   * @return 0 when it worked.
   */
  int status() const noexcept { return status_; }

  /**
   * @brief The attributes.
   * @return A pointer to them.
   */
  posix_spawnattr_t* get() noexcept { return &attr_; }

private:
  posix_spawnattr_t attr_{};
  int status_;
};

/**
 * @brief A child that leads its own process group, owned from the moment it is spawned: when its owner goes (an
 *   exception, an early return), the whole group is killed and the leader reaped.
 *
 * The leader is never reaped while the group may still be signalled: until then its process id, which is the
 * group's id, cannot be reused, so a signal to the group never reaches another process.
 */
class ChildGroup {
public:
  /**
   * @brief Takes a spawned child that leads its own process group.
   * @param pid The child's process id, which is its group's id.
   */
  explicit ChildGroup(pid_t pid) noexcept
  : pid_(pid) {}

  ChildGroup(const ChildGroup&) = delete;
  ChildGroup& operator=(const ChildGroup&) = delete;

  /** @brief Kills what is left of the group and reaps the leader, if that was not done. */
  ~ChildGroup() {
    int status = 0;
    (void)finish(status);
  }

  /**
   * @brief Sends a signal to every process of the group, while it is owned.
   * @param sig The signal.
   */
  void signal(int sig) noexcept {
    if (pid_ > 0) {
      ::killpg(pid_, sig);
    }
  }

  /**
   * @brief Whether the leader has ended, without reaping it, so the group's id stays reserved.
   * @return 1 when it has ended, 0 when it runs, -1 when it is no longer this process's child (the group is then
   *   given up unsignalled, since its id may already name another process).
   */
  int ended() noexcept {
    if (pid_ <= 0) {
      return -1;
    }
    siginfo_t info;
    std::memset(&info, 0, sizeof(info));
    int waited = 0;
    do {
      waited = ::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
      pid_ = -1;
      return -1;
    }
    return info.si_pid != 0 ? 1 : 0;
  }

  /**
   * @brief Kills what is left of the group, then reaps the leader (at once, since it was killed if it still ran).
   *
   * Ownership ends here: the leader was reaped, or it is no longer this process's child (`ECHILD`); either way its
   * id is no longer this owner's to signal, and the group was killed before.
   *
   * @param status Where the leader's wait status goes.
   * @return Whether the leader was reaped; false when it was not owned or not this process's child.
   */
  bool finish(int& status) noexcept {
    if (pid_ <= 0) {
      return false;
    }
    ::killpg(pid_, SIGKILL);
    pid_t waited = 0;
    do {
      waited = ::waitpid(pid_, &status, 0);
    } while (waited < 0 && errno == EINTR);
    pid_ = -1;
    return waited > 0;
  }

private:
  pid_t pid_;
};
#endif

/** @brief Set to 1 by SIGINT or SIGTERM, once `handle_signals` has installed the handlers. */
inline volatile std::sig_atomic_t interruption = 0;

/**
 * @brief The handler of SIGINT and SIGTERM: it only notes the signal; the loop waiting on a child reads the note.
 * @param sig The signal.
 */
inline void note_interruption(int sig) {
  (void)sig;
  interruption = 1;
}

/**
 * @brief Whether SIGINT or SIGTERM has arrived.
 * @return Whether one has.
 */
inline bool interrupted() noexcept {
  return interruption != 0;
}

/**
 * @brief cmdatl's signal dispositions, set once at start: SIGINT and SIGTERM are noted, never fatal, so a command
 *   stops its child and still replies; SIGPIPE is ignored, so a write to a closed output fails through its stream;
 *   SIGCHLD is the default, so children are not reaped behind cmdatl's back.
 */
inline void handle_signals() noexcept {
#if !defined(_WIN32)
  struct sigaction noted{};
  noted.sa_handler = note_interruption;
  sigemptyset(&noted.sa_mask);
  ::sigaction(SIGINT, &noted, nullptr);
  ::sigaction(SIGTERM, &noted, nullptr);
  struct sigaction ignored{};
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  ::sigaction(SIGPIPE, &ignored, nullptr);
  struct sigaction standard{};
  standard.sa_handler = SIG_DFL;
  sigemptyset(&standard.sa_mask);
  ::sigaction(SIGCHLD, &standard, nullptr);
#endif
}

/** @brief How a child run by `run_child` ended. */
struct ChildResult {
  /** @brief What happened. */
  enum class Kind {
    NOT_STARTED,
    IO_FAILED,
    INTERRUPTED,
    FINISHED,
  };
  /** @brief What happened. */
  Kind kind_{Kind::NOT_STARTED};
  /** @brief When it did not start or its output was lost: the system's message. */
  std::string error_;
  /** @brief When it finished: whether a signal ended it, `code_` then being the signal. */
  bool signaled_{false};
  /** @brief When it finished: its exit code, or the signal that ended it. */
  int code_{0};
  /** @brief Everything it wrote on stdout. */
  std::string stdout_;
  /** @brief The last lines it wrote on stderr, oldest first. */
  std::deque<std::string> stderr_tail_;
};

/** @brief The most stderr lines a `ChildResult` keeps. */
inline constexpr std::size_t STDERR_TAIL_LINES = 20;
/** @brief The most stderr bytes a `ChildResult` keeps. */
inline constexpr std::size_t STDERR_TAIL_BYTES = 8192;
/** @brief The longest stderr line handed on whole; a longer one is handed on in pieces of this size. */
inline constexpr std::size_t STDERR_LINE_BYTES = 8192;
/** @brief How long the loop waiting on a child sleeps at most before it looks at the interruption note again. */
inline constexpr int CHILD_POLL_MS = 100;
/** @brief How long a child's group has, after SIGTERM, or after its leader ended, before it is killed. */
inline constexpr std::chrono::milliseconds CHILD_GRACE{2000};
/** @brief How long the output of a killed group is still read before cmdatl stops waiting for its end. */
inline constexpr std::chrono::milliseconds CHILD_KILLED_WAIT{1000};

#if !defined(_WIN32)
/**
 * @brief A pipe whose ends are above the standard descriptors and close on exec, so a child's `dup2` onto stdout
 *   or stderr can never close its own end, and no other child inherits it.
 * @param fds Where the read and write ends go.
 * @return 0, or the error number.
 */
inline int lifted_pipe(int (&fds)[2]) noexcept {
  int raw[2];
  if (::pipe(raw) != 0) {
    return errno;
  }
  // The originals close on every path; an open copy of a write end would keep a read from ever ending.
  ScopedFd raw_read(raw[0]);
  ScopedFd raw_write(raw[1]);
  const int read_end = ::fcntl(raw_read.get(), F_DUPFD_CLOEXEC, 3);
  if (read_end < 0) {
    return errno;
  }
  const int write_end = ::fcntl(raw_write.get(), F_DUPFD_CLOEXEC, 3);
  if (write_end < 0) {
    const int error = errno;
    ::close(read_end);
    return error;
  }
  fds[0] = read_end;
  fds[1] = write_end;
  return 0;
}
#endif

/**
 * @brief Runs a program with arguments, no shell, in its own process group, reading its stdout whole and handing
 *   each stderr line on as it arrives.
 *
 * For offline work only (atlc's replies): a command that may send never maps a child's end to an outcome through
 * it. stdin is `/dev/null`; the child starts with the default dispositions of SIGPIPE, SIGINT and SIGTERM and no
 * signal blocked. One loop reads both pipes (whichever is ready, so neither fills), waits at most 100 ms between
 * looks at the interruption note, and checks the leader without blocking. On an interruption the group gets
 * SIGTERM, and SIGKILL two seconds later on the monotonic clock if it has not ended; a group whose leader ended
 * while another process holds its pipes gets the same two seconds. Every descriptor and the group are owned, so an
 * exception (an allocation, the callback) kills the group, reaps the leader and leaks nothing.
 *
 * @param argv The program (a path) and its arguments.
 * @param on_stderr_line Called with each stderr line, without its newline, and with an unterminated last one; what
 *   it throws ends the run.
 * @return How it ended; partial output after a failure is never `FINISHED`.
 */
inline ChildResult run_child(
  const std::vector<std::string>& argv,
  const std::function<void(const std::string&)>& on_stderr_line
) {
  ChildResult result;
#if defined(_WIN32)
  (void)argv;
  (void)on_stderr_line;
  result.error_ = "running a program is not supported on this platform";
  return result;
#else
  if (argv.empty()) {
    result.error_ = "no program to run";
    return result;
  }
  // Everything that allocates before any resource is taken.
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);
  std::string line;
  line.reserve(256);
  int out_fds[2];
  if (const int error = lifted_pipe(out_fds)) {
    result.error_ = std::strerror(error);
    return result;
  }
  ScopedFd out_read(out_fds[0]);
  ScopedFd out_write(out_fds[1]);
  int err_fds[2];
  if (const int error = lifted_pipe(err_fds)) {
    result.error_ = std::strerror(error);
    return result;
  }
  ScopedFd err_read(err_fds[0]);
  ScopedFd err_write(err_fds[1]);
  pid_t pid = 0;
  {
    ScopedSpawnActions actions;
    ScopedSpawnAttr attr;
    sigset_t defaults;
    sigset_t unblocked;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigemptyset(&unblocked);
    // Each of these returns an error number, not errno; the flags are what make the values below take effect.
    int error = actions.ok() ? attr.status() : ENOMEM;
    const short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    if (error == 0) {
      error = posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    if (error == 0) {
      error = posix_spawn_file_actions_adddup2(actions.get(), out_write.get(), STDOUT_FILENO);
    }
    if (error == 0) {
      error = posix_spawn_file_actions_adddup2(actions.get(), err_write.get(), STDERR_FILENO);
    }
    if (error == 0) {
      error = posix_spawnattr_setflags(attr.get(), flags);
    }
    if (error == 0) {
      error = posix_spawnattr_setpgroup(attr.get(), 0);
    }
    if (error == 0) {
      error = posix_spawnattr_setsigdefault(attr.get(), &defaults);
    }
    if (error == 0) {
      error = posix_spawnattr_setsigmask(attr.get(), &unblocked);
    }
    if (error == 0) {
      error = posix_spawn(&pid, args[0], actions.get(), attr.get(), args.data(), environ);
    }
    if (error != 0) {
      result.error_ = std::strerror(error);
      return result;
    }
  }
  ChildGroup group(pid);
  out_write.reset();
  err_write.reset();

  std::size_t tail_bytes = 0;
  const auto deliver = [&](const std::string& text) {
    result.stderr_tail_.push_back(text);
    tail_bytes += text.size();
    while (result.stderr_tail_.size() > STDERR_TAIL_LINES || tail_bytes > STDERR_TAIL_BYTES) {
      tail_bytes -= result.stderr_tail_.front().size();
      result.stderr_tail_.pop_front();
    }
    on_stderr_line(text);
  };
  const auto lost = [&](const char* what, int error) {
    result.kind_ = ChildResult::Kind::IO_FAILED;
    result.error_ = std::string(what) + ": " + std::strerror(error);
  };
  using Clock = std::chrono::steady_clock;
  std::optional<Clock::time_point> kill_at;
  std::optional<Clock::time_point> give_up_at;
  bool terminated = false;
  bool leader_ended = false;
  int idle_ms = 1;
  char buffer[16384];
  while (true) {
    const auto now = Clock::now();
    if (!terminated && interrupted()) {
      group.signal(SIGTERM);
      terminated = true;
      if (!kill_at || now + CHILD_GRACE < *kill_at) {
        kill_at = now + CHILD_GRACE;
      }
    }
    if (!leader_ended) {
      const int ended = group.ended();
      if (ended < 0) {
        lost("waiting for the child", ECHILD);
        return result;
      }
      leader_ended = ended > 0;
      if (leader_ended && !kill_at && (out_read.get() >= 0 || err_read.get() >= 0)) {
        kill_at = now + CHILD_GRACE;
      }
    }
    if (leader_ended && out_read.get() < 0 && err_read.get() < 0) {
      break;
    }
    if (kill_at && !give_up_at && now >= *kill_at) {
      group.signal(SIGKILL);
      give_up_at = now + CHILD_KILLED_WAIT;
    }
    if (give_up_at && leader_ended && now >= *give_up_at) {
      // Whoever still holds the pipes left the group; the leader's end is what counts.
      break;
    }
    pollfd fds[2];
    nfds_t count = 0;
    for (const int fd : {out_read.get(), err_read.get()}) {
      if (fd >= 0) {
        fds[count].fd = fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        ++count;
      }
    }
    int timeout = CHILD_POLL_MS;
    if (count == 0) {
      // Both pipes closed and the leader not yet reaped: it usually exits a moment later, so look again soon,
      // backing off to the usual step for a child that closed its pipes and goes on running.
      timeout = idle_ms;
      idle_ms = std::min(idle_ms * 2, CHILD_POLL_MS);
    }
    if (kill_at && !give_up_at) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*kill_at - now).count();
      timeout = static_cast<int>(std::max<long long>(0, std::min<long long>(timeout, left)));
    }
    const int ready = ::poll(count == 0 ? nullptr : fds, count, timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      lost("waiting for the child's output", errno);
      return result;
    }
    for (nfds_t i = 0; i < count; ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      const bool is_out = fds[i].fd == out_read.get();
      const auto n = ::read(fds[i].fd, buffer, sizeof(buffer));
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        lost("reading the child's output", errno);
        return result;
      }
      if (n == 0) {
        if (is_out) {
          out_read.reset();
        } else {
          err_read.reset();
          if (!line.empty()) {
            deliver(line);
            line.clear();
          }
        }
        continue;
      }
      if (is_out) {
        result.stdout_.append(buffer, static_cast<std::size_t>(n));
        continue;
      }
      for (ssize_t at = 0; at < n; ++at) {
        if (buffer[at] == '\n') {
          deliver(line);
          line.clear();
        } else {
          line.push_back(buffer[at]);
          if (line.size() >= STDERR_LINE_BYTES) {
            deliver(line);
            line.clear();
          }
        }
      }
    }
  }
  int status = 0;
  if (!group.finish(status)) {
    lost("reaping the child", ECHILD);
    return result;
  }
  if (terminated) {
    result.kind_ = ChildResult::Kind::INTERRUPTED;
    return result;
  }
  result.kind_ = ChildResult::Kind::FINISHED;
  result.signaled_ = WIFSIGNALED(status);
  result.code_ = result.signaled_ ? WTERMSIG(status) : WEXITSTATUS(status);
  return result;
#endif
}

/**
 * @brief Runs a program with arguments, no shell, and returns what it printed on stdout.
 *
 * Only for short probes such as `atlc --version`: a failure to run, a failing exit and an interruption read alike
 * (the caller checks `interrupted()`). Never use it for a command whose outcome must be known (a send).
 *
 * @param argv The program (a path) and its arguments.
 * @return The output, or nothing when it could not run, was interrupted or exited with an error.
 */
inline std::optional<std::string> capture(const std::vector<std::string>& argv) {
  auto result = run_child(argv, [](const std::string&) {});
  if (result.kind_ != ChildResult::Kind::FINISHED || result.signaled_ || result.code_ != 0) {
    return std::nullopt;
  }
  return std::move(result.stdout_);
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
