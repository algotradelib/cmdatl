#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

using namespace support;

namespace {

/** @brief A callback that drops every line. */
const std::function<void(const std::string&)> ignore_lines = [](const std::string&) {};

/**
 * @brief The fake atlc as `atlc run`, run in process by `run_child`.
 * @param on_line The stderr callback.
 * @return How it ended.
 */
cmdatl::ChildResult run_fake(const std::function<void(const std::string&)>& on_line = ignore_lines) {
  return cmdatl::run_child({CMDATL_FAKE_ATLC, "run"}, on_line);
}

/**
 * @brief The highest descriptor this process has open.
 * @return It.
 */
int highest_fd() {
  int highest = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/dev/fd")) {
    highest = std::max(highest, std::stoi(entry.path().filename().string()));
  }
  return highest;
}

/** @brief A stream buffer that takes the first lines and refuses everything after them. */
class RefusingAfter : public std::stringbuf {
public:
  /**
   * @brief Sets how many lines it takes.
   * @param lines The lines.
   */
  explicit RefusingAfter(int lines)
  : lines_(lines) {}

protected:
  /**
   * @brief Takes a character while lines are left.
   * @param c The character.
   * @return It, or end of file once the lines are used.
   */
  int_type overflow(int_type c) override {
    if (lines_ <= 0) {
      return traits_type::eof();
    }
    if (c == '\n') {
      --lines_;
    }
    return std::stringbuf::overflow(c);
  }

  /**
   * @brief Takes characters while lines are left.
   * @param s The characters.
   * @param n How many.
   * @return How many it took.
   */
  std::streamsize xsputn(const char* s, std::streamsize n) override {
    std::streamsize taken = 0;
    for (; taken < n; ++taken) {
      if (overflow(traits_type::to_int_type(s[taken])) == traits_type::eof()) {
        break;
      }
    }
    return taken;
  }

private:
  int lines_;
};

/** @brief What a signalled cmdatl ended with. */
struct Stopped {
  /** @brief Its wait status. */
  int status_;
  /** @brief Its stdout. */
  std::string out_;
  /** @brief Its stderr after the lines read before the signal. */
  std::string err_;
  /** @brief The seconds from the signal to its end. */
  double seconds_;
};

/**
 * @brief Starts `cmdatl --json strategy backtest s.toml` against the fake, waits for a mark, signals cmdatl alone,
 *   and waits for its end.
 * @param fake The fake.
 * @param mark The mark the script makes when it is where the signal should find it.
 * @param sig The signal.
 * @param first_line Whether to read cmdatl's first progress line before the signal, and check it.
 * @return How it ended.
 */
Stopped stop_backtest(const FakeAtlc& fake, const std::string& mark, int sig, bool first_line = false) {
  Spawned cmdatl({"--json", "strategy", "backtest", "s.toml"});
  if (first_line) {
    // Written before atlc is looked for, so it arrives while `atlc --version` is still blocked.
    CHECK(cmdatl.line(true) == std::optional<std::string>("cmdatl: replaying s.toml"));
  }
  REQUIRE(fake.wait_for(mark));
  if (first_line) {
    CHECK(!cmdatl.out().size());
  }
  const auto start = std::chrono::steady_clock::now();
  cmdatl.signal(sig);
  const int status = cmdatl.wait(20);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return {status, cmdatl.out(), cmdatl.err(), seconds};
}

/**
 * @brief Checks that cmdatl ended by itself with exit 130 and one `interrupted` reply in JSON.
 * @param stopped How it ended.
 */
void check_interrupted(const Stopped& stopped) {
  INFO("stdout: " << stopped.out_ << "\nstderr: " << stopped.err_);
  REQUIRE(WIFEXITED(stopped.status_));
  CHECK(WEXITSTATUS(stopped.status_) == 130);
  const auto lines = lines_of(stopped.out_);
  REQUIRE(lines.size() == 1);
  CHECK(code_of(lines.front()) == "interrupted");
  CHECK(declares({"strategy", "backtest"}, 130, "interrupted"));
}

}  // namespace

TEST_CASE("run_child drains stdout and stderr larger than a pipe and interleaved") {
  const Watchdog watchdog("the flood", 60);
  const FakeAtlc fake("run flood 300000\n");
  std::size_t lines = 0;
  std::size_t odd = 0;
  const auto result = run_fake([&](const std::string& line) {
    ++lines;
    odd += line == std::string(99, 'e') ? 0 : 1;
  });
  REQUIRE(result.kind_ == cmdatl::ChildResult::Kind::FINISHED);
  CHECK(result.code_ == 0);
  CHECK(result.stdout_.size() == 293 * 1024);
  CHECK(lines == 2930);
  CHECK(odd == 0);
  CHECK(result.stderr_tail_.size() == cmdatl::STDERR_TAIL_LINES);
}

TEST_CASE("run_child hands on an unterminated last line and splits a line too long to keep") {
  const Watchdog watchdog("the last line", 30);
  {
    const FakeAtlc fake("run err first\nrun errpart last\nrun exit 3\n");
    std::vector<std::string> lines;
    const auto result = run_fake([&lines](const std::string& line) { lines.push_back(line); });
    REQUIRE(result.kind_ == cmdatl::ChildResult::Kind::FINISHED);
    CHECK(!result.signaled_);
    CHECK(result.code_ == 3);
    CHECK(lines == std::vector<std::string>{"first", "last"});
    CHECK(result.stderr_tail_ == std::deque<std::string>{"first", "last"});
  }
  {
    const FakeAtlc fake("run errbytes 20000\n");
    std::vector<std::size_t> sizes;
    const auto result = run_fake([&sizes](const std::string& line) { sizes.push_back(line.size()); });
    CHECK(sizes == std::vector<std::size_t>{8192, 8192, 3616});
    std::size_t kept = 0;
    for (const auto& line : result.stderr_tail_) {
      kept += line.size();
    }
    CHECK(kept <= cmdatl::STDERR_TAIL_BYTES);
  }
  {
    const FakeAtlc fake("run errlines 50\n");
    const auto result = run_fake();
    REQUIRE(result.stderr_tail_.size() == 20);
    CHECK(result.stderr_tail_.front() == "line 30");
    CHECK(result.stderr_tail_.back() == "line 49");
  }
}

TEST_CASE("run_child tells a signal from an exit code and a missing program from both") {
  const Watchdog watchdog("the endings", 30);
  {
    const FakeAtlc fake("run raise 9\n");
    const auto result = run_fake();
    REQUIRE(result.kind_ == cmdatl::ChildResult::Kind::FINISHED);
    CHECK(result.signaled_);
    CHECK(result.code_ == 9);
  }
  {
    const FakeAtlc fake("run exit 130\n");
    const auto result = run_fake();
    CHECK(!result.signaled_);
    CHECK(result.code_ == 130);
  }
  const int before = open_fds();
  const auto missing = cmdatl::run_child({"/nonexistent/atlc", "run"}, ignore_lines);
  CHECK(missing.kind_ == cmdatl::ChildResult::Kind::NOT_STARTED);
  CHECK(!missing.error_.empty());
  CHECK(open_fds() == before);
  CHECK(no_children());
  CHECK(cmdatl::run_child({}, ignore_lines).kind_ == cmdatl::ChildResult::Kind::NOT_STARTED);
}

TEST_CASE("a child that closes its pipes and exits soon after is reaped soon, not a poll step later") {
  const Watchdog watchdog("the reap after end of file", 30);
  // atlc run's shape: its replay closes the pipes, and atlc exits a moment later.
  std::vector<double> took;
  for (int i = 0; i < 5; ++i) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = cmdatl::run_child({"/bin/sh", "-c", "exec >&- 2>&-; sleep 0.01; exit 7"}, ignore_lines);
    took.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    REQUIRE(result.kind_ == cmdatl::ChildResult::Kind::FINISHED);
    CHECK(result.code_ == 7);
  }
  // The old wait slept a whole 100 ms step once the pipes had closed, so its runs took about 105-113 ms; the new one
  // takes a few milliseconds more than the child's 10 ms. The fastest of five is checked against 95 ms, a margin that
  // eight busy workers on eight cores did not exhaust in 100 runs; it is a timing check, not a guarantee.
  CHECK(*std::min_element(took.begin(), took.end()) < 95.0);
}

TEST_CASE("a throwing callback kills the child's whole group with its grandchild") {
  const Watchdog watchdog("the abandoned group", 30);
  const FakeAtlc fake("run grandchild hold\nrun pid\nrun err hello\nrun block\n");
  const int before = open_fds();
  CHECK_THROWS_AS(run_fake([](const std::string&) { throw std::runtime_error("the output broke"); }), std::runtime_error);
  CHECK(open_fds() == before);
  CHECK(no_children());
  const auto pids = fake.pids();
  CHECK(pids.size() == 2);
  CHECK(all_gone(pids));
}

TEST_CASE("a leader that ends while its grandchild holds the pipes is reported and the grandchild killed") {
  const Watchdog watchdog("the held pipes", 30);
  const FakeAtlc fake("run grandchild hold\nrun pid\nrun out done\nrun exit 4\n");
  const auto start = std::chrono::steady_clock::now();
  const auto result = run_fake();
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REQUIRE(result.kind_ == cmdatl::ChildResult::Kind::FINISHED);
  CHECK(!result.signaled_);
  CHECK(result.code_ == 4);
  CHECK(result.stdout_ == "done\n");
  CHECK(all_gone(fake.pids()));
  CHECK(no_children());
  // The two seconds' grace after the leader ended, give or take the scheduler.
  CHECK(seconds > 1.5);
  CHECK(seconds < 8.0);
}

TEST_CASE("run_child works with stdin or stdout or stderr closed") {
  const Watchdog watchdog("closed standard descriptors", 30);
  const FakeAtlc fake("run out hello\nrun err world\n");
  for (const int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    const int saved = ::dup(fd);
    REQUIRE(saved >= 0);
    ::close(fd);
    const auto result = run_fake();
    ::dup2(saved, fd);
    ::close(saved);
    INFO("closed " << fd);
    REQUIRE(result.kind_ == cmdatl::ChildResult::Kind::FINISHED);
    CHECK(result.code_ == 0);
    CHECK(result.stdout_ == "hello\n");
    CHECK(result.stderr_tail_ == std::deque<std::string>{"world"});
  }
}

TEST_CASE("cmdatl started with stdin or stdout or stderr closed") {
  const Watchdog watchdog("cmdatl without its descriptors", 60);
  const auto script = "spec out " + SPEC_OK + "\nrun args\nrun out " + REPORT + "\n";
  {
    const FakeAtlc fake(script);
    Spawned cmdatl({"--json", "strategy", "backtest", "s.toml"}, {true, false, false});
    const int status = cmdatl.wait();
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(reply_of(cmdatl.out()).at("report").as_object().at("executions").as_int64() == 4);
  }
  {
    // The reply cannot be written, but atlc ran: the pipes cmdatl opened were moved off the closed descriptor.
    const FakeAtlc fake(script);
    Spawned cmdatl({"--json", "strategy", "backtest", "s.toml"}, {false, true, false});
    const int status = cmdatl.wait();
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(!fake.args("run").empty());
    CHECK(cmdatl.err().find("its reply could not be written") != std::string::npos);
  }
  {
    // Progress cannot be written: the command stops before atlc runs, and says so on stdout.
    const FakeAtlc fake(script);
    Spawned cmdatl({"--json", "strategy", "backtest", "s.toml"}, {false, false, true});
    const int status = cmdatl.wait();
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(fake.args("run").empty());
    CHECK(code_of(cmdatl.out()) == "internal.error");
  }
}

TEST_CASE("an allocation failing anywhere in a process probe leaks no descriptor and leaves no child") {
  const Watchdog watchdog("the probe sweep", 60);
  const std::vector<std::string> argv{"/bin/echo", std::string(256, 'x')};
  const int before = open_fds();
  int threw = 0;
  for (long n = 1; n <= 12; ++n) {
    fail_allocation(n);
    try {
      (void)cmdatl::capture(argv);
    } catch (const std::bad_alloc&) {
      ++threw;
    }
    fail_allocation(0);
    INFO("allocation " << n);
    CHECK(open_fds() == before);
    CHECK(no_children());
  }
  CHECK(threw > 0);
  const auto normal = cmdatl::capture(argv);
  REQUIRE(normal);
  CHECK(normal->size() == 257);
  CHECK(open_fds() == before);
}

TEST_CASE("an allocation failing anywhere in run_child or its lines or its tail or its callback leaks nothing") {
  const Watchdog watchdog("the run_child sweep", 120);
  // Lines longer than a string keeps inline, so each line, its copy in the tail and the callback's copy allocate.
  std::string script = "run out " + REPORT + "\n";
  for (int i = 0; i < 25; ++i) {
    script += "run err a stderr line long enough to need its own allocation, number " + std::to_string(i) + "\n";
  }
  const FakeAtlc fake(script + "run errpart an unterminated last line, long enough to allocate too\n");
  const int before = open_fds();
  int threw = 0;
  long first_bad = 0;
  long n = 1;
  for (; n < 2000; ++n) {
    std::vector<std::string> copies;
    fail_allocation(n);
    try {
      (void)run_fake([&copies](const std::string& line) { copies.push_back(line); });
    } catch (const std::bad_alloc&) {
      ++threw;
    }
    const bool consumed = fail_at.load() == 0;
    fail_allocation(0);
    if ((open_fds() != before || !no_children()) && first_bad == 0) {
      first_bad = n;
    }
    if (!consumed) {
      break;
    }
  }
  INFO("allocations swept: " << n << ", first leak at " << first_bad);
  CHECK(first_bad == 0);
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
  // The sanitizer's runtime takes the allocations libc++ makes inside its own library (std::string's members), so
  // this binary's operator new sees, and fails, only the inlined ones.
  CHECK(threw > 5);
#else
  CHECK(threw > 50);
#endif
  CHECK(n < 2000);
}

TEST_CASE("an allocation failing anywhere in a backtest becomes a reply or a lost-reply exit and never a leak") {
  const Watchdog watchdog("the backtest sweep", 300);
  const FakeAtlc fake("spec out " + SPEC_OK + "\nrun err building\nrun out " + REPORT + "\n");
  const int before = open_fds();
  long first_bad = 0;
  long n = 1;
  for (; n < 20000; ++n) {
    std::ostringstream out;
    std::ostringstream err;
    fail_allocation(n);
    int exit = -1;
    try {
      exit = cmdatl::run({"--format", "jsonl", "strategy", "backtest", "s.toml"}, out, err, false);
    } catch (const std::bad_alloc&) {
      // Before the command started (reading the line): main's last resort.
    }
    const bool consumed = fail_at.load() == 0;
    fail_allocation(0);
    if ((open_fds() != before || !no_children() || (exit != -1 && exit != 0 && exit != 1)) && first_bad == 0) {
      first_bad = n;
    }
    if (!consumed) {
      CHECK(exit == 0);
      break;
    }
  }
  INFO("allocations swept: " << n << ", first problem at " << first_bad);
  CHECK(first_bad == 0);
  CHECK(n < 20000);
}

TEST_CASE("a failing or throwing output stream while atlc runs kills its group and leaks nothing") {
  const Watchdog watchdog("the broken output", 60);
  const auto script = "spec out " + SPEC_OK + "\nrun grandchild hold\nrun pid\nrun err building\nrun block\n";
  const int before = open_fds();
  {
    // Text: progress on stderr, which takes cmdatl's first line and refuses atlc's.
    const FakeAtlc fake(script);
    RefusingAfter refusing(1);
    std::ostream err(&refusing);
    std::ostringstream out;
    CHECK(cmdatl::run({"--format", "text", "strategy", "backtest", "s.toml"}, out, err, false) == 1);
    CHECK(no_children());
    CHECK(all_gone(fake.pids()));
  }
  {
    // jsonl: progress on stdout, which throws once its first line is written.
    const FakeAtlc fake(script);
    RefusingAfter refusing(1);
    std::ostream out(&refusing);
    out.exceptions(std::ios::badbit);
    std::ostringstream err;
    CHECK(cmdatl::run({"--format", "jsonl", "strategy", "backtest", "s.toml"}, out, err, false) == 1);
    CHECK(err.str().find("its reply could not be written") != std::string::npos);
    CHECK(no_children());
    CHECK(all_gone(fake.pids()));
  }
  CHECK(open_fds() == before);
}

TEST_CASE("descriptors running out at either pipe or at the spawn leak nothing") {
  const Watchdog watchdog("descriptor exhaustion", 60);
  const FakeAtlc fake("run out ok\n");
  rlimit saved{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
  // The limit below leaves exactly `spare` descriptors free only when none is free under the highest open one; a
  // runner can start the process with gaps there, so they are filled first.
  const int highest = highest_fd();
  std::vector<int> fillers;
  for (;;) {
    const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    if (fd > highest) {
      ::close(fd);
      break;
    }
    fillers.push_back(fd);
  }
  const int before = open_fds();
  std::vector<cmdatl::ChildResult> results;
  for (int spare = 0; spare <= 10; ++spare) {
    rlimit low = saved;
    low.rlim_cur = static_cast<rlim_t>(highest + 1 + spare);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &low) == 0);
    auto result = run_fake();
    ::setrlimit(RLIMIT_NOFILE, &saved);
    results.push_back(std::move(result));
    INFO("spare descriptors: " << spare << ", result " << static_cast<int>(results.back().kind_) << ": "
                                << results.back().error_);
    CHECK(open_fds() == before);
    CHECK(no_children());
  }
  for (const int fd : fillers) {
    ::close(fd);
  }
  const auto refused = std::count_if(results.begin(), results.end(), [](const cmdatl::ChildResult& result) {
    return result.kind_ == cmdatl::ChildResult::Kind::NOT_STARTED &&
      result.error_.find("Too many open files") != std::string::npos;
  });
  // Spares 0 to 4 cannot hold the second pipe (each pipe needs two descriptors and two more while it is lifted).
  CHECK(refused >= 5);
  CHECK(results.back().kind_ == cmdatl::ChildResult::Kind::FINISHED);
}

TEST_CASE("SIGINT or SIGTERM while cmdatl probes atlc is interrupted and never atlc.missing") {
  const Watchdog watchdog("an interrupted probe", 60);
  for (const int sig : {SIGINT, SIGTERM}) {
    INFO("signal " << sig);
    const FakeAtlc fake("version pid\nversion touch probing\nversion block\n", false);
    const auto stopped = stop_backtest(fake, "probing", sig, true);
    check_interrupted(stopped);
    CHECK(all_gone(fake.pids()));
  }
}

TEST_CASE("SIGINT or SIGTERM while atlc runs stops its group whatever it is doing") {
  const Watchdog watchdog("interrupted runs", 120);
  const std::string spec = "spec out " + SPEC_OK + "\n";
  const std::vector<std::pair<std::string, std::string>> scenarios{
    {"draining a long reply", "run pid\nrun touch running\nrun forever\n"},
    {"waiting on a child with a grandchild", "run grandchild hold\nrun pid\nrun touch running\nrun block\n"},
    {"just before a wait: both pipes closed", "run pid\nrun close\nrun touch running\nrun block\n"},
    {"a grandchild that ignores SIGTERM, pipes closed", "run grandchild ignore-term-closed\nrun pid\nrun touch running\nrun block\n"},
  };
  for (const auto& scenario : scenarios) {
    for (const int sig : {SIGINT, SIGTERM}) {
      INFO(scenario.first << ", signal " << sig);
      const FakeAtlc fake(spec + scenario.second);
      const auto stopped = stop_backtest(fake, "running", sig);
      check_interrupted(stopped);
      CHECK(all_gone(fake.pids()));
    }
  }
}

TEST_CASE("what ignores SIGTERM is killed two seconds later") {
  const Watchdog watchdog("the grace period", 120);
  const std::string spec = "spec out " + SPEC_OK + "\n";
  const std::vector<std::pair<std::string, std::string>> scenarios{
    {"a grandchild that ignores SIGTERM, pipes kept open", "run grandchild ignore-term\nrun pid\nrun touch running\nrun block\n"},
    {"atlc itself ignoring SIGTERM", "run ignore-term\nrun pid\nrun touch running\nrun block\n"},
  };
  for (const auto& scenario : scenarios) {
    for (const int sig : {SIGINT, SIGTERM}) {
      INFO(scenario.first << ", signal " << sig);
      const FakeAtlc fake(spec + scenario.second);
      const auto stopped = stop_backtest(fake, "running", sig);
      check_interrupted(stopped);
      CHECK(all_gone(fake.pids()));
      // Two seconds by the monotonic clock, give or take the scheduler.
      CHECK(stopped.seconds_ > 1.5);
      CHECK(stopped.seconds_ < 8.0);
    }
  }
}

TEST_CASE("Ctrl-C on a terminal reaches cmdatl only and it stops atlc's group and replies") {
  const Watchdog watchdog("Ctrl-C", 60);
  const std::vector<std::pair<std::string, std::string>> scenarios{
    {"while probing", "version pid\nversion touch waiting\nversion block\n"},
    {"while running", "version out atlc 0.0.0-9000\nspec out " + SPEC_OK + "\nrun grandchild hold\nrun pid\nrun touch waiting\nrun block\n"},
  };
  for (const auto& scenario : scenarios) {
    INFO(scenario.first);
    const FakeAtlc fake(scenario.second, false);
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0);
    cmdatl::ScopedFd master_fd(master);
    REQUIRE(::grantpt(master) == 0);
    REQUIRE(::unlockpt(master) == 0);
    const std::string slave_name = ::ptsname(master);
    int out[2];
    REQUIRE(::pipe(out) == 0);
    std::vector<std::string> argv{CMDATL_BINARY, "--json", "strategy", "backtest", "s.toml"};
    std::vector<char*> raw;
    for (auto& arg : argv) {
      raw.push_back(const_cast<char*>(arg.c_str()));
    }
    raw.push_back(nullptr);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      // Only async-signal-safe calls between fork and exec: a session of its own, the terminal as its controlling
      // terminal, which makes cmdatl's group the foreground one.
      ::setsid();
      const int slave = ::open(slave_name.c_str(), O_RDWR);
      if (slave < 0) {
        ::_exit(126);
      }
      ::ioctl(slave, TIOCSCTTY, 0);
      termios modes{};
      ::tcgetattr(slave, &modes);
      modes.c_lflag |= ISIG;
      modes.c_cc[VINTR] = 3;
      ::tcsetattr(slave, TCSANOW, &modes);
      ::dup2(slave, STDIN_FILENO);
      ::dup2(out[1], STDOUT_FILENO);
      ::dup2(slave, STDERR_FILENO);
      ::close(slave);
      ::close(out[0]);
      ::close(out[1]);
      ::close(master);
      ::execv(raw[0], raw.data());
      ::_exit(127);
    }
    ::close(out[1]);
    cmdatl::ScopedFd out_fd(out[0]);
    REQUIRE(fake.wait_for("waiting"));
    const char interrupt = 3;
    REQUIRE(::write(master, &interrupt, 1) == 1);
    std::string reply;
    int status = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (true) {
      char chunk[4096];
      pollfd fds[2] = {{out_fd.get(), POLLIN, 0}, {master, POLLIN, 0}};
      if (::poll(fds, 2, 20) > 0) {
        if (fds[0].revents != 0) {
          const auto n = ::read(out_fd.get(), chunk, sizeof(chunk));
          if (n > 0) {
            reply.append(chunk, static_cast<std::size_t>(n));
          }
        }
        if (fds[1].revents != 0) {
          // Drained and dropped; a bare `(void)` does not silence GCC's unused-result warning.
          [[maybe_unused]] const auto drained = ::read(master, chunk, sizeof(chunk));
        }
      }
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        break;
      }
      if (std::chrono::steady_clock::now() > until) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        FAIL("cmdatl did not end after Ctrl-C");
      }
    }
    char chunk[4096];
    for (ssize_t n = 0; (n = ::read(out_fd.get(), chunk, sizeof(chunk))) > 0;) {
      reply.append(chunk, static_cast<std::size_t>(n));
    }
    check_interrupted({status, reply, "", 0});
    CHECK(all_gone(fake.pids()));
  }
}

TEST_CASE("a reader closing cmdatl's stdout mid-run ends cmdatl without SIGPIPE and its group with it") {
  const Watchdog watchdog("the closed pipe", 60);
  const FakeAtlc fake("spec out " + SPEC_OK + "\nrun grandchild hold\nrun pid\nrun touch running\nrun forever\n");
  Spawned cmdatl({"--format", "jsonl", "strategy", "backtest", "s.toml"});
  const auto first = cmdatl.line(false);
  REQUIRE(first);
  CHECK(reply_of(*first).at("event").as_string() == "progress");
  REQUIRE(fake.wait_for("running"));
  cmdatl.close_stdout();
  const int status = cmdatl.wait();
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 1);
  CHECK(cmdatl.err().find("its reply could not be written") != std::string::npos);
  CHECK(all_gone(fake.pids()));
}
#endif
