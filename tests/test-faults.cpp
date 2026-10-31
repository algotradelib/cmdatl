// The fault tests: their own executable, built with CMDATL_FAULT_HOOKS in its only translation unit, which defines
// cmdatl::sys below.
// So no translation unit sees both the inline system calls and these stand-ins.

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
  ~Scoped() { clear(); }
};

}  // namespace fault

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

}  // namespace sys
}  // namespace cmdatl

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
  return __real_fsync(fd);
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

}  // namespace

TEST_CASE("a read failing after a prefix of the log changes nothing, a torn tail included") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto torn = slurp(fixture.log()) + "{\"op\":\"adv";
  spit(fixture.log(), torn);
  {
    const fault::Scoped scoped({"read", "paper.jsonl", "prefix", 0, 1, EIO});
    const auto result = outcome(fixture, {"status"});
    CHECK(result.first == 4);
    CHECK(result.second == "unknown.paper_state");
    CHECK(fault::injected == 2);
  }
  CHECK(slurp(fixture.log()) == torn);
  CHECK(outcome(fixture, {"status"}).first == 0);
  CHECK(slurp(fixture.log()) + "{\"op\":\"adv" == torn);
}

TEST_CASE("an advance not written whole leaves the clock where it was, and the next open cuts the piece") {
  for (const auto* mode : {"short", "zero", "fail"}) {
    INFO(mode);
    const PaperFixture fixture;
    REQUIRE(outcome(fixture, {"status"}).first == 0);
    const auto log = slurp(fixture.log());
    {
      const fault::Scoped scoped({"write", "paper.jsonl", mode, 0, 1, ENOSPC});
      const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
      CHECK(result.first == 1);
      CHECK(result.second == "file.unwritable");
      CHECK(fault::injected >= 1);
    }
    CHECK(now_of(fixture) == "2024-03-25T16:00:00+09:00");
    CHECK(slurp(fixture.log()) == log);
    CHECK(outcome(fixture, {"clock", "advance", "--minutes", "5"}).first == 0);
    CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
  }
}

TEST_CASE("repeated EINTR on the append is retried, and the advance happens once") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  {
    const fault::Scoped scoped({"write", "paper.jsonl", "eintr", 0, 5, EINTR});
    CHECK(outcome(fixture, {"clock", "advance", "--minutes", "5"}).first == 0);
    CHECK(fault::injected == 5);
  }
  CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
  CHECK(lines_of(slurp(fixture.log())).size() == 2);
}

TEST_CASE("a whole advance whose flush failed is unknown, and the next command sees it once") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  {
    const fault::Scoped scoped({"fsync", "paper.jsonl", "fail", 0, 1, EIO});
    const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
    CHECK(result.first == 4);
    CHECK(result.second == "unknown.clock_advance");
    CHECK(declares({"clock", "advance"}, 4, "unknown.clock_advance"));
  }
  CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
  CHECK(lines_of(slurp(fixture.log())).size() == 2);
}

TEST_CASE("a failed repair appends nothing more, and a later open finishes it") {
  const PaperFixture fixture;
  REQUIRE(outcome(fixture, {"status"}).first == 0);
  const auto complete = slurp(fixture.log());
  const auto torn = complete + "{\"op\":";
  spit(fixture.log(), torn);
  {
    const fault::Scoped scoped({"ftruncate", "paper.jsonl", "fail", 0, 1, EIO});
    const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
    CHECK(result.first == 4);
    CHECK(result.second == "unknown.paper_state");
  }
  CHECK(slurp(fixture.log()) == torn);
  {
    // The cut made, its flush failed: no append after it.
    const fault::Scoped scoped({"fsync", "paper.jsonl", "fail", 0, 1, EIO});
    const auto result = outcome(fixture, {"clock", "advance", "--minutes", "5"});
    CHECK(result.first == 4);
    CHECK(result.second == "unknown.paper_state");
  }
  CHECK(slurp(fixture.log()) == complete);
  CHECK(outcome(fixture, {"clock", "advance", "--minutes", "5"}).first == 0);
  CHECK(lines_of(slurp(fixture.log())).size() == 2);
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
    const fault::Scoped scoped({"fstat", "paper.jsonl", "replace-lock", 0, -1, 0});
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
  const auto torn = slurp(fixture.log()) + "{\"op\":";
  spit(fixture.log(), torn);
  const auto journal = slurp(fixture.journal());
  const std::vector<std::pair<std::string, std::string>> cases{
    {"paper.jsonl", "unknown.paper_state"},
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
  // The second status read of the log (after the read, checking its size held): refused the same way.
  {
    const fault::Scoped scoped({"fstat", "paper.jsonl", "fail", 2, 1, EIO});
    CHECK(outcome(fixture, {"status"}).second == "unknown.paper_state");
  }
  CHECK(slurp(fixture.log()) == torn);
  CHECK(outcome(fixture, {"status"}).first == 0);
}

TEST_CASE("an advance killed after its line, after its flush, or after it applied, is seen exactly once") {
  for (const auto* point : {"advance.written", "advance.synced", "advance.applied"}) {
    INFO(point);
    const PaperFixture fixture;
    REQUIRE(outcome(fixture, {"status"}).first == 0);
    const int status = run_killed({"--json", "clock", "advance", "--minutes", "5"}, point);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
    const auto lines = lines_of(slurp(fixture.log()));
    REQUIRE(lines.size() == 2);
    CHECK(lines.back() == R"({"op":"advance","to":1711350300000000000})");
    CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
    CHECK(now_of(fixture) == "2024-03-25T16:05:00+09:00");
    CHECK(lines_of(slurp(fixture.log())).size() == 2);
  }
}

TEST_CASE("a first initialization killed during its genesis is finished by the next command") {
  const PaperFixture fixture;
  const int status = run_killed({"--json", "status"}, "genesis.written");
  REQUIRE(WIFSIGNALED(status));
  CHECK(std::filesystem::exists(fixture.log()));
  CHECK(!std::filesystem::exists(fixture.journal()));
  const auto genesis = slurp(fixture.log());
  CHECK(outcome(fixture, {"status"}).first == 0);
  CHECK(slurp(fixture.log()) == genesis);
  CHECK(algotradelib::runner::read_journal_file(fixture.journal().string()).has_header_);
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
  int exit() const { return WIFEXITED(status_) ? WEXITSTATUS(status_) : -1; }

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
  const PaperFixture& fixture,
  std::vector<std::string> args,
  const std::function<void(const std::string&)>& hook,
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
  const std::string& point,
  const std::filesystem::path& file,
  off_t extra,
  bool fresh
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

}  // namespace

TEST_CASE("a place killed after its plan was marked used: plan_used and nothing journaled or logged and a new plan works") {
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

TEST_CASE("a place killed after its PENDING and before the paper line: owed unnamed and never a second order and release") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  const auto forked = run_forked(fixture, {"order", "place", id}, nullptr, "send.before_input");
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
  const auto journal_id = std::string(ask(fixture, {"account"}).body_.at("owed").as_array()[0].as_object().at("journal_id").as_string());
  spit(fixture.dir() / "paper.release", "005930 " + journal_id + "\n");
  CHECK(ask_checked(fixture, {"order", "release", "005930", "--reason", "read the order list and holdings"}).reason_ == "released");
  CHECK(ask_checked(fixture, {"order", "reconcile"}).reason_ == "clean");
  const auto next = plan_id(fixture, "005930", "10", "75000");
  CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
}

TEST_CASE("a place killed after its paper line or its result: the market has it and and the next commands name it") {
  for (const auto* point : {"send.after_input", "send.after_result"}) {
    INFO(point);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto forked = run_forked(fixture, {"order", "place", id}, nullptr, point);
    REQUIRE(WIFSIGNALED(forked.status_));
    CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
    const auto account = ask(fixture, {"account"});
    REQUIRE(account.body_.at("owed").as_array().size() == 1);
    CHECK(ops_of(slurp(fixture.log()), "result").size() == 1);
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

TEST_CASE("B1: the armed file removed before once() and or before the paper line and sends nothing and names the file") {
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
      if (name == "send.before_input") {
        std::filesystem::remove(armed);
      }
    });
    INFO(forked.out_);
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.not_reached");
    // The binding's own refusal, naming the armed file.
    const auto message = std::string(reply_of(forked.out_).at("error").as_object().at("message").as_string());
    CHECK(message.find("armed file does not exist: " + armed.string()) != std::string::npos);
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    // The journal answered the PENDING (a local refusal is journaled as REJECTED): nothing is owed.
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().empty());
    fixture.arm();
    const auto next = plan_id(fixture, "005930", "10", "75000");
    CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
  }
}

TEST_CASE("B5: the journal moved aside and emptied or replaced by a copy before once(): journal_missing and none created") {
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

TEST_CASE("the paper line not written whole is refused.not_reached; its fsync failing is unknown.indeterminate") {
  for (const auto* mode : {"short", "zero", "fail"}) {
    INFO(mode);
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    {
      // The first write is the command's simulated second; the second is the place line.
      const fault::Scoped scoped({"write", "paper.jsonl", mode, 1, 1, ENOSPC});
      const auto result = ask(fixture, {"order", "place", id});
      INFO(boost::json::serialize(result.body_));
      CHECK(result.exit_ == 3);
      CHECK(result.code_ == "refused.not_reached");
    }
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    CHECK(ask(fixture, {"account"}).body_.at("owed").as_array().empty());
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    const auto next = plan_id(fixture, "005930", "10", "75000");
    CHECK(ask_checked(fixture, {"order", "place", next}).reason_ == "placed");
  }
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  {
    const fault::Scoped scoped({"fsync", "paper.jsonl", "fail", 1, 1, EIO});
    const auto result = ask(fixture, {"order", "place", id});
    CHECK(result.exit_ == 4);
    CHECK(result.code_ == "unknown.indeterminate");
  }
  // The line survived: the replay applies it, and the order rests.
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
  const auto reconciled = ask_checked(fixture, {"order", "reconcile"});
  CHECK(reconciled.exit_ != 0);
  CHECK(ask(fixture, {"order", "list"}).body_.at("orders").as_array().size() == 1);
}

TEST_CASE("the journal's PENDING failing at zero bytes or as a prefix: refused.journal_unwritable and nothing sent") {
  {
    const PaperFixture fixture;
    const auto id = ready_plan(fixture);
    const auto journal = slurp(fixture.journal());
    const auto forked = run_forked(fixture, {"order", "place", id}, limit_at("order.before_once", fixture.journal(), 0, true));
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
    const auto forked = run_forked(fixture, {"order", "place", id}, limit_at("order.before_once", fixture.journal(), 20, true));
    INFO(forked.out_);
    CHECK(forked.exit() == 3);
    CHECK(forked.code() == "refused.journal_unwritable");
    CHECK(ops_of(slurp(fixture.log()), "place").empty());
    // The torn line: every command refuses, nothing is repaired.
    const auto torn = slurp(fixture.journal());
    for (int round = 0; round < 2; ++round) {
      CHECK(ask_checked(fixture, {"account"}).code_ == "unknown.journal_damaged");
      CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "1", "--limit", "75000"}).code_ == "unknown.journal_damaged");
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
  const auto forked = run_forked(fixture, {"order", "place", id}, limit_at("send.after_result", fixture.journal(), 0, false));
  INFO(forked.out_);
  CHECK(forked.exit() == 4);
  CHECK(forked.code() == "unknown.journal_write");
  CHECK(ops_of(slurp(fixture.log()), "place").size() == 1);
  // The PENDING is unanswered and the order rests: owed, and the next plan finds it.
  const auto owed = ask(fixture, {"account"}).body_.at("owed").as_array();
  REQUIRE(owed.size() == 1);
  CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "75000"}).reason_ == "met");
}

TEST_CASE("a market-refused place whose journal answer fails: unknown.journal_write and then unresolved") {
  ConfigParts parts;
  parts.limits_ += "phases = [\"pre_open\", \"open\"]\n";
  const PaperFixture fixture(parts);
  fixture.arm();
  REQUIRE(outcome(fixture, {"clock", "advance", "--to", "2024-03-26T08:10"}).first == 0);
  const auto id = plan_id(fixture, "005930", "10", "78500");
  const auto forked = run_forked(fixture, {"order", "place", id}, limit_at("send.after_result", fixture.journal(), 0, false));
  INFO(forked.out_);
  CHECK(forked.exit() == 4);
  CHECK(forked.code() == "unknown.journal_write");
  const auto plan = ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "78500"});
  CHECK(plan.exit_ == 4);
}

TEST_CASE("a cancel killed after its paper line: canceled on replay and unresolved until the match window closes") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
  const auto forked = run_forked(fixture, {"order", "cancel", "005930"}, nullptr, "send.after_input");
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
    const auto args = std::string(command) == "place" ? std::vector<std::string>{"order", "place", id} :
      std::vector<std::string>{"order", "cancel", "005930"};
    const auto forked = run_forked(fixture, args, [](const std::string& name) {
      if (name == "send.after_input") {
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

TEST_CASE("the per-call second not written whole is exit 1 with nothing sent") {
  const PaperFixture fixture;
  const auto id = ready_plan(fixture);
  for (const auto* call : {"write", "fsync"}) {
    INFO(call);
    const fault::Scoped scoped({call, "paper.jsonl", "fail", 0, 1, EIO});
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
      const auto journal_before = slurp(fixture.journal());
      std::string new_id;
      const auto newer_holder = [&] {
        new_id = plan_id(fixture, "000660", "1", "180000");
        REQUIRE(cmdatl::PlanBook::load(fixture.account().string()).find(new_id).has_value());
      };
      const auto line = std::string(command) == "place" ? std::vector<std::string>{"order", "place", id} :
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
