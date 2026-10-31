// `run log` and `run stop`: the stop file's rewrite, the event cursor, the follow's finish and the stop's outcomes in
// process, over run directories the tests write; and the commands as processes against the real atlc and algotradelib
// (tagged `[acceptance][runs]`), each run held at a known step by the driver's test barrier instead of sleeps.

#include "./paper-support.hpp"
#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace json = boost::json;
namespace runs = cmdatl::runs;
using StopIntent = algotradelib::runner::StopIntent;

namespace {

const std::string RUN_A = "r00000000000a";
const std::string RUN_B = "r00000000000b";

/** @brief Nothing is finished. */
bool none_finished(const std::string&) {
  return false;
}

/**
 * @brief A run's directory under a fixture, made.
 * @param fixture The fixture.
 * @param run The run's id.
 * @return The directory.
 */
std::filesystem::path make_run(const PaperFixture& fixture, const std::string& run) {
  const auto dir = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001" / run;
  std::filesystem::create_directories(dir);
  spit(dir / "run.json", "{\"format\":1,\"run\":\"" + run + "\",\"config\":\"" + fixture.config().string() + "\",\"started\":\"2026-01-01T00:00:00Z\"}\n");
  return dir;
}

/**
 * @brief A success reply of a run.
 * @param run The run's id.
 * @param stopped Its `stopped`.
 * @return The record's text.
 */
std::string success_reply(const std::string& run, const std::string& stopped = "end") {
  return "{\"run\":\"" + run + "\",\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"stopped\":\"" + stopped +
    "\",\"executions\":2,\"owed_at_end\":[]}\n";
}

/** @brief A valid `exit.json` of a driver that exited. */
std::string exited(int code) {
  return "{\"format\":1,\"pid\":7,\"exit\":" + std::to_string(code) + ",\"signal\":null,\"spawned\":true,\"ended\":\"t\"}";
}

/**
 * @brief A start line naming a lock by its device and inode.
 * @param run The run's id.
 * @param lock The lock file.
 * @return The line.
 */
std::string start_line(const std::string& run, const std::filesystem::path& lock) {
  struct stat info{};
  REQUIRE(::stat(lock.c_str(), &info) == 0);
  return "{\"event\":\"start\",\"run\":\"" + run + "\",\"at\":\"2024-01-02T09:00:00+09:00\",\"lock\":{\"device\":" +
    std::to_string(static_cast<long long>(info.st_dev)) + ",\"inode\":" + std::to_string(static_cast<long long>(info.st_ino)) + "}}\n";
}

/** @brief A run held by this test: its lock taken, its pid this process's own (alive). */
struct LiveRun {
  /** @brief The run's directory. */
  std::filesystem::path dir_;
  /** @brief The lock, held. */
  std::unique_ptr<cmdatl::OwnerLock> lock_;

  /**
   * @brief Makes the run, its lock held and its start line written.
   * @param fixture The fixture.
   * @param run The run's id.
   * @param probeable Whether the start line names the lock (else the probe cannot tell).
   */
  LiveRun(const PaperFixture& fixture, const std::string& run, bool probeable = true) : dir_(make_run(fixture, run)) {
    lock_ = std::make_unique<cmdatl::OwnerLock>(dir_.string());
    REQUIRE(lock_->acquire(std::chrono::milliseconds(0), [] { return false; }, [](const std::string&) {}) == cmdatl::OwnerLock::Outcome::HELD);
    spit(dir_ / "pid", std::to_string(::getpid()) + "\n");
    if (probeable) {
      spit(dir_ / "events.jsonl", start_line(run, dir_ / "lock"));
    } else {
      spit(dir_ / "other", "");
      spit(dir_ / "events.jsonl", start_line(run, dir_ / "other"));
    }
  }
};

/** @brief The shortened stop wait for a test's length. */
class ShortWait {
public:
  /**
   * @brief Sets it.
   * @param wait The wait.
   */
  explicit ShortWait(std::chrono::milliseconds wait) { runs::stop_wait_hook() = wait; }
  ShortWait(const ShortWait&) = delete;
  ShortWait& operator=(const ShortWait&) = delete;
  /** @brief Restores 30 s. */
  ~ShortWait() { runs::stop_wait_hook() = std::chrono::milliseconds(30000); }
};

/**
 * @brief Writes a file after a delay, from another thread.
 * @param path The file.
 * @param text Its bytes.
 * @param delay The delay.
 * @return The thread.
 */
std::thread write_later(std::filesystem::path path, std::string text, std::chrono::milliseconds delay) {
  return std::thread([path, text, delay] {
    std::this_thread::sleep_for(delay);
    spit(path, text);
  });
}

}  // namespace

TEST_CASE("the stop file's rewrite keeps every other run's line, terminated, and writes this run's once") {
  // Another run's unterminated last line: kept, terminated, still read by the driver's parser.
  auto out = runs::rewrite_stop("withdraw " + RUN_A, RUN_B, StopIntent::QUIESCE, none_finished);
  REQUIRE(out.text_);
  CHECK(*out.text_ == "withdraw " + RUN_A + "\nquiesce " + RUN_B + "\n");
  CHECK(cmdatl::stop_intent_of(*out.text_, RUN_A) == StopIntent::WITHDRAW);
  CHECK(cmdatl::stop_intent_of(*out.text_, RUN_B) == StopIntent::QUIESCE);
  // This run's own unterminated line, strengthened.
  out = runs::rewrite_stop("quiesce " + RUN_B, RUN_B, StopIntent::WITHDRAW, none_finished);
  CHECK(*out.text_ == "withdraw " + RUN_B + "\n");
  // A line the driver ignores (trailing whitespace) is another line: kept, and this run's written.
  out = runs::rewrite_stop("quiesce " + RUN_B + " \n", RUN_B, StopIntent::QUIESCE, none_finished);
  CHECK(*out.text_ == "quiesce " + RUN_B + " \nquiesce " + RUN_B + "\n");
  // A malformed line naming a path is kept byte for byte and never looked up.
  std::vector<std::string> asked;
  out = runs::rewrite_stop("quiesce ../../etc\nwithdraw " + RUN_A + "\n", RUN_B, StopIntent::QUIESCE, [&asked](const std::string& run) {
    asked.push_back(run);
    return false;
  });
  CHECK(*out.text_ == "quiesce ../../etc\nwithdraw " + RUN_A + "\nquiesce " + RUN_B + "\n");
  CHECK(asked == std::vector<std::string>{RUN_A});
  // A's unread withdraw is kept when B writes, and on B's retry.
  out = runs::rewrite_stop("withdraw " + RUN_A + "\nquiesce " + RUN_B + "\n", RUN_B, StopIntent::WITHDRAW, none_finished);
  CHECK(*out.text_ == "withdraw " + RUN_A + "\nwithdraw " + RUN_B + "\n");
  CHECK(out.kept_runs_ == std::vector<std::string>{RUN_A});
  // A's line goes once A is finished.
  out = runs::rewrite_stop("withdraw " + RUN_A + "\n", RUN_B, StopIntent::QUIESCE, [](const std::string& run) { return run == RUN_A; });
  CHECK(*out.text_ == "quiesce " + RUN_B + "\n");
  // A weaker request over a stronger: nothing to write.
  out = runs::rewrite_stop("withdraw " + RUN_B + "\n", RUN_B, StopIntent::QUIESCE, none_finished);
  CHECK(!out.text_);
  out = runs::rewrite_stop("quiesce " + RUN_B + "\n", RUN_B, StopIntent::QUIESCE, none_finished);
  CHECK(!out.text_);
  // The bound: 4,096 bytes accepted, 4,097 refused.
  const std::string mine = "quiesce " + RUN_B + "\n";
  std::string filler = "x";
  filler += std::string(cmdatl::MAX_STOP_BYTES - mine.size() - 2, 'y');
  filler += "\n";
  out = runs::rewrite_stop(filler, RUN_B, StopIntent::QUIESCE, none_finished);
  CHECK(out.text_->size() == cmdatl::MAX_STOP_BYTES);
  CHECK(!out.full_);
  out = runs::rewrite_stop("z" + filler, RUN_B, StopIntent::QUIESCE, none_finished);
  CHECK(out.text_->size() == cmdatl::MAX_STOP_BYTES + 1);
  CHECK(out.full_);
}

TEST_CASE("the event cursor gives each complete line once, holds a split line, and notices a replaced or shrunk log") {
  const ScratchDir dir;
  const auto path = dir.path() / "events.jsonl";
  runs::EventCursor cursor(path);
  std::vector<std::string> lines;
  const auto take = [&lines](const std::string& line) { lines.push_back(line); };
  CHECK(cursor.read(take) == runs::CursorRead::MISSING);
  spit(path, "{\"event\":\"start\"}\n{\"event\":\"pla");
  CHECK(cursor.read(take) == runs::CursorRead::OK);
  CHECK(lines == std::vector<std::string>{"{\"event\":\"start\"}"});
  CHECK(cursor.tail());
  {
    std::FILE* file = std::fopen(path.c_str(), "a");
    std::fputs("ced\"}\n{\"event\":\"end\"}\n", file);
    std::fclose(file);
  }
  CHECK(cursor.read(take) == runs::CursorRead::OK);
  CHECK(lines == std::vector<std::string>{"{\"event\":\"start\"}", "{\"event\":\"placed\"}", "{\"event\":\"end\"}"});
  CHECK(!cursor.tail());
  // Nothing new: nothing given again.
  CHECK(cursor.read(take) == runs::CursorRead::OK);
  CHECK(lines.size() == 3);
  // A line longer than 64 KiB is skipped, and the next one read.
  {
    std::FILE* file = std::fopen(path.c_str(), "a");
    std::fputs((std::string(70000, 'q') + "\n{\"event\":\"clock\"}\n").c_str(), file);
    std::fclose(file);
  }
  CHECK(cursor.read(take) == runs::CursorRead::OK);
  CHECK(cursor.skipped() == 1);
  CHECK(lines.back() == "{\"event\":\"clock\"}");
  // Shrunk below the cursor, then replaced by another file.
  ::truncate(path.c_str(), 10);
  CHECK(cursor.read(take) == runs::CursorRead::REPLACED);
  runs::EventCursor second(path);
  CHECK(second.read(take) == runs::CursorRead::OK);
  // Another file renamed over it while the first is still open: another inode.
  spit(dir.path() / "new.jsonl", "{\"event\":\"start\"}\n{\"event\":\"end\"}\n");
  std::filesystem::rename(dir.path() / "new.jsonl", path);
  CHECK(second.read(take) == runs::CursorRead::REPLACED);
  // A directory at the path is not a log.
  std::filesystem::remove(path);
  std::filesystem::create_directory(path);
  runs::EventCursor third(path);
  CHECK(third.read(take) == runs::CursorRead::UNREADABLE);
}

TEST_CASE("run log without --follow: every event but heartbeats and the last heartbeat, bounded after filtering") {
  const PaperFixture fixture;
  const auto dir = make_run(fixture, RUN_A);
  // No log, and a run that never traded: empty, with its state.
  spit(dir / "reply.json", "{\"run\":\"" + RUN_A + "\",\"exit\":3,\"result\":\"refused\",\"error\":{\"code\":\"refused.not_armed\"}}\n");
  spit(dir / "exit.json", exited(3));
  auto ran = fixture.json({"run", "log", RUN_A});
  CHECK(ran.exit_ == 0);
  CHECK(reply_of(ran.out_).at("events").as_array().empty());
  CHECK(reply_of(ran.out_).at("state").as_string() == "refused");
  // No log, and a run that traded: refused as unreadable, never an empty success.
  spit(dir / "reply.json", success_reply(RUN_A));
  spit(dir / "exit.json", exited(0));
  ran = fixture.json({"run", "log", RUN_A});
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "run.log_unreadable");
  // 5,000 events of one symbol, 10 of another, heartbeats between.
  std::string log = "{\"event\":\"start\",\"run\":\"" + RUN_A + "\"}\n";
  for (int k = 0; k < 5000; ++k) {
    log += "{\"event\":\"placed\",\"symbol\":\"005930\",\"k\":" + std::to_string(k) + "}\n";
    if (k % 500 == 0) {
      log += "{\"event\":\"clock\",\"at\":\"c" + std::to_string(k) + "\"}\n";
      log += "{\"event\":\"filled\",\"symbol\":\"000660\",\"k\":" + std::to_string(k) + "}\n";
    }
  }
  log += "not json\n{\"event\":\"end\"}\n";
  spit(dir / "events.jsonl", log);
  ran = fixture.json({"run", "log", RUN_A});
  CHECK(ran.exit_ == 0);
  auto reply = reply_of(ran.out_);
  CHECK(reply.at("truncated").as_bool());
  const auto& events = reply.at("events").as_array();
  CHECK(events.size() == 4096);
  std::size_t clocks = 0;
  for (const auto& event : events) {
    clocks += event.as_object().at("event").as_string() == "clock" ? 1 : 0;
  }
  CHECK(clocks == 1);
  CHECK(events.back().as_object().at("event").as_string() == "end");
  ran = fixture.json({"run", "log", RUN_A, "--symbol", "000660"});
  reply = reply_of(ran.out_);
  CHECK(!reply.at("truncated").as_bool());
  std::size_t filled = 0;
  for (const auto& event : reply.at("events").as_array()) {
    filled += event.as_object().at("event").as_string() == "filled" ? 1 : 0;
    CHECK(event.as_object().at("event").as_string() != "placed");
  }
  CHECK(filled == 10);
}

TEST_CASE("run log --follow finishes on the end line, or on a final state with the writer gone, and says when it is incomplete") {
  const PaperFixture fixture;
  {
    // Refused before trading: a reply, exit.json, no log.
    const auto dir = make_run(fixture, RUN_A);
    spit(dir / "reply.json", "{\"run\":\"" + RUN_A + "\",\"exit\":3,\"result\":\"refused\",\"error\":{\"code\":\"refused.not_armed\"}}\n");
    spit(dir / "exit.json", exited(3));
    const auto ran = fixture.json({"run", "log", RUN_A, "--follow"});
    CHECK(ran.exit_ == 0);
    const auto reply = reply_of(lines_of(ran.out_).back());
    CHECK(reply.at("state").as_string() == "refused");
    CHECK(!reply.at("incomplete").as_bool());
  }
  {
    // Nothing spawned.
    const auto dir = make_run(fixture, RUN_B);
    spit(dir / "exit.json", "{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}");
    const auto ran = fixture.json({"run", "log", RUN_B, "--follow"});
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(lines_of(ran.out_).back()).at("state").as_string() == "failed");
  }
  {
    // A reply and no end line, the writer gone: incomplete, exit 4, every event emitted once.
    const std::string run = "r00000000000c";
    const auto dir = make_run(fixture, run);
    spit(dir / "lock", "");
    spit(dir / "events.jsonl", start_line(run, dir / "lock") + "{\"event\":\"placed\",\"symbol\":\"005930\"}\n{\"event\":\"fil");
    spit(dir / "reply.json", success_reply(run));
    spit(dir / "exit.json", exited(0));
    const auto ran = fixture.json({"run", "log", run, "--follow"});
    CHECK(ran.exit_ == 4);
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 3);
    CHECK(reply_of(lines[0]).at("event").as_string() == "start");
    CHECK(reply_of(lines[1]).at("event").as_string() == "placed");
    CHECK(reply_of(lines[2]).at("incomplete").as_bool());
  }
  {
    // With the end line: every event once, then the final reply, exit 0, and --symbol filtering order events only.
    const std::string run = "r00000000000d";
    const auto dir = make_run(fixture, run);
    spit(dir / "lock", "");
    spit(dir / "events.jsonl",
         start_line(run, dir / "lock") + "{\"event\":\"placed\",\"symbol\":\"005930\"}\n{\"event\":\"placed\",\"symbol\":\"000660\"}\n"
           "{\"event\":\"clock\"}\n{\"event\":\"end\"}\n");
    spit(dir / "reply.json", success_reply(run));
    spit(dir / "exit.json", exited(0));
    const auto ran = fixture.json({"run", "log", run, "--follow", "--symbol", "000660"});
    CHECK(ran.exit_ == 0);
    const auto lines = lines_of(ran.out_);
    REQUIRE(lines.size() == 5);
    CHECK(reply_of(lines[1]).at("symbol").as_string() == "000660");
    CHECK(reply_of(lines[2]).at("event").as_string() == "clock");
    CHECK(reply_of(lines[3]).at("event").as_string() == "end");
    CHECK(!reply_of(lines[4]).at("incomplete").as_bool());
  }
}

TEST_CASE("run log --follow keeps reading a live driver behind an unprobeable lock until interrupted, and picks up appended lines") {
  const PaperFixture fixture;
  const LiveRun live(fixture, RUN_A, false);
  std::thread appender([&live] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::FILE* file = std::fopen((live.dir_ / "events.jsonl").c_str(), "a");
    std::fputs("{\"event\":\"placed\",\"sym", file);
    std::fflush(file);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::fputs("bol\":\"005930\"}\n", file);
    std::fclose(file);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cmdatl::interruption = 1;
  });
  const auto ran = fixture.json({"run", "log", RUN_A, "--follow"});
  appender.join();
  cmdatl::interruption = 0;
  CHECK(ran.exit_ == 130);
  const auto lines = lines_of(ran.out_);
  REQUIRE(lines.size() >= 2);
  CHECK(reply_of(lines[0]).at("event").as_string() == "start");
  CHECK(reply_of(lines[1]).at("event").as_string() == "placed");
}

TEST_CASE("run stop answers success only on evidence that trading ended") {
  const ShortWait wait(std::chrono::milliseconds(400));
  const PaperFixture fixture;
  const auto stop_file = fixture.dir() / "paper.stop";
  {
    // Ended with its reply: nothing written.
    const auto dir = make_run(fixture, RUN_A);
    spit(dir / "reply.json", success_reply(RUN_A));
    spit(dir / "exit.json", exited(0));
    const auto ran = fixture.json({"run", "stop", RUN_A});
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("state").as_string() == "ended");
    CHECK(!std::filesystem::exists(stop_file));
  }
  {
    // Lost: a start line, the lock free, no reply.
    const auto dir = make_run(fixture, RUN_B);
    spit(dir / "lock", "");
    spit(dir / "pid", "999999\n");
    spit(dir / "events.jsonl", start_line(RUN_B, dir / "lock"));
    const auto ran = fixture.json({"run", "stop", RUN_B});
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("state").as_string() == "lost");
    CHECK(reply_of(ran.out_).at("run_exit").as_int64() == 4);
    CHECK(!std::filesystem::exists(stop_file));
  }
  {
    // Nothing spawned.
    const std::string run = "r00000000000c";
    const auto dir = make_run(fixture, run);
    spit(dir / "exit.json", "{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}");
    const auto ran = fixture.json({"run", "stop", run});
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("run_exit").as_int64() == 1);
    CHECK(!std::filesystem::exists(stop_file));
  }
  {
    // A live driver behind a lock the probe cannot tell: written, never answered as success.
    const std::string run = "r00000000000d";
    const LiveRun live(fixture, run, false);
    const auto ran = fixture.json({"run", "stop", run});
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.run_stop");
    CHECK(slurp(stop_file) == "quiesce " + run + "\n");
  }
}

TEST_CASE("run stop: an acknowledgement then the reply is stopped, a retried stop counts the earlier acknowledgement, an end without one says so") {
  const ShortWait wait(std::chrono::milliseconds(3000));
  const PaperFixture fixture;
  {
    const LiveRun live(fixture, RUN_A);
    auto writer = std::thread([&live] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::FILE* file = std::fopen((live.dir_ / "events.jsonl").c_str(), "a");
      std::fputs("{\"event\":\"stop\",\"reason\":\"operator\",\"intent\":\"withdraw\"}\n", file);
      std::fclose(file);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      spit(live.dir_ / "reply.json", success_reply(RUN_A, "operator"));
    });
    const auto ran = fixture.json({"run", "stop", RUN_A, "--withdraw"});
    writer.join();
    CHECK(ran.exit_ == 0);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("state").as_string() == "stopped");
    CHECK(reply.at("acknowledged").as_bool());
    CHECK(reply.at("summary").as_object().at("stopped").as_string() == "operator");
  }
  {
    // A retried quiesce after the driver acknowledged a withdraw: nothing written, acknowledged.
    const LiveRun live(fixture, RUN_B);
    spit(fixture.dir() / "paper.stop", "withdraw " + RUN_B + "\n");
    {
      std::FILE* file = std::fopen((live.dir_ / "events.jsonl").c_str(), "a");
      std::fputs("{\"event\":\"stop\",\"reason\":\"operator\",\"intent\":\"withdraw\"}\n", file);
      std::fclose(file);
    }
    auto writer = write_later(live.dir_ / "reply.json", success_reply(RUN_B, "end"), std::chrono::milliseconds(300));
    const auto ran = fixture.json({"run", "stop", RUN_B});
    writer.join();
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("acknowledged").as_bool());
    CHECK(slurp(fixture.dir() / "paper.stop") == "withdraw " + RUN_B + "\n");
  }
  {
    // The run ended without acknowledging: exit 0, acknowledged false.
    const std::string run = "r00000000000c";
    const LiveRun live(fixture, run);
    auto writer = write_later(live.dir_ / "reply.json", success_reply(run, "end"), std::chrono::milliseconds(300));
    const auto ran = fixture.json({"run", "stop", run});
    writer.join();
    CHECK(ran.exit_ == 0);
    CHECK(reply_of(ran.out_).at("state").as_string() == "ended");
    CHECK(!reply_of(ran.out_).at("acknowledged").as_bool());
  }
  {
    // Acknowledged, no reply within the wait: unknown.run_stopping.
    const ShortWait shorter(std::chrono::milliseconds(500));
    const std::string run = "r00000000000d";
    const LiveRun live(fixture, run);
    {
      std::FILE* file = std::fopen((live.dir_ / "events.jsonl").c_str(), "a");
      std::fputs("{\"event\":\"stop\",\"reason\":\"operator\",\"intent\":\"quiesce\"}\n", file);
      std::fclose(file);
    }
    const auto ran = fixture.json({"run", "stop", run});
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.run_stopping");
  }
}

TEST_CASE("run stop refuses a stop file that is not a regular file, a full one, and another operator file's run") {
  const ShortWait wait(std::chrono::milliseconds(200));
  const PaperFixture fixture;
  const LiveRun live(fixture, RUN_A);
  const auto stop_file = fixture.dir() / "paper.stop";
  std::filesystem::create_symlink(fixture.dir() / "elsewhere", stop_file);
  CHECK(code_of(fixture.json({"run", "stop", RUN_A}).out_) == "file.unwritable");
  CHECK(!std::filesystem::exists(fixture.dir() / "elsewhere"));
  std::filesystem::remove(stop_file);
  std::filesystem::create_directory(stop_file);
  CHECK(code_of(fixture.json({"run", "stop", RUN_A}).out_) == "file.unwritable");
  std::filesystem::remove(stop_file);
  REQUIRE(::mkfifo(stop_file.c_str(), 0600) == 0);
  CHECK(code_of(fixture.json({"run", "stop", RUN_A}).out_) == "file.unwritable");
  std::filesystem::remove(stop_file);
  // Full of another live run's lines (malformed ones are kept too): refused, nothing written.
  const std::string full(cmdatl::MAX_STOP_BYTES - 10, 'x');
  spit(stop_file, full + "\n");
  const auto refused = fixture.json({"run", "stop", RUN_A});
  CHECK(refused.exit_ == 3);
  CHECK(code_of(refused.out_) == "refused.stop_file_full");
  CHECK(slurp(stop_file) == full + "\n");
  std::filesystem::remove(stop_file);
  // A stale temporary of a killed writer does not block the next.
  spit(fixture.dir() / ".paper.stop.123.00000000000000ff.tmp", "half");
  CHECK(fixture.json({"run", "stop", RUN_A}).exit_ == 4);
  CHECK(slurp(stop_file) == "quiesce " + RUN_A + "\n");
  // A run that recorded another operator file.
  const std::string other = "r00000000000e";
  const auto dir = make_run(fixture, other);
  spit(dir / "run.json", "{\"format\":1,\"run\":\"" + other + "\",\"config\":\"/somewhere/else.toml\",\"started\":\"2026-01-01T00:00:00Z\"}\n");
  const auto elsewhere = fixture.json({"run", "stop", other});
  CHECK(elsewhere.exit_ == 2);
  CHECK(code_of(elsewhere.out_) == "usage.bad_value");
}

TEST_CASE("the stop file's writer lock: busy past its deadline, and replaced while waited for") {
  const ScratchDir dir;
  const auto path = dir.path() / "paper.stop.lock";
  spit(path, "");
  const int held = ::open(path.c_str(), O_RDWR);
  REQUIRE(::flock(held, LOCK_EX) == 0);
  runs::StopLock busy;
  CHECK(busy.take(path, std::chrono::milliseconds(200)) == runs::StopLock::Outcome::BUSY);
  std::atomic<int> outcome{-1};
  std::thread waiter([&] {
    runs::StopLock lock;
    outcome = static_cast<int>(lock.take(path, std::chrono::milliseconds(3000)));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::filesystem::rename(path, dir.path() / "old.lock");
  spit(path, "");
  ::close(held);
  waiter.join();
  CHECK(outcome == static_cast<int>(runs::StopLock::Outcome::FAILED));
}

// --- Processes, against the real atlc and algotradelib ---

namespace {

/**
 * @brief Daily bars of 005930 from 2024-01-02, date-only rows, a walk with a sharp dip every 23 days, so the
 *   mean-reversion starter trades.
 * @param days How many weekdays.
 * @return The CSV.
 */
std::string dip_csv(int days) {
  std::string out = "symbol,date,open,high,low,close,volume\n";
  long close = 78000;
  long day = 19724;  // 2024-01-02
  for (int k = 0, made = 0; made < days; ++day) {
    if ((day + 3) % 7 >= 5) {
      continue;
    }
    const long open = close;
    long step = ((k * 37) % 7 - 3) * 100;
    if (k % 23 == 20) {
      step = -5000;
    }
    if (k % 23 == 21 || k % 23 == 22) {
      step = 2500;
    }
    close = std::max(1000L, open + step);
    const long z = day + 719468;
    const long era = z / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    const long d = doy - (153 * mp + 2) / 5 + 1;
    const long m = mp < 10 ? mp + 3 : mp - 9;
    const long y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char row[160];
    std::snprintf(row, sizeof row, "005930,%04ld-%02ld-%02ld,%ld,%ld,%ld,%ld,100000\n", y, m, d, open, std::max(open, close) + 300,
                  std::min(open, close) - 300, close);
    out += row;
    ++k;
    ++made;
  }
  return out;
}

/** @brief The program, its specs and the build cache the process cases share. */
struct Shared {
  /** @brief Where they live. */
  ScratchDir dir_;

  Shared() {
    spit(dir_.path() / "dip.atls", slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "templates" / "mean-reversion.atls"));
    spit(dir_.path() / "short.csv", dip_csv(120));
    spit(dir_.path() / "mid.csv", dip_csv(250));
    for (const auto* name : {"short", "mid"}) {
      spit(dir_.path() / (std::string(name) + ".toml"),
           "strategy = \"dip.atls\"\n\n[data]\nsource = \"csv\"\nfile = \"" + std::string(name) +
             ".csv\"\nsymbol_column = \"symbol\"\ntime = \"date\"\ntime_format = \"iso\"\n\n[instruments]\nstock = \"005930\"\n\n"
             "[replay]\ncash = 100000000\ncommission = 0.0005\nmarket = \"kr\"\n\n[orders]\ntype = \"limit\"\noffset = 0.01\n");
    }
    std::filesystem::create_directories(dir_.path() / "cache");
  }

  /**
   * @brief A spec's path.
   * @param name `short` or `mid`.
   * @return The path.
   */
  std::string spec(const std::string& name) const { return (dir_.path() / (name + ".toml")).string(); }

  /**
   * @brief The barrier file's path.
   * @return The path.
   */
  std::filesystem::path barrier() const { return dir_.path() / "barrier"; }
};

/**
 * @brief The shared fixture, made once per test process.
 * @return It.
 */
Shared& shared() {
  static Shared s;
  return s;
}

/** @brief The operator's file: 005930 allowed with room for every order. */
ConfigParts operator_parts() {
  ConfigParts parts;
  parts.limits_ = "symbols = [\"005930\"]\nposition = { \"005930\" = 100000 }\norder_value = 100000000000\ndaily_orders = 1000\n";
  return parts;
}

/** @brief The real atlc, the library, the shared cache, a test-built driver and its barrier, for a case's length. */
class Equipped {
public:
  /**
   * @brief Sets the variables.
   * @param atlc atlc's path.
   * @param prefix The library prefix.
   * @param on_order Whether the barrier holds only while an order rests.
   */
  Equipped(const std::string& atlc, const std::string& prefix, bool on_order = false)
  : atlc_("CMDATL_ATLC", atlc),
    prefix_("CMAKE_PREFIX_PATH", prefix),
    cache_("XDG_CACHE_HOME", (shared().dir_.path() / "cache").string()),
    flags_("CXXFLAGS", std::string("-DCMDATL_RUN_TEST_HOOKS")),
    barrier_("CMDATL_TEST_BARRIER", shared().barrier().string()),
    on_order_("CMDATL_TEST_BARRIER_ON_ORDER", on_order ? std::optional<std::string>("1") : std::nullopt) {}

private:
  ScopedVariable atlc_;
  ScopedVariable prefix_;
  ScopedVariable cache_;
  ScopedVariable flags_;
  ScopedVariable barrier_;
  ScopedVariable on_order_;
};

/** @brief The real atlc and the library prefix, or nothing. */
std::optional<std::pair<std::string, std::string>> equipment() {
  const auto atlc = cmdatl::variable("CMDATL_TEST_ATLC");
  const auto prefix = cmdatl::variable("CMDATL_TEST_PREFIX");
  if (!atlc || !prefix) {
    return std::nullopt;
  }
  return std::make_pair(*atlc, *prefix);
}

/**
 * @brief Waits until a condition holds, at most `seconds`.
 * @param condition The condition.
 * @param seconds The bound.
 * @return Whether it held.
 */
bool eventually(const std::function<bool()>& condition, int seconds = 60) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!condition()) {
    if (std::chrono::steady_clock::now() > until) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

/**
 * @brief Starts a run held at its first step by the barrier, and answers its id.
 * @param fixture The fixture.
 * @param spec The spec.
 * @return The run's id.
 */
std::string start_held(const PaperFixture& fixture, const std::string& spec) {
  spit(shared().barrier(), "");
  const auto ran = fixture.json({"run", "start", spec});
  INFO(ran.out_);
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  return std::string(reply_of(ran.out_).at("run").as_string());
}

/**
 * @brief `run status` until the run's state is final.
 * @param fixture The fixture.
 * @param run The run's id.
 * @return The last reply.
 */
json::object final_status(const PaperFixture& fixture, const std::string& run) {
  json::object reply;
  REQUIRE(eventually([&] {
    reply = reply_of(fixture.json({"run", "status", run}).out_);
    const std::string state(reply.at("state").as_string());
    return state != "starting" && state != "running";
  }, 120));
  return reply;
}

/**
 * @brief A run's directory.
 * @param fixture The fixture.
 * @param run The run's id.
 * @return The directory.
 */
std::filesystem::path dir_of(const PaperFixture& fixture, const std::string& run) {
  return fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001" / run;
}

}  // namespace

TEST_CASE("run stop is acknowledged and the run ends; --withdraw cancels a resting limit", "[acceptance][runs]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const auto stop_file = [](const PaperFixture& fixture) { return fixture.dir() / "paper.stop"; };
  {
    const PaperFixture fixture(operator_parts());
    const Equipped equipped(equip->first, equip->second);
    fixture.arm();
    const auto run = start_held(fixture, shared().spec("mid"));
    Spawned stopper({"run", "stop", run, "--json"});
    REQUIRE(eventually([&] { return slurp(stop_file(fixture)).find("quiesce " + run) != std::string::npos; }));
    std::filesystem::remove(shared().barrier());
    const int status = stopper.wait(60);
    INFO(stopper.out());
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    const auto reply = reply_of(stopper.out());
    CHECK(reply.at("state").as_string() == "stopped");
    CHECK(reply.at("acknowledged").as_bool());
    CHECK(reply.at("summary").as_object().at("stopped").as_string() == "operator");
  }
  {
    const PaperFixture fixture(operator_parts());
    const Equipped equipped(equip->first, equip->second, true);
    fixture.arm();
    const auto run = start_held(fixture, shared().spec("mid"));
    // Held at the step after the first placement, while the limit rests.
    REQUIRE(eventually([&] { return slurp(dir_of(fixture, run) / "events.jsonl").find("\"event\":\"placed\"") != std::string::npos; }));
    Spawned stopper({"run", "stop", run, "--withdraw", "--json"});
    REQUIRE(eventually([&] { return slurp(stop_file(fixture)).find("withdraw " + run) != std::string::npos; }));
    std::filesystem::remove(shared().barrier());
    const int status = stopper.wait(60);
    INFO(stopper.out());
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    const auto reply = reply_of(stopper.out());
    CHECK(reply.at("state").as_string() == "stopped");
    CHECK(reply.at("summary").as_object().at("owed_at_end").as_array().empty());
    CHECK(slurp(dir_of(fixture, run) / "events.jsonl").find("\"event\":\"canceled\"") != std::string::npos);
  }
}

TEST_CASE("two runs of one operator file are each stopped, neither erasing the other's line", "[acceptance][runs]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(operator_parts());
  const Equipped equipped(equip->first, equip->second);
  fixture.arm();
  const auto a = start_held(fixture, shared().spec("mid"));
  const auto b = start_held(fixture, shared().spec("mid"));
  const auto stop_file = fixture.dir() / "paper.stop";
  Spawned stop_a({"run", "stop", a, "--withdraw", "--json"});
  REQUIRE(eventually([&] { return slurp(stop_file).find("withdraw " + a) != std::string::npos; }));
  Spawned stop_b({"run", "stop", b, "--json"});
  REQUIRE(eventually([&] { return slurp(stop_file).find("quiesce " + b) != std::string::npos; }));
  const auto both = slurp(stop_file);
  CHECK(both.find("withdraw " + a + "\n") != std::string::npos);
  CHECK(both.find("quiesce " + b + "\n") != std::string::npos);
  std::filesystem::remove(shared().barrier());
  for (auto* stopper : {&stop_a, &stop_b}) {
    const int status = stopper->wait(60);
    INFO(stopper->out());
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(reply_of(stopper->out()).at("summary").as_object().at("stopped").as_string() == "operator");
  }
}

TEST_CASE("run stop of a stopped driver ends unknown, of an ended run writes nothing, and from another operator file is refused", "[acceptance][runs]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(operator_parts());
  const Equipped equipped(equip->first, equip->second);
  fixture.arm();
  {
    const auto run = start_held(fixture, shared().spec("mid"));
    const auto driver = std::stol(slurp(dir_of(fixture, run) / "pid"));
    ::kill(static_cast<pid_t>(driver), SIGSTOP);
    std::filesystem::remove(shared().barrier());
    {
      const ShortWait wait(std::chrono::milliseconds(2000));
      const auto ran = fixture.json({"run", "stop", run});
      CHECK(ran.exit_ == 4);
      CHECK(code_of(ran.out_) == "unknown.run_stop");
    }
    ::kill(static_cast<pid_t>(driver), SIGCONT);
    const auto final = final_status(fixture, run);
    CHECK(final.at("state").as_string() == "ended");
    CHECK(final.at("summary").as_object().at("stopped").as_string() == "operator");
  }
  std::filesystem::remove(fixture.dir() / "paper.stop");
  const auto ended = fixture.json({"run", "start", shared().spec("short"), "--wait"});
  REQUIRE(ended.exit_ == 0);
  const auto run = std::string(reply_of(ended.out_).at("run").as_string());
  const auto stopped = fixture.json({"run", "stop", run});
  CHECK(stopped.exit_ == 0);
  CHECK(reply_of(stopped.out_).at("state").as_string() == "ended");
  CHECK(!std::filesystem::exists(fixture.dir() / "paper.stop"));
  // A live run started from this operator file, stopped from another: refused, nothing written anywhere.
  const auto live = start_held(fixture, shared().spec("mid"));
  spit(fixture.dir() / "other.toml", slurp(fixture.config()));
  {
    const ScopedVariable other("CMDATL_CONFIG", (fixture.dir() / "other.toml").string());
    const auto refused = fixture.json({"run", "stop", live});
    CHECK(refused.exit_ == 2);
    CHECK(code_of(refused.out_) == "usage.bad_value");
  }
  CHECK(!std::filesystem::exists(fixture.dir() / "paper.stop"));
  CHECK(!std::filesystem::exists(fixture.dir() / "other.stop"));
  std::filesystem::remove(shared().barrier());
  CHECK(final_status(fixture, live).at("state").as_string() == "ended");
}

TEST_CASE("run log --follow prints each event once across a run's end, and Ctrl-C stops the watching, never the run", "[acceptance][runs]") {
  const auto equip = equipment();
  if (!equip) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(operator_parts());
  const Equipped equipped(equip->first, equip->second);
  fixture.arm();
  {
    const auto run = start_held(fixture, shared().spec("mid"));
    Spawned follower({"run", "log", run, "--follow", "--json"});
    const auto first = follower.line(false, 30);
    REQUIRE(first);
    CHECK(reply_of(*first).at("event").as_string() == "start");
    std::filesystem::remove(shared().barrier());
    const int status = follower.wait(120);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    std::vector<std::string> emitted{"start"};
    const auto rest = lines_of(follower.out());
    REQUIRE(!rest.empty());
    for (std::size_t k = 0; k + 1 < rest.size(); ++k) {
      emitted.push_back(std::string(reply_of(rest[k]).at("event").as_string()));
    }
    CHECK(!reply_of(rest.back()).at("incomplete").as_bool());
    std::vector<std::string> written;
    for (const auto& line : lines_of(slurp(dir_of(fixture, run) / "events.jsonl"))) {
      written.push_back(std::string(reply_of(line).at("event").as_string()));
    }
    CHECK(emitted == written);
    CHECK(written.back() == "end");
  }
  {
    const auto run = start_held(fixture, shared().spec("mid"));
    Spawned follower({"run", "log", run, "--follow", "--json"});
    REQUIRE(follower.line(false, 30));
    follower.signal(SIGINT);
    const int status = follower.wait(30);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 130);
    CHECK(reply_of(fixture.json({"run", "status", run}).out_).at("state").as_string() == "running");
    std::filesystem::remove(shared().barrier());
    CHECK(final_status(fixture, run).at("state").as_string() == "ended");
  }
}
#endif
