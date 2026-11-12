// The fault tests: their own executable, built with CMDATL_FAULT_HOOKS in its only translation unit, which defines
// cmdatl::sys below.
// So no translation unit sees both the inline system calls and these stand-ins.

#include "./mock-desk-support.hpp"
#include "./order-support.hpp"

#include <cmdatl/cmdatl>
#include <cmdatl/system/syscalls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if !defined(CMDATL_FAULT_HOOKS)
#error "test-faults is built with CMDATL_FAULT_HOOKS"
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fault {

/** @brief One injected failure: which call, on which file, how, after how many matching calls, how many times. */
struct Plan {
  /** @brief `read`, `write`, `fsync`, `ftruncate`, `rename` or `fstat`; empty for none. */
  std::string call_;
  /** @brief The file's name the call is on (its last component); empty for any. */
  std::string file_;
  /**
   * @brief For `read`: `prefix`; for `write`: `short`, `zero`, `fail`, `eintr`; for `fstat`: `fail` or `replace-lock`
   *   (the call succeeds after another file is renamed onto the `lock` beside its file); otherwise `fail`.
   */
  std::string mode_{"fail"};
  /** @brief Matching calls let through first. */
  int skip_{0};
  /** @brief Matching calls faulted after those, or -1 for every one. */
  int times_{1};
  /** @brief The error number of a failure. */
  int error_{EIO};
};

/** @brief The plan in force. */
inline Plan plan;
/** @brief The kill point that raises SIGKILL, or empty. */
inline std::string kill_at;
/** @brief What a kill point does besides killing: the test's hook, called with the point's name. */
inline std::function<void(const std::string&)> hook;
/** @brief Whether the next `fsync` of the journal fails (Linux, through `--wrap=fsync`). */
inline bool fail_journal_fsync = false;
/** @brief How many `fsync`s of the mock market's state file succeed before every later one fails; -1 for no failure. */
inline int state_fsyncs_left = -1;
/** @brief What runs right after a `replace-lock` fault replaced the lock: another process taking the account. */
inline std::function<void()> after_replace;
/** @brief How many matching calls were seen. */
inline int seen = 0;
/** @brief How many faults were injected. */
inline int injected = 0;

/**
 * @brief The file a descriptor is open on.
 * @param fd The descriptor.
 * @return The path, or empty.
 */
inline std::filesystem::path path_of(int fd) {
#if defined(__APPLE__)
  char path[MAXPATHLEN];
  if (::fcntl(fd, F_GETPATH, path) != 0) {
    return {};
  }
  return path;
#else
  char path[4096];
  const auto n = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path, sizeof(path) - 1);
  if (n <= 0) {
    return {};
  }
  path[n] = '\0';
  return path;
#endif
}

/**
 * @brief The last component of the file a descriptor is open on.
 * @param fd The descriptor.
 * @return The name, or empty.
 */
inline std::string name_of(int fd) {
  return path_of(fd).filename().string();
}

/**
 * @brief Replaces the `lock` beside a descriptor's file with a new file, as another process taking the account would.
 * @param fd The descriptor.
 */
inline void replace_lock_beside(int fd) {
  const auto dir = path_of(fd).parent_path();
  const auto fresh = dir / "lock.replacement";
  {
    std::ofstream out(fresh);
  }
  std::filesystem::rename(fresh, dir / "lock");
}

/**
 * @brief Whether this call is to be faulted.
 * @param call The call.
 * @param file The file's name.
 * @return Whether it is.
 */
inline bool hit(const char* call, const std::string& file) {
  if (plan.call_ != call || (!plan.file_.empty() && file != plan.file_)) {
    return false;
  }
  const int index = seen++;
  if (index < plan.skip_ || (plan.times_ >= 0 && index >= plan.skip_ + plan.times_)) {
    return false;
  }
  ++injected;
  return true;
}

/** @brief Clears the plan. */
inline void clear() {
  plan = Plan{};
  kill_at.clear();
  hook = nullptr;
  after_replace = nullptr;
  fail_journal_fsync = false;
  state_fsyncs_left = -1;
  seen = 0;
  injected = 0;
}

/** @brief Sets a plan for its owner's life. */
class Scoped {
public:
  /**
   * @brief Sets the plan.
   * @param p The plan.
   */
  explicit Scoped(Plan p) {
    clear();
    plan = std::move(p);
  }

  Scoped(const Scoped&) = delete;
  Scoped& operator=(const Scoped&) = delete;

  /** @brief Clears it. */
  ~Scoped() {
    clear();
  }
};

} // namespace fault

namespace cmdatl {
namespace sys {

ssize_t read(int fd, void* buffer, std::size_t size) {
  if (fault::hit("read", fault::name_of(fd))) {
    if (fault::plan.mode_ == "prefix" && fault::injected == 1) {
      // A prefix, then the failure on the next call.
      ++fault::plan.times_;
      return ::read(fd, buffer, std::min<std::size_t>(size, 7));
    }
    errno = fault::plan.error_;
    return -1;
  }
  return ::read(fd, buffer, size);
}

ssize_t write(int fd, const void* buffer, std::size_t size) {
  if (fault::hit("write", fault::name_of(fd))) {
    const auto& mode = fault::plan.mode_;
    if (mode == "short" && fault::injected == 1) {
      // Half the bytes, then the failure on the next call: a short write that cannot complete.
      ++fault::plan.times_;
      return ::write(fd, buffer, size / 2);
    }
    if (mode == "zero") {
      return 0;
    }
    errno = mode == "eintr" ? EINTR : fault::plan.error_;
    return -1;
  }
  return ::write(fd, buffer, size);
}

int fsync(int fd) {
  if (fault::hit("fsync", fault::name_of(fd))) {
    errno = fault::plan.error_;
    return -1;
  }
  return ::fsync(fd);
}

int ftruncate(int fd, off_t size) {
  if (fault::hit("ftruncate", fault::name_of(fd))) {
    errno = fault::plan.error_;
    return -1;
  }
  return ::ftruncate(fd, size);
}

int rename(const char* from, const char* to) {
  if (fault::hit("rename", std::filesystem::path(to).filename().string())) {
    errno = fault::plan.error_;
    return -1;
  }
  return std::rename(from, to);
}

int fstat(int fd, struct stat* info) {
  if (fault::hit("fstat", fault::name_of(fd))) {
    if (fault::plan.mode_ == "replace-lock") {
      fault::replace_lock_beside(fd);
      if (fault::after_replace) {
        fault::after_replace();
      }
      return ::fstat(fd, info);
    }
    errno = fault::plan.error_;
    return -1;
  }
  return ::fstat(fd, info);
}

void kill_point(const char* name) {
  if (!fault::kill_at.empty() && fault::kill_at == name) {
    std::raise(SIGKILL);
  }
  if (fault::hook) {
    fault::hook(name);
  }
}

} // namespace sys
} // namespace cmdatl

#if defined(CMDATL_WRAP_FSYNC)
extern "C" int __real_fsync(int fd);

/**
 * @brief The linker's stand-in for `fsync`, the library's included: fails the journal's next one when asked.
 * @param fd The descriptor.
 * @return As `fsync`.
 */
extern "C" int __wrap_fsync(int fd) {
  if (fault::fail_journal_fsync && fault::name_of(fd) == "journal.jsonl") {
    fault::fail_journal_fsync = false;
    errno = EIO;
    return -1;
  }
  if (fault::state_fsyncs_left >= 0 && fault::name_of(fd) == cmdatl::MOCK_STATE_FILE) {
    if (fault::state_fsyncs_left == 0) {
      errno = EIO;
      return -1;
    }
    --fault::state_fsyncs_left;
  }
  return __real_fsync(fd);
}
#else
/**
 * @brief The C library's `fsync`, replaced in this executable only: fails the mock market's state file while asked.
 * @param fd The descriptor.
 * @return As `fsync`.
 */
extern "C" int fsync(int fd) {
  if (fault::state_fsyncs_left >= 0 && fault::name_of(fd) == cmdatl::MOCK_STATE_FILE) {
    if (fault::state_fsyncs_left == 0) {
      errno = EIO;
      return -1;
    }
    --fault::state_fsyncs_left;
  }
  using Sync = int (*)(int);
  static const auto real = reinterpret_cast<Sync>(::dlsym(RTLD_NEXT, "fsync"));
  if (real == nullptr) {
    std::fputs("test-faults: the C library's fsync was not found\n", stderr);
    std::abort();
  }
  return real(fd);
}
#endif

using namespace support;

namespace {

/**
 * @brief Runs a command line in a forked child that a kill point may end, and waits for it.
 * @param args The command line.
 * @param point The kill point.
 * @return The child's wait status.
 */
int run_killed(const std::vector<std::string>& args, const std::string& point) {
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    fault::clear();
    fault::kill_at = point;
    std::ostringstream out;
    std::ostringstream err;
    const int exit = cmdatl::run(args, out, err, false);
    ::_exit(exit);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return status;
}

/**
 * @brief Runs a command and returns its exit code and error code.
 * @param fixture The account.
 * @param args The command line.
 * @return The exit code and the error code (empty on success).
 */
std::pair<int, std::string> outcome(const PaperFixture& fixture, const std::vector<std::string>& args) {
  const auto ran = fixture.json(args);
  INFO(ran.out_ << ran.err_);
  const auto reply = reply_of(ran.out_);
  const auto* error = reply.if_contains("error");
  return {ran.exit_, error == nullptr ? "" : std::string(error->as_object().at("code").as_string())};
}

/**
 * @brief The account's clock as `clock` reports it.
 * @param fixture The account.
 * @return The instant.
 */
std::string now_of(const PaperFixture& fixture) {
  return std::string(reply_of(fixture.json({"clock"}).out_).at("now").as_string());
}

} // namespace

namespace {

/** @brief The file-size limit at the state file's size, and SIGXFSZ ignored, while it lives. */
class StateFull {
public:
  /**
   * @brief Sets the limit.
   * @param file The state file.
   */
  explicit StateFull(const std::filesystem::path& file) : previous_(std::signal(SIGXFSZ, SIG_IGN)) {
    ::getrlimit(RLIMIT_FSIZE, &saved_);
    struct stat info{};
    REQUIRE(::stat(file.c_str(), &info) == 0);
    auto limit = saved_;
    limit.rlim_cur = static_cast<rlim_t>(info.st_size);
    ::setrlimit(RLIMIT_FSIZE, &limit);
  }

  StateFull(const StateFull&) = delete;
  StateFull& operator=(const StateFull&) = delete;

  /** @brief Lifts the limit. */
  ~StateFull() {
    ::setrlimit(RLIMIT_FSIZE, &saved_);
    std::signal(SIGXFSZ, previous_);
  }

private:
  struct rlimit saved_{};
  void (*previous_)(int);
};

} // namespace

TEST_CASE("an advance the state file does not take whole leaves the clock where it was") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto log = slurp(fixture.log());
  {
    const StateFull full(fixture.log());
    const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
    CHECK(result.first == 1);
    CHECK(result.second == "file.unwritable");
  }
  CHECK(now_of(fixture) == "2024-03-25T16:00:00+09:00");
  CHECK(slurp(fixture.log()) == log);
  CHECK(outcome(fixture, {"clock", "advance", "--minutes", "5"}).first == 0);
  CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
}

TEST_CASE("a whole advance whose flush failed is unknown, and the next command sees it once") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  {
    // The first flush is the open's; the advance's is the second.
    const fault::Scoped scoped({});
    fault::state_fsyncs_left = 1;
    const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
    CHECK(result.first == 4);
    CHECK(result.second == "unknown.clock_advance");
    CHECK(declares({"clock", "advance"}, 4, "unknown.clock_advance"));
  }
  CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
  CHECK(ops_of(slurp(fixture.log()), "advance").size() == 1);
}

TEST_CASE("the owner record's rename failing is ignored, and its temporary removed") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto owner = slurp(fixture.account() / "owner.json");
  {
    const fault::Scoped scoped({"rename", "owner.json", "fail", 0, 1, EIO});
    CHECK(outcome(fixture, {"account"}).first == 0);
    CHECK(fault::injected == 1);
  }
  CHECK(slurp(fixture.account() / "owner.json") == owner);
  for (const auto& entry : std::filesystem::directory_iterator(fixture.account())) {
    CHECK(entry.path().extension() != ".tmp");
  }
}

TEST_CASE("the lock lost before the owner record is written: no record replaced, and lock_lost") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto owner = fixture.account() / "owner.json";
  const std::string record = "{\"role\":\"operator\",\"pid\":1}\n";
  spit(owner, record);
  const auto log = slurp(fixture.log());
  {
    // Replaced while the account opens, after the lock was taken and before anything is written.
    const fault::Scoped scoped({"fstat", "market.jsonl", "replace-lock", 0, -1, 0});
    const auto result = outcome(fixture, {"status"});
    CHECK(fault::injected > 0);
    CHECK(result.first == 4);
    CHECK(result.second == "unknown.lock_lost");
  }
  CHECK(slurp(owner) == record);
  CHECK(slurp(fixture.log()) == log);
  for (const auto& entry : std::filesystem::directory_iterator(fixture.account())) {
    CHECK(entry.path().extension() != ".tmp");
  }
  // The new lock is free: the next command takes it and writes its own record.
  CHECK(outcome(fixture, {"status"}).first == 0);
  CHECK(slurp(owner) != record);
}

TEST_CASE("a failed status read refuses before anything destructive") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto torn = slurp(fixture.log()) + "{\"at\":";
  spit(fixture.log(), torn);
  const auto journal = slurp(fixture.journal());
  const std::vector<std::pair<std::string, std::string>> cases{
    {"market.jsonl", "unknown.paper_state"},
    {"journal.jsonl", "unknown.journal_damaged"},
    {"lock", "state.unavailable"},
    {"paper.toml", "config.invalid"},
    {"bars.csv", "paper.data"},
  };
  for (const auto& entry : cases) {
    INFO(entry.first);
    {
      const fault::Scoped scoped({"fstat", entry.first, "fail", 0, 1, EIO});
      const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
      CHECK(result.second == entry.second);
      CHECK(fault::injected == 1);
    }
    CHECK(slurp(fixture.log()) == torn);
    CHECK(slurp(fixture.journal()) == journal);
  }
  CHECK(outcome(fixture, {"status"}).first == 0);
}

TEST_CASE("an initialization stopped after the journal was created, or partway through its header, fails closed") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto header = slurp(fixture.journal());
  const auto log = slurp(fixture.log());
  std::filesystem::remove(fixture.account() / "owner.json");
  // Created, empty.
  spit(fixture.journal(), "");
  auto result = outcome(fixture, {"status"});
  CHECK(result.first == 4);
  CHECK(result.second == "unknown.journal_missing");
  CHECK(slurp(fixture.journal()).empty());
  // Partway through its header.
  spit(fixture.journal(), header.substr(0, header.size() / 2));
  result = outcome(fixture, {"status"});
  CHECK(result.first == 4);
  CHECK(result.second == "unknown.journal_damaged");
  CHECK(slurp(fixture.journal()) == header.substr(0, header.size() / 2));
  CHECK(slurp(fixture.log()) == log);
}

namespace {

/** @brief What a forked command left: its wait status and its reply. */
struct Forked {
  /** @brief The wait status. */
  int status_{0};
  /** @brief What it wrote to stdout, or empty when it was killed. */
  std::string out_;

  /**
   * @brief The exit code, or -1 when it did not exit.
   * @return The code.
   */
  int exit() const {
    return WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }

  /**
   * @brief The error code of its reply.
   * @return The code, or empty.
   */
  std::string code() const {
    if (out_.empty()) {
      return {};
    }
    const auto reply = reply_of(out_);
    const auto* error = reply.if_contains("error");
    return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
  }
};

/**
 * @brief Runs a command line in a forked child with a hook at the kill points, and waits for it.
 * @param fixture The account (for a scratch file the reply goes through).
 * @param args The command line, without `--json`.
 * @param hook The hook, called with each kill point's name in the child.
 * @param kill_at A kill point that raises SIGKILL, or empty.
 * @return What it left.
 */
Forked run_forked(
  const PaperFixture& fixture, std::vector<std::string> args, const std::function<void(const std::string&)>& hook,
  const std::string& kill_at = ""
) {
  args.insert(args.begin(), "--json");
  const auto out_path = fixture.dir() / "forked.out";
  std::filesystem::remove(out_path);
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    fault::clear();
    fault::kill_at = kill_at;
    fault::hook = hook;
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit saved{};
    ::getrlimit(RLIMIT_FSIZE, &saved);
    std::ostringstream out;
    std::ostringstream err;
    const int exit = cmdatl::run(args, out, err, false);
    // A hook may have limited the size of every file this process writes: lifted for the reply's own file.
    ::setrlimit(RLIMIT_FSIZE, &saved);
    const auto text = out.str() + err.str();
    const int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      (void)!::write(fd, text.data(), text.size());
      ::close(fd);
    }
    ::_exit(exit);
  }
  Forked forked;
  REQUIRE(::waitpid(pid, &forked.status_, 0) == pid);
  if (std::filesystem::exists(out_path)) {
    forked.out_ = slurp(out_path);
  }
  return forked;
}

/**
 * @brief A hook that sets the file-size limit at one kill point, so the next append to a file writes nothing or only
 *   a prefix and fails with `EFBIG`.
 *
 * Linux checks the limit against where an appending write lands, the file's end; macOS checks it against the
 * descriptor's offset, which is 0 for a file opened for appending after the hook and nothing written through it yet.
 * @param point The kill point.
 * @param file The file.
 * @param extra Bytes past the file's end (or past offset 0, for a descriptor not yet written) the limit allows.
 * @param fresh Whether the descriptor the append goes through is opened after the hook.
 * @return The hook.
 */
std::function<void(const std::string&)> limit_at(
  const std::string& point, const std::filesystem::path& file, off_t extra, bool fresh
) {
  return [point, file, extra, fresh](const std::string& name) {
    if (name != point) {
      return;
    }
    struct stat info{};
    REQUIRE(::stat(file.c_str(), &info) == 0);
#if defined(__APPLE__)
    const off_t base = fresh ? 0 : info.st_size;
#else
    (void)fresh;
    const off_t base = info.st_size;
#endif
    struct rlimit limit{};
    ::getrlimit(RLIMIT_FSIZE, &limit);
    limit.rlim_cur = static_cast<rlim_t>(base + extra);
    ::setrlimit(RLIMIT_FSIZE, &limit);
  };
}

/**
 * @brief Moves an account to the first session's open, armed, and returns a plan id for 10 shares of 005930 at a limit
 *   below the day's prices (so it rests).
 * @param fixture The account.
 * @param limit The limit.
 * @return The plan id.
 */
std::string ready_plan(const PaperFixture& fixture, const std::string& limit = "75000") {
  fixture.arm();
  REQUIRE(outcome(fixture, {"clock", "advance", "--next-open"}).first == 0);
  return plan_id(fixture, "005930", "10", limit);
}

} // namespace

TEST_CASE(
  "a place killed after its plan was marked used: plan_used and nothing journaled or logged and a new plan works"
) {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  const auto journal = slurp(fixture.journal());
  const auto forked = run_forked(fixture, {"order", "place", id}, nullptr, "order.before_once");
  REQUIRE(WIFSIGNALED(forked.status_));
  CHECK(slurp(fixture.journal()) == journal);
  CHECK(ops_of(slurp(fixture.log()), "place").empty());
  CHECK(ask_checked(fixture, {"order", "place", id}).code_ == "refused.plan_used");
  const auto next = plan_id(fixture, "005930", "10", "75000");
  CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
}

TEST_CASE(
  "a place killed after its PENDING and before it was forwarded: owed unnamed and never a second order and release"
) {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  const auto forked = run_forked(fixture, {"order", "place", id}, nullptr, "send.before_forward");
  REQUIRE(WIFSIGNALED(forked.status_));
  CHECK(ops_of(slurp(fixture.log()), "place").empty());
  for (int round = 0; round < 2; ++round) {
    const auto account = ask(fixture, {"account"});
    const auto& owed = account.body_.at("owed").as_array();
    REQUIRE(owed.size() == 1);
    CHECK(owed[0].as_object().at("state").as_string() == "unnamed");
    CHECK(owed[0].as_object().at("broker_status").is_null());
    const auto plan = ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"});
    CHECK(plan.exit_ == 4);
    CHECK((plan.code_ == "unknown.unresolved" || plan.code_ == "unknown.unaccounted"));
    CHECK(ask_checked(fixture, {"order", "cancel", "005930"}).exit_ == 4);
    CHECK(ask_checked(fixture, {"order", "reconcile"}).exit_ == 4);
    CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read"}).code_ == "refused.not_approved");
  }
  CHECK(ops_of(slurp(fixture.log()), "place").empty());
  const auto journal_id =
    std::string(ask(fixture, {"account"}).body_.at("owed").as_array()[0].as_object().at("journal_id").as_string());
  spit(fixture.dir() / "paper.release", "005930 " + journal_id + "\n");
  CHECK(
    ask_checked(fixture, {"order", "release", "005930", "--reason", "read the order list and holdings"}).reason_ ==
    "released"
  );
  CHECK(ask_checked(fixture, {"order", "reconcile"}).reason_ == "clean");
  const auto next = plan_id(fixture, "005930", "10", "75000");
  CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
}

TEST_CASE("a place killed after the market answered: the market has it and the next commands name it") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  const auto forked = run_forked(fixture, {"order", "place", id}, nullptr, "send.after_result");
  REQUIRE(WIFSIGNALED(forked.status_));
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
  const auto account = ask(fixture, {"account"});
  REQUIRE(account.body_.at("owed").as_array().size() == 1);
  // A limit away from the next prices keeps it listed: the plan finds the target met by it.
  const auto plan = ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"});
  INFO(boost::json::serialize(plan.body_));
  CHECK(plan.exit_ == 0);
  CHECK(plan.reason_ == "met");
  const auto reconciled = ask_checked(fixture, {"order", "reconcile"});
  CHECK(reconciled.code_ == "refused.awaiting");
  CHECK(reconciled.body_.at("checked").as_array()[0].as_object().at("state").as_string() == "working");
  REQUIRE(ask(fixture, {"order", "cancel", "005930"}).reason_ == "withdrawal_sent");
  CHECK(ask_checked(fixture, {"order", "reconcile"}).reason_ == "clean");
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
}

TEST_CASE("a place killed after once() returned: owed and named and pending") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  const auto forked = run_forked(fixture, {"order", "place", id}, nullptr, "order.after_once");
  REQUIRE(WIFSIGNALED(forked.status_));
  const auto owed = ask(fixture, {"account"}).body_.at("owed").as_array();
  REQUIRE(owed.size() == 1);
  CHECK(owed[0].as_object().at("state").as_string() == "named");
  CHECK(owed[0].as_object().at("broker_status").as_string() == "pending");
}

TEST_CASE("an exception after a send and inside once() or after it and is unknown.internal") {
  for (const auto* point : {"send.after_result", "order.after_once"}) {
    INFO(point);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const std::string at = point;
    const auto forked = run_forked(fixture, {"order", "place", id}, [at](const std::string& name) {
      if (name == at) {
        throw std::runtime_error("a defect at " + at);
      }
    });
    CHECK(forked.exit() == 4);
    CHECK(forked.code() == "unknown.internal");
    CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().size() == 1);
  }
}

TEST_CASE("the armed file removed before once() or before the boundary forwards sends nothing") {
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto armed = fixture.armed();
    const auto forked = run_forked(fixture, {"order", "place", id}, [armed](const std::string& name) {
      if (name == "order.before_once") {
        std::filesystem::remove(armed);
      }
    });
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.not_armed");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
  }
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto armed = fixture.armed();
    const auto forked = run_forked(fixture, {"order", "place", id}, [armed](const std::string& name) {
      if (name == "send.before_forward") {
        std::filesystem::remove(armed);
      }
    });
    INFO(forked.out_);
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.not_reached");
    // The boundary's own refusal.
    const auto message = std::string(reply_of(forked.out_).at("error").as_object().at("message").as_string());
    CHECK(message.find("not_armed") != std::string::npos);
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    // The journal answered the PENDING (a local refusal is journaled as REJECTED): nothing is owed.
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().empty());
    fixture.arm();
    const auto next = plan_id(fixture, "005930", "10", "75000");
    CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
  }
}

TEST_CASE("the journal moved aside or emptied or replaced by a copy before once(): journal_missing and none created") {
  for (const auto* how : {"moved", "emptied", "copied"}) {
    INFO(how);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto journal = fixture.journal();
    const std::string mode = how;
    const auto aside = fixture.dir() / "journal.aside";
    const auto forked = run_forked(fixture, {"order", "place", id}, [journal, mode, aside](const std::string& name) {
      if (name != "order.before_once") {
        return;
      }
      if (mode == "moved") {
        std::filesystem::rename(journal, aside);
      } else if (mode == "emptied") {
        std::filesystem::resize_file(journal, 0);
      } else {
        std::filesystem::copy_file(journal, aside);
        std::filesystem::rename(aside, journal);
        std::filesystem::copy_file(journal, aside);
      }
    });
    CHECK(forked.exit() == 4);
    CHECK(forked.code() == "unknown.journal_missing");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    if (mode == "moved") {
      CHECK(!std::filesystem::exists(journal));
    } else if (mode == "emptied") {
      CHECK(std::filesystem::file_size(journal) == 0);
    } else {
      CHECK(slurp(journal) == slurp(aside));
    }
  }
}

namespace {

/**
 * @brief A hook that fails the state file's writes, or its flushes, only while the boundary forwards a send.
 * @param file The state file.
 * @param flush Whether the flush fails rather than the write.
 * @return The hook.
 */
std::function<void(const std::string&)> state_fails_at_send(const std::filesystem::path& file, bool flush) {
  auto saved = std::make_shared<struct rlimit>();
  return [file, flush, saved](const std::string& name) {
    if (name == "send.before_forward") {
      if (flush) {
        fault::state_fsyncs_left = 0;
        return;
      }
      struct stat info{};
      REQUIRE(::stat(file.c_str(), &info) == 0);
      ::getrlimit(RLIMIT_FSIZE, saved.get());
      auto limit = *saved;
      limit.rlim_cur = static_cast<rlim_t>(info.st_size);
      ::setrlimit(RLIMIT_FSIZE, &limit);
    } else if (name == "send.after_result") {
      fault::state_fsyncs_left = -1;
      if (!flush) {
        ::setrlimit(RLIMIT_FSIZE, saved.get());
      }
    }
  };
}

} // namespace

TEST_CASE("a send the state file does not take whole is refused.not_reached and nothing is sent") {
  for (const auto* command : {"place", "cancel"}) {
    INFO(command);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const bool cancel = std::string(command) == "cancel";
    if (cancel) {
      REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
    }
    const auto before = slurp(fixture.log());
    const auto args =
      cancel ? std::vector<std::string>{"order", "cancel", "005930"} : std::vector<std::string>{"order", "place", id};
    const auto forked = run_forked(fixture, args, state_fails_at_send(fixture.log(), false));
    INFO(forked.out_);
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.not_reached");
    CHECK(ops_of(slurp(fixture.log()), "place").size() == ops_of(before, "place").size());
    CHECK(ops_of(slurp(fixture.log()), "cancel").empty());
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().size() == (cancel ? 1U : 0U));
    CHECK(ask(fixture, {"order", "list"}).body_.at("orders").as_array().size() == (cancel ? 1U : 0U));
  }
}

TEST_CASE("a send whose state file flush is not confirmed is unknown.indeterminate and the reopened market has it") {
  for (const auto* command : {"place", "cancel"}) {
    INFO(command);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const bool cancel = std::string(command) == "cancel";
    if (cancel) {
      REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
    }
    const auto args =
      cancel ? std::vector<std::string>{"order", "cancel", "005930"} : std::vector<std::string>{"order", "place", id};
    const auto forked = run_forked(fixture, args, state_fails_at_send(fixture.log(), true));
    INFO(forked.out_);
    CHECK(forked.exit() == 4);
    CHECK(forked.code() == "unknown.indeterminate");
    // The line survived: the replay applies it.
    CHECK(ops_of(slurp(fixture.log()), cancel ? "cancel" : "place").size() == 1);
    CHECK(ask(fixture, {"order", "list"}).body_.at("orders").as_array().size() == (cancel ? 0U : 1U));
    CHECK(ask_checked(fixture, {"order", "reconcile"}).exit_ != 0);
  }
}

TEST_CASE("a journal failing after a forwarded send is unknown.journal_write for place and cancel") {
  for (const auto* command : {"place", "cancel"}) {
    INFO(command);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const bool cancel = std::string(command) == "cancel";
    if (cancel) {
      REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
    }
    const auto args =
      cancel ? std::vector<std::string>{"order", "cancel", "005930"} : std::vector<std::string>{"order", "place", id};
    const auto forked = run_forked(fixture, args, limit_at("send.after_result", fixture.journal(), 0, false));
    INFO(forked.out_);
    CHECK(forked.exit() == 4);
    CHECK(forked.code() == "unknown.journal_write");
    CHECK(ops_of(slurp(fixture.log()), cancel ? "cancel" : "place").size() == 1);
  }
}

TEST_CASE("the journal's PENDING failing at zero bytes or as a prefix: refused.journal_unwritable and nothing sent") {
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto journal = slurp(fixture.journal());
    const auto forked =
      run_forked(fixture, {"order", "place", id}, limit_at("order.before_once", fixture.journal(), 0, true));
    INFO(forked.out_);
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.journal_unwritable");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    CHECK(slurp(fixture.journal()) == journal);
    // Nothing journaled: a new plan works.
    const auto next = plan_id(fixture, "005930", "10", "75000");
    CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
  }
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto forked =
      run_forked(fixture, {"order", "place", id}, limit_at("order.before_once", fixture.journal(), 20, true));
    INFO(forked.out_);
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.journal_unwritable");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    // The torn line: every command refuses, nothing is repaired.
    const auto torn = slurp(fixture.journal());
    for (int round = 0; round < 2; ++round) {
      CHECK(ask_checked(fixture, {"account"}).code_ == "unknown.journal_damaged");
      CHECK(
        ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "75000"}).code_ ==
        "unknown.journal_damaged"
      );
    }
    CHECK(slurp(fixture.journal()) == torn);
  }
#if defined(CMDATL_WRAP_FSYNC)
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto forked = run_forked(fixture, {"order", "place", id}, [](const std::string& name) {
      if (name == "order.before_once") {
        fault::fail_journal_fsync = true;
      }
    });
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.journal_unwritable");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    // The PENDING is whole and unanswered: owed until the operator releases it.
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().size() == 1);
  }
#endif
}

TEST_CASE("the journal's answer failing after the market answered: unknown.journal_write") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  const auto forked =
    run_forked(fixture, {"order", "place", id}, limit_at("send.after_result", fixture.journal(), 0, false));
  INFO(forked.out_);
  CHECK(forked.exit() == 4);
  CHECK(forked.code() == "unknown.journal_write");
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
  // The PENDING is unanswered and the order rests: owed, and the next plan finds it.
  const auto owed = ask(fixture, {"account"}).body_.at("owed").as_array();
  REQUIRE(owed.size() == 1);
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"}).reason_ == "met");
}

TEST_CASE("a cancel killed after the market answered: canceled and unresolved until the match window closes") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
  const auto forked = run_forked(fixture, {"order", "cancel", "005930"}, nullptr, "send.after_result");
  REQUIRE(WIFSIGNALED(forked.status_));
  CHECK(ops_of(slurp(fixture.log()), "cancel").size() == 1);
  CHECK(ask(fixture, {"order", "list"}).body_.at("orders").as_array().empty());
  const auto plan = ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"});
  INFO(boost::json::serialize(plan.body_));
  CHECK(plan.exit_ == 4);
  REQUIRE(ask(fixture, {"clock", "advance", "--minutes", "3"}).exit_ == 0);
  const auto reconciled = ask_checked(fixture, {"order", "reconcile"});
  INFO(boost::json::serialize(reconciled.body_));
  CHECK(reconciled.reason_ == "clean");
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
}

TEST_CASE("an interrupt before once() is 130 and sends nothing; after once() is called and the call's own reply") {
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto forked = run_forked(fixture, {"order", "place", id}, [](const std::string& name) {
      if (name == "order.before_once") {
        cmdatl::interruption = 1;
      }
    });
    CHECK(forked.exit() == 130);
    CHECK(forked.code() == "interrupted");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().empty());
  }
  for (const auto* command : {"place", "cancel"}) {
    INFO(command);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    if (std::string(command) == "cancel") {
      REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
    }
    const auto args = std::string(command) == "place" ?
      std::vector<std::string>{"order", "place", id} :
      std::vector<std::string>{"order", "cancel", "005930"};
    const auto forked = run_forked(fixture, args, [](const std::string& name) {
      if (name == "send.after_result") {
        cmdatl::interruption = 1;
      }
    });
    INFO(forked.out_);
    CHECK(forked.exit() == 0);
    CHECK(forked.code().empty());
  }
}

TEST_CASE("the plan book's rename failing: exit 1 and the plan unused and nothing sent") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  {
    const fault::Scoped scoped({"rename", "plans.json", "fail", 0, 1, EIO});
    const auto result = ask(fixture, {"order", "place", id});
    CHECK(result.exit_ == 1);
    CHECK(result.code_ == "file.unwritable");
  }
  for (const auto& entry : std::filesystem::directory_iterator(fixture.account())) {
    CHECK(entry.path().extension() != ".tmp");
  }
  CHECK(ops_of(slurp(fixture.log()), "place").empty());
  // Unused: the same plan places now (the clock moved two seconds; the plan is still fresh).
  CHECK(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
}

TEST_CASE("the per-call second not written whole or not flushed is exit 1 with nothing sent") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  {
    const StateFull full(fixture.log());
    const auto result = ask(fixture, {"order", "place", id});
    CHECK(result.exit_ == 1);
    CHECK(result.code_ == "file.unwritable");
  }
  {
    // The first flush is the open's; the second is the command's second.
    const fault::Scoped scoped({});
    fault::state_fsyncs_left = 1;
    const auto result = ask(fixture, {"order", "place", id});
    CHECK(result.exit_ == 1);
    CHECK(result.code_ == "file.unwritable");
  }
  CHECK(ops_of(slurp(fixture.log()), "place").empty());
}

TEST_CASE("a former lock holder never publishes its plan book over a newer holder's") {
  for (const auto* command : {"place", "plan"}) {
    for (const auto* when : {"before the save", "between the temporary and its publication"}) {
      INFO("order " << command << ", the lock replaced " << when);
      const PaperFixture fixture;
      const auto id = ready_plan(fixture);
      const auto book_path = fixture.account() / "plans.json";
      const auto former_book = slurp(book_path);
      // The newer holder's book, made beforehand: while the former holder runs, its market's state file stays locked,
      // so the newer holder only publishes its book.
      const auto new_id = plan_id(fixture, "000660", "1", "180000");
      const auto newer_book = slurp(book_path);
      spit(book_path, former_book);
      const auto journal_before = slurp(fixture.journal());
      const auto newer_holder = [&] {
        spit(book_path, newer_book);
      };
      const auto line = std::string(command) == "place" ?
        std::vector<std::string>{"order", "place", id} :
        std::vector<std::string>{"order", "plan", "005930", "--target", "20", "--limit", "75000"};
      Answer answer;
      if (std::string(when) == "before the save") {
        // The book's third status read comes after it was read, before it is written.
        const fault::Scoped scoped({"fstat", "plans.json", "replace-lock", 2, 1, 0});
        fault::after_replace = newer_holder;
        answer = ask_checked(fixture, line);
        REQUIRE(fault::injected == 1);
      } else {
        const fault::Scoped scoped({});
        bool fired = false;
        fault::hook = [&](const std::string& name) {
          if (name != "plans.written" || fired) {
            return;
          }
          fired = true;
          const auto fresh = fixture.account() / "lock.replacement";
          spit(fresh, "");
          std::filesystem::rename(fresh, fixture.account() / "lock");
          newer_holder();
        };
        answer = ask_checked(fixture, line);
        REQUIRE(fired);
      }
      CHECK(answer.exit_ == 4);
      CHECK(answer.code_ == "unknown.lock_lost");
      CHECK(slurp(fixture.journal()) == journal_before);
      CHECK(ops_of(slurp(fixture.log()), "place").empty());
      const auto book = cmdatl::PlanBook::load(fixture.account().string());
      CHECK(!book.damaged());
      CHECK(book.find(new_id).has_value());
      REQUIRE(book.find(id).has_value());
      CHECK(!book.find(id)->used_);
      for (const auto& entry : std::filesystem::directory_iterator(fixture.account())) {
        CHECK(entry.path().extension() != ".tmp");
      }
    }
  }
}

TEST_CASE("a mock desk whose state file takes no whole line changes nothing and counts no send") {
  const PaperFixture fixture;
  fixture.arm();
  const HeldAccount held(fixture);
  {
    auto desk = held.desk();
    REQUIRE(desk->advance_to(kst("2024-03-26T10:00")) == cmdatl::Moved::OK);
    REQUIRE(desk->session().place(order("005930", true, "1", "78500")));
  }
  const auto before = slurp(state_file(fixture));
  auto desk = held.desk();
  SECTION("a placement") {
    const StateFull full(state_file(fixture));
    const auto placed = desk->session().place(order("005930", true, "1", "78500"));
    REQUIRE_FALSE(placed);
    CHECK(placed.error().broker_code_ == "state_not_written");
    CHECK_FALSE(placed.error().reached_broker_);
  }
  SECTION("a cancellation") {
    algotradelib::AmendInstruction whole;
    whole.all_remaining_ = true;
    const auto ref = desk->session().working_orders().value().orders_.front().ref_;
    const StateFull full(state_file(fixture));
    const auto canceled = desk->session().cancel(ref, whole);
    REQUIRE_FALSE(canceled);
    CHECK(canceled.error().broker_code_ == "state_not_written");
    CHECK_FALSE(canceled.error().reached_broker_);
  }
  SECTION("a clock move") {
    const StateFull full(state_file(fixture));
    CHECK(desk->advance_to(kst("2024-03-26T11:00")) == cmdatl::Moved::NOT_WRITTEN);
  }
  CHECK(desk->sends() == 0);
  CHECK(desk->now() == kst("2024-03-26T10:00"));
  CHECK(desk->order_status("1") == std::optional<std::string>("pending"));
  CHECK_FALSE(desk->order_status("2"));
  // After a failed write the market takes no change until it is opened again.
  CHECK(desk->advance_to(kst("2024-03-26T11:00")) == cmdatl::Moved::NOT_WRITTEN);
  CHECK(slurp(state_file(fixture)) == before);
  desk.reset();
  auto reopened = held.desk();
  CHECK(reopened->now() == kst("2024-03-26T10:00"));
  CHECK(reopened->order_status("1") == std::optional<std::string>("pending"));
  CHECK_FALSE(reopened->order_status("2"));
}

TEST_CASE("a mock desk whose state file flush is not confirmed counts the send and a reopening makes the change") {
  const PaperFixture fixture;
  fixture.arm();
  const HeldAccount held(fixture);
  {
    auto desk = held.desk();
    REQUIRE(desk->advance_to(kst("2024-03-26T10:00")) == cmdatl::Moved::OK);
  }
  SECTION("a placement") {
    {
      auto desk = held.desk();
      const fault::Scoped scoped({});
      fault::state_fsyncs_left = 0;
      const auto placed = desk->session().place(order("005930", true, "1", "78500"));
      fault::state_fsyncs_left = -1;
      REQUIRE_FALSE(placed);
      CHECK(placed.error().broker_code_ == "state_unconfirmed");
      CHECK(placed.error().reached_broker_);
      CHECK(desk->sends() == 1);
      CHECK_FALSE(desk->order_status("1"));
    }
    auto reopened = held.desk();
    CHECK(reopened->order_status("1") == std::optional<std::string>("pending"));
  }
  SECTION("a cancellation") {
    {
      auto desk = held.desk();
      REQUIRE(desk->session().place(order("005930", true, "1", "78500")));
    }
    {
      auto desk = held.desk();
      algotradelib::AmendInstruction whole;
      whole.all_remaining_ = true;
      const auto ref = desk->session().working_orders().value().orders_.front().ref_;
      const fault::Scoped scoped({});
      fault::state_fsyncs_left = 0;
      const auto canceled = desk->session().cancel(ref, whole);
      fault::state_fsyncs_left = -1;
      REQUIRE_FALSE(canceled);
      CHECK(canceled.error().broker_code_ == "state_unconfirmed");
      CHECK(canceled.error().reached_broker_);
      CHECK(desk->sends() == 1);
      CHECK(desk->order_status("1") == std::optional<std::string>("pending"));
    }
    auto reopened = held.desk();
    CHECK(reopened->order_status("1") == std::optional<std::string>("canceled"));
  }
  SECTION("a clock move") {
    {
      auto desk = held.desk();
      const fault::Scoped scoped({});
      fault::state_fsyncs_left = 0;
      const auto moved = desk->advance_to(kst("2024-03-26T11:00"));
      fault::state_fsyncs_left = -1;
      CHECK(moved == cmdatl::Moved::UNCONFIRMED);
      CHECK(desk->now() == kst("2024-03-26T10:00"));
    }
    auto reopened = held.desk();
    CHECK(reopened->now() == kst("2024-03-26T11:00"));
  }
}
