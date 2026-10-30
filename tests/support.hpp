#pragma once

#include <cmdatl/cli.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace support {

/** @brief When above zero, the allocation that brings it to zero throws `std::bad_alloc` (a one-shot failure). */
inline std::atomic<long> fail_at{0};

/**
 * @brief Fails the n-th allocation from now (1 for the next one); 0 disarms.
 * @param n The allocation.
 */
inline void fail_allocation(long n) {
  fail_at.store(n);
}

/** @brief What one command line wrote and returned. */
struct Ran {
  /** @brief The exit code. */
  int exit_;
  /** @brief What it wrote on stdout. */
  std::string out_;
  /** @brief What it wrote on stderr. */
  std::string err_;
};

/**
 * @brief Runs a command line in process against cmdatl's commands.
 * @param args The command line, without the program's name.
 * @param terminal Whether to behave as if writing to a terminal.
 * @return What it wrote and returned.
 */
inline Ran run(const std::vector<std::string>& args, bool terminal = false) {
  std::ostringstream out;
  std::ostringstream err;
  const auto exit = cmdatl::run(args, out, err, terminal);
  return {exit, out.str(), err.str()};
}

/**
 * @brief Runs a command line in process against a given table.
 * @param args The command line, without the program's name.
 * @param table The commands.
 * @return What it wrote and returned.
 */
inline Ran run_with(const std::vector<std::string>& args, const cmdatl::Table& table) {
  std::ostringstream out;
  std::ostringstream err;
  const auto exit = cmdatl::run(args, out, err, false, table);
  return {exit, out.str(), err.str()};
}

/**
 * @brief A JSON reply's object.
 * @param text The reply.
 * @return The object.
 */
inline boost::json::object reply_of(const std::string& text) {
  return boost::json::parse(text).as_object();
}

/**
 * @brief The error code of a JSON reply.
 * @param text The reply.
 * @return The code.
 */
inline std::string code_of(const std::string& text) {
  return std::string(reply_of(text).at("error").as_object().at("code").as_string());
}

/**
 * @brief A text's lines, without their newlines.
 * @param text The text.
 * @return The lines.
 */
inline std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

/**
 * @brief Splits a shell-like line into words without running anything: double and single quotes keep spaces, a
 *   backslash outside quotes keeps the next character, and `#` after a space, outside quotes, starts a comment.
 * @param text The line.
 * @return The words, or nothing for an unfinished quote (a shell would wait for more).
 */
inline std::optional<std::vector<std::string>> shell_words(const std::string& text) {
  std::vector<std::string> words;
  std::string word;
  bool in_word = false;
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      } else {
        word.push_back(c);
      }
    } else if (c == '\\' && i + 1 < text.size()) {
      word.push_back(text[++i]);
      in_word = true;
    } else if (c == '"' || c == '\'') {
      quote = c;
      in_word = true;
    } else if (c == ' ' || c == '\t') {
      if (in_word) {
        words.push_back(word);
        word.clear();
        in_word = false;
      }
    } else if (c == '#' && !in_word) {
      break;
    } else {
      word.push_back(c);
      in_word = true;
    }
  }
  if (quote != 0) {
    return std::nullopt;
  }
  if (in_word) {
    words.push_back(word);
  }
  return words;
}

/**
 * @brief Whether a command declares an exit code and an error code (the schema's own list, the common ones
 *   included).
 * @param path The command's path.
 * @param exit The exit code.
 * @param code The error code, or empty for a success.
 * @return Whether both are declared.
 */
inline bool declares(const std::vector<std::string>& path, int exit, const std::string& code) {
  const cmdatl::Table table(cmdatl::command_list());
  const auto found = table.resolve(path);
  if (found.command_ == nullptr) {
    return false;
  }
  const auto& command = *found.command_;
  const auto& exits = command.exits_;
  const bool exit_ok = std::any_of(exits.begin(), exits.end(), [exit](cmdatl::Exit e) {
    return static_cast<int>(e) == exit;
  });
  const auto errors = cmdatl::errors_of(command);
  const bool code_ok = code.empty() || std::find(errors.begin(), errors.end(), code) != errors.end();
  return exit_ok && code_ok;
}

/** @brief Ends the test binary when a test outlives its time, so a broken drain or reap fails instead of hanging. */
class Watchdog {
public:
  /**
   * @brief Starts the watch.
   * @param what The test, named in the message.
   * @param seconds The time it has.
   */
  Watchdog(std::string what, int seconds)
  : what_(std::move(what)) {
    thread_ = std::thread([this, seconds] {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!done_.wait_for(lock, std::chrono::seconds(seconds), [this] { return finished_; })) {
        std::fprintf(stderr, "watchdog: %s did not finish in %d s\n", what_.c_str(), seconds);
        std::fflush(stderr);
        std::_Exit(124);
      }
    });
  }

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  /** @brief Stops the watch. */
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
    }
    done_.notify_all();
    thread_.join();
  }

private:
  std::string what_;
  std::mutex mutex_;
  std::condition_variable done_;
  bool finished_{false};
  std::thread thread_;
};

#if !defined(_WIN32)
/**
 * @brief How many descriptors this process has open.
 * @return The count.
 */
inline int open_fds() {
  int n = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/dev/fd")) {
    (void)entry;
    ++n;
  }
  return n;
}

/**
 * @brief Whether this process has no child left, running or unreaped.
 *
 * A child seen running gets two seconds to go: on macOS a `posix_spawn` that fails (a missing program) leaves a
 * process the kernel ends and reaps by itself a moment later. An unreaped child fails at once.
 *
 * @return Whether it has none.
 */
inline bool no_children() {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (true) {
    errno = 0;
    const auto waited = ::waitpid(-1, nullptr, WNOHANG);
    if (waited == -1 && errno == ECHILD) {
      return true;
    }
    if (waited != 0 || std::chrono::steady_clock::now() > until) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

/**
 * @brief Whether every process is gone, waiting a little for the system to reap orphans.
 * @param pids The processes.
 * @return Whether none is left.
 */
inline bool all_gone(const std::vector<pid_t>& pids) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (true) {
    const bool gone = std::all_of(pids.begin(), pids.end(), [](pid_t pid) {
      return ::kill(pid, 0) != 0 && errno == ESRCH;
    });
    if (gone || std::chrono::steady_clock::now() > until) {
      return gone;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

/** @brief A unique scratch directory, removed with everything in it when its owner goes. */
class ScratchDir {
public:
  /** @brief Creates it. */
  ScratchDir() {
    static std::atomic<int> count{0};
    const auto name = "cmdatl-test-" + std::to_string(::getpid()) + "-" + std::to_string(count++);
    // The canonical path, so it compares with what getcwd and weakly_canonical give (/tmp is a link on macOS).
    path_ = std::filesystem::canonical(std::filesystem::temp_directory_path()) / name;
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  /** @brief Removes it. */
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  /**
   * @brief Where it is.
   * @return The path.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

/**
 * @brief Reads a whole file.
 * @param path The file.
 * @return Its contents, or empty.
 */
inline std::string slurp(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

/**
 * @brief Writes a whole file.
 * @param path The file.
 * @param text The contents.
 */
inline void spit(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

/** @brief A valid `atlc spec --json` reply. */
inline const std::string SPEC_OK =
  R"({"atlc_spec":1,"status":"ok","atlc":"0.0.0-9000","program":{"path":"/x/dip.atls","struct":"Dip"},)"
  R"("build":{"algotradelib":null,"duckdb":null,"indicators":"incremental"},"store":false,)"
  R"("orders":{"type":"market","offset":"0"},"args":[],"warnings":[],"errors":[]})";

/** @brief A report as `atlc run` prints it, shortened. */
inline const std::string REPORT =
  R"({"bars":250,"executions":4,"opening_equity":"100000","closing_equity":"101234.5",)"
  R"("total_return":0.012345,"halted":null,"rejected":0,"pricing_refused":0,"caveats":{"gaps":0}})";

/**
 * @brief The fake atlc, named by `CMDATL_ATLC` while its owner lives, doing what its script says.
 *
 * The script's steps are `WHEN ACTION [TEXT]` lines (`tests/fake-atlc.cpp`); it answers `--version` unless the
 * script says otherwise.
 */
class FakeAtlc {
public:
  /**
   * @brief Writes the script and points `CMDATL_ATLC` at the fake.
   * @param script The steps.
   * @param answer_version Whether to answer `--version` with `atlc 0.0.0-9000` first.
   */
  explicit FakeAtlc(const std::string& script, bool answer_version = true) {
    script_ = dir_.path() / "script";
    spit(script_, (answer_version ? "version out atlc 0.0.0-9000\n" : "") + script);
    ::setenv("CMDATL_ATLC", CMDATL_FAKE_ATLC, 1);
    ::setenv("FAKE_ATLC_SCRIPT", script_.c_str(), 1);
  }

  FakeAtlc(const FakeAtlc&) = delete;
  FakeAtlc& operator=(const FakeAtlc&) = delete;

  /** @brief Unsets the variables. */
  ~FakeAtlc() {
    ::unsetenv("CMDATL_ATLC");
    ::unsetenv("FAKE_ATLC_SCRIPT");
  }

  /**
   * @brief The scratch directory the script is in.
   * @return The path.
   */
  const std::filesystem::path& dir() const noexcept { return dir_.path(); }

  /**
   * @brief The processes the script recorded (`pid`, `grandchild`).
   * @return Their ids.
   */
  std::vector<pid_t> pids() const {
    std::vector<pid_t> out;
    for (const auto& line : lines_of(slurp(script_.string() + ".pids"))) {
      out.push_back(static_cast<pid_t>(std::stol(line)));
    }
    return out;
  }

  /**
   * @brief The arguments a command was called with (`args`).
   * @param mode `spec` or `run`.
   * @return The arguments after the program's name.
   */
  std::vector<std::string> args(const std::string& mode) const {
    return lines_of(slurp(script_.string() + "." + mode + ".args"));
  }

  /**
   * @brief Whether a mark was made (`touch`).
   * @param mark The mark.
   * @return Whether it was.
   */
  bool marked(const std::string& mark) const {
    return std::filesystem::exists(script_.string() + "." + mark);
  }

  /**
   * @brief Waits until a mark is made.
   * @param mark The mark.
   * @param seconds How long at most.
   * @return Whether it was made.
   */
  bool wait_for(const std::string& mark, int seconds = 10) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!marked(mark)) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

private:
  ScratchDir dir_;
  std::filesystem::path script_;
};

/** @brief Which of cmdatl's standard descriptors to close before it starts. */
struct Closed {
  /** @brief stdin. */
  bool in_{false};
  /** @brief stdout. */
  bool out_{false};
  /** @brief stderr. */
  bool err_{false};
};

/** @brief The cmdatl binary, started with pipes on its stdout and stderr, and killed if a test leaves it running. */
class Spawned {
public:
  /**
   * @brief Starts cmdatl.
   * @param args Its arguments.
   * @param closed Which standard descriptors it starts without.
   */
  explicit Spawned(const std::vector<std::string>& args, Closed closed = {}) {
    std::vector<std::string> argv{CMDATL_BINARY};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> raw;
    for (auto& arg : argv) {
      raw.push_back(const_cast<char*>(arg.c_str()));
    }
    raw.push_back(nullptr);
    int out[2];
    int err[2];
    REQUIRE(::pipe(out) == 0);
    REQUIRE(::pipe(err) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (closed.in_) {
      posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
    }
    if (closed.out_) {
      posix_spawn_file_actions_addclose(&actions, STDOUT_FILENO);
    } else {
      posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    }
    if (closed.err_) {
      posix_spawn_file_actions_addclose(&actions, STDERR_FILENO);
    } else {
      posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    }
    for (const int fd : {out[0], out[1], err[0], err[1]}) {
      posix_spawn_file_actions_addclose(&actions, fd);
    }
    const int spawned = posix_spawn(&pid_, raw[0], &actions, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    ::close(err[1]);
    out_.reset(out[0]);
    err_.reset(err[0]);
    REQUIRE(spawned == 0);
  }

  Spawned(const Spawned&) = delete;
  Spawned& operator=(const Spawned&) = delete;

  /** @brief Kills and reaps cmdatl if the test did not wait for it. */
  ~Spawned() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
  }

  /**
   * @brief cmdatl's process id.
   * @return It.
   */
  pid_t pid() const noexcept { return pid_; }

  /**
   * @brief Sends cmdatl a signal.
   * @param sig The signal.
   */
  void signal(int sig) const { ::kill(pid_, sig); }

  /**
   * @brief Reads one line of stdout or stderr.
   * @param from_err Whether from stderr.
   * @param seconds How long at most.
   * @return The line, or nothing at the end or on time-out.
   */
  std::optional<std::string> line(bool from_err, int seconds = 10) {
    auto& buffer = from_err ? err_text_ : out_text_;
    auto& fd = from_err ? err_ : out_;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (buffer.find('\n') == std::string::npos) {
      if (fd.get() < 0 || std::chrono::steady_clock::now() > until) {
        return std::nullopt;
      }
      read_some(fd, buffer, 100);
    }
    const auto end = buffer.find('\n');
    auto out = buffer.substr(0, end);
    buffer.erase(0, end + 1);
    return out;
  }

  /** @brief Closes cmdatl's stdout at the reading end, as `head` does when it has read enough. */
  void close_stdout() { out_.reset(); }

  /**
   * @brief Waits for cmdatl to end, reading both pipes meanwhile.
   * @param seconds How long at most; past it, the test fails and cmdatl is killed.
   * @return Its wait status.
   */
  int wait(int seconds = 20) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int status = 0;
    while (true) {
      const auto waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        pid_ = -1;
        break;
      }
      if (std::chrono::steady_clock::now() > until) {
        FAIL("cmdatl did not end in " << seconds << " s");
      }
      if (out_.get() >= 0) {
        read_some(out_, out_text_, 0);
      }
      if (err_.get() >= 0) {
        read_some(err_, err_text_, 0);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (out_.get() >= 0 && read_some(out_, out_text_, 100)) {
    }
    while (err_.get() >= 0 && read_some(err_, err_text_, 100)) {
    }
    return status;
  }

  /**
   * @brief What cmdatl wrote on stdout and was not read by `line`.
   * @return The text.
   */
  const std::string& out() const noexcept { return out_text_; }

  /**
   * @brief What cmdatl wrote on stderr and was not read by `line`.
   * @return The text.
   */
  const std::string& err() const noexcept { return err_text_; }

private:
  /**
   * @brief Reads what is there within a time.
   * @param fd The pipe.
   * @param buffer Where it goes.
   * @param ms How long to wait for something.
   * @return Whether something was read; false at the end (the pipe is then closed) or on time-out.
   */
  static bool read_some(cmdatl::ScopedFd& fd, std::string& buffer, int ms) {
    pollfd p{fd.get(), POLLIN, 0};
    if (::poll(&p, 1, ms) <= 0) {
      return false;
    }
    char chunk[4096];
    const auto n = ::read(fd.get(), chunk, sizeof(chunk));
    if (n <= 0) {
      fd.reset();
      return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    return true;
  }

  pid_t pid_{-1};
  cmdatl::ScopedFd out_;
  cmdatl::ScopedFd err_;
  std::string out_text_;
  std::string err_text_;
};
#endif

}  // namespace support
