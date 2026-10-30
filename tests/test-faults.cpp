// The fault tests: their own executable, built with CMDATL_FAULT_HOOKS in its only translation unit, which defines
// cmdatl::sys below.
// So no translation unit sees both the inline system calls and these stand-ins.

#include "./paper-support.hpp"

#include <cmdatl/cli.hpp>
#include <cmdatl/syscalls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(CMDATL_FAULT_HOOKS)
#error "test-faults is built with CMDATL_FAULT_HOOKS"
#endif

#include <fcntl.h>
#include <sys/param.h>
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
}

}  // namespace sys
}  // namespace cmdatl

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
