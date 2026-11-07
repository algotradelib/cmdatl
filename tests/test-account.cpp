#include "./paper-support.hpp"

#include <cmdatl/account/account.hpp>
#include <cmdatl/account/reads.hpp>
#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace paper = cmdatl::paper;
namespace atl = algotradelib;
namespace runner = algotradelib::runner;

namespace {

/** @brief The account's scope in the fixture. */
const std::string SCOPE = "paper:paper:paper-0001";

/**
 * @brief A placement's journal line.
 * @param id The journal id.
 * @param outcome What the line says.
 * @param at When it was written.
 * @param order_id The broker's id, or empty.
 * @param symbol The symbol.
 * @return The entry.
 */
runner::JournalEntry place_entry(
  const std::string& id, runner::JournalOutcome outcome, paper::Clock::time_point at, const std::string& order_id = "",
  const std::string& symbol = "005930"
) {
  runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = outcome;
  entry.recorded_at_ = at;
  entry.instrument_ = paper::kr(symbol);
  entry.side_ = atl::OrderSide::BUY;
  entry.type_ = atl::OrderType::LIMIT;
  entry.time_in_force_ = atl::TimeInForce::DAY;
  entry.session_ = atl::OrderSession::REGULAR;
  entry.venue_ = atl::Venue::UNSPECIFIED;
  entry.quantity_ = atl::Decimal64(10, 0);
  entry.price_ = atl::Decimal64(78000, 0);
  entry.baseline_ = atl::Decimal64(0, 0);
  entry.order_id_ = order_id;
  return entry;
}

/**
 * @brief Appends entries to the account's journal through the library's writer, which stamps its scope.
 * @param fixture The account.
 * @param entries The entries.
 */
void journal(const PaperFixture& fixture, const std::vector<runner::JournalEntry>& entries) {
  runner::FileJournal file(fixture.journal().string(), SCOPE);
  REQUIRE(file.open());
  for (const auto& entry : entries) {
    REQUIRE(file.append(entry));
  }
}

/**
 * @brief Runs a command and returns its reply.
 * @param fixture The account.
 * @param args The command line.
 * @param exit The exit code expected.
 * @return The reply.
 */
boost::json::object reply(const PaperFixture& fixture, const std::vector<std::string>& args, int exit = 0) {
  const auto ran = fixture.json(args);
  INFO(ran.out_ << ran.err_);
  CHECK(ran.exit_ == exit);
  return reply_of(ran.out_);
}

/**
 * @brief A reply's words.
 * @param value An array of strings.
 * @return The words.
 */
std::vector<std::string> words(const boost::json::value& value) {
  std::vector<std::string> out;
  for (const auto& word : value.as_array()) {
    out.emplace_back(word.as_string());
  }
  return out;
}

/** @brief The hold-lock helper, started on an account's directory, killed if the test leaves it running. */
class Holder {
public:
  /**
   * @brief Starts it and waits until it holds the lock.
   * @param dir The account's directory.
   * @param ms How long it holds.
   */
  Holder(const std::filesystem::path& dir, int ms) {
    std::filesystem::create_directories(dir);
    int out[2];
    REQUIRE(::pipe(out) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    posix_spawn_file_actions_addclose(&actions, out[1]);
    const std::string program = CMDATL_HOLD_LOCK;
    const std::string where = dir.string();
    const std::string time = std::to_string(ms);
    char* argv[] = {
      const_cast<char*>(program.c_str()), const_cast<char*>(where.c_str()), const_cast<char*>(time.c_str()), nullptr
    };
    const int spawned = posix_spawn(&pid_, argv[0], &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    cmdatl::ScopedFd read_end(out[0]);
    REQUIRE(spawned == 0);
    pollfd p{read_end.get(), POLLIN, 0};
    REQUIRE(::poll(&p, 1, 10000) == 1);
    char buffer[64];
    REQUIRE(::read(read_end.get(), buffer, sizeof(buffer)) > 0);
  }

  Holder(const Holder&) = delete;
  Holder& operator=(const Holder&) = delete;

  /** @brief Kills and reaps it. */
  ~Holder() {
    release();
  }

  /** @brief Kills it now, releasing the lock. */
  void release() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }

private:
  pid_t pid_{-1};
};

/**
 * @brief The fixture's account initialized by a first `status`.
 * @param fixture The account.
 */
void initialize(const PaperFixture& fixture) {
  REQUIRE(fixture.json({"status"}).exit_ == 0);
}

} // namespace

TEST_CASE("a fresh account is initialized under the lock: the log's genesis, then the journal") {
  const PaperFixture fixture;
  const auto first = reply(fixture, {"status"});
  CHECK(first.at("env").as_string() == "paper");
  const auto log = slurp(fixture.log());
  CHECK(lines_of(log).size() == 1);
  CHECK(log.rfind("{\"op\":\"genesis\",\"format\":1,\"market\":1,\"fingerprint\":\"", 0) == 0);
  const auto replay = runner::read_journal_file(fixture.journal().string());
  CHECK(replay.has_header_);
  CHECK(replay.scope_ == SCOPE);
  CHECK(replay.entries_.empty());
  const auto owner = reply_of(slurp(fixture.account() / "owner.json"));
  CHECK(owner.at("role").as_string() == "agent");
  CHECK(owner.at("what").as_string() == "status");
  CHECK(owner.at("pid").as_int64() == ::getpid());
  CHECK(std::filesystem::is_regular_file(fixture.account() / "lock"));
  // Opened again and again: nothing more written.
  for (int i = 0; i < 3; ++i) {
    initialize(fixture);
  }
  CHECK(slurp(fixture.log()) == log);
}

TEST_CASE("a first initialization interrupted before the journal is finished by the next command") {
  const PaperFixture fixture;
  initialize(fixture);
  const auto genesis = slurp(fixture.log());
  for (const auto& left : {std::string(), genesis.substr(0, 20), genesis}) {
    INFO("paper.jsonl: " << left);
    std::filesystem::remove_all(fixture.account());
    std::filesystem::create_directories(fixture.account());
    spit(fixture.account() / "lock", "");
    spit(fixture.log(), left);
    initialize(fixture);
    CHECK(slurp(fixture.log()) == genesis);
    CHECK(runner::read_journal_file(fixture.journal().string()).has_header_);
  }
}

TEST_CASE("a journal gone after the account was used, or a log gone beside a journal, is exit 4 and changes nothing") {
  const PaperFixture fixture;
  initialize(fixture);
  REQUIRE(fixture.json({"clock", "advance", "--minutes", "5"}).exit_ == 0);
  const auto log = slurp(fixture.log());
  const auto journal_text = slurp(fixture.journal());
  const auto owner = slurp(fixture.account() / "owner.json");
  const auto expect = [&](const std::string& code, const std::vector<std::string>& args) {
    const auto ran = fixture.json(args);
    INFO(ran.out_);
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == code);
    CHECK(declares({args.front()}, 4, code));
  };
  // The journal removed: every account command refuses, the log untouched, no owner record written.
  std::filesystem::remove(fixture.journal());
  for (
    const auto& args : std::vector<std::vector<std::string>>{{"status"}, {"account"}, {"quote", "005930"}, {"clock"}}
  ) {
    expect("unknown.journal_missing", args);
  }
  expect("unknown.journal_missing", {"clock", "advance", "--minutes", "5"});
  CHECK(slurp(fixture.log()) == log);
  CHECK(slurp(fixture.account() / "owner.json") == owner);
  CHECK(!std::filesystem::exists(fixture.journal()));
  // Emptied.
  spit(fixture.journal(), "");
  expect("unknown.journal_missing", {"status"});
  CHECK(slurp(fixture.journal()).empty());
  // Removed, with only a log of more than a genesis and no owner record.
  std::filesystem::remove(fixture.journal());
  std::filesystem::remove(fixture.account() / "owner.json");
  expect("unknown.journal_missing", {"status"});
  CHECK(!std::filesystem::exists(fixture.account() / "owner.json"));
  CHECK(slurp(fixture.log()) == log);
  // The log removed or emptied beside a journal.
  spit(fixture.journal(), journal_text);
  std::filesystem::remove(fixture.log());
  expect("unknown.paper_state", {"status"});
  CHECK(!std::filesystem::exists(fixture.log()));
  spit(fixture.log(), "");
  expect("unknown.paper_state", {"status"});
  CHECK(slurp(fixture.log()).empty());
  // Restored, it opens again.
  spit(fixture.log(), log);
  initialize(fixture);
}

TEST_CASE("state files that are links or special files are refused before anything destructive") {
  const PaperFixture fixture;
  initialize(fixture);
  const auto log = slurp(fixture.log());
  const auto journal_text = slurp(fixture.journal());
  const auto restore = [&] {
    for (const auto* name : {"paper.jsonl", "journal.jsonl", "lock", "owner.json"}) {
      std::filesystem::remove(fixture.account() / name);
    }
    spit(fixture.log(), log);
    spit(fixture.journal(), journal_text);
  };
  const auto expect = [&](int exit, const std::string& code) {
    const auto ran = fixture.json({"status"});
    INFO(ran.out_);
    CHECK(ran.exit_ == exit);
    CHECK(code_of(ran.out_) == code);
  };
  // The log a symbolic link (a torn tail on its target, which must not be cut).
  spit(fixture.dir() / "elsewhere", log + "{\"op\":");
  std::filesystem::remove(fixture.log());
  std::filesystem::create_symlink(fixture.dir() / "elsewhere", fixture.log());
  expect(4, "unknown.paper_state");
  CHECK(slurp(fixture.dir() / "elsewhere") == log + "{\"op\":");
  restore();
  // The journal a symbolic link.
  spit(fixture.dir() / "journal-elsewhere", journal_text);
  std::filesystem::remove(fixture.journal());
  std::filesystem::create_symlink(fixture.dir() / "journal-elsewhere", fixture.journal());
  expect(4, "unknown.journal_damaged");
  restore();
  // The log and the journal two names of one file.
  spit(fixture.log(), log + "{\"op\":");
  std::filesystem::remove(fixture.journal());
  std::filesystem::create_hard_link(fixture.log(), fixture.journal());
  expect(4, "unknown.journal_damaged");
  CHECK(slurp(fixture.log()) == log + "{\"op\":");
  restore();
  // FIFOs, without blocking.
  {
    const Watchdog watchdog("FIFO state files", 30);
    std::filesystem::remove(fixture.log());
    REQUIRE(::mkfifo(fixture.log().c_str(), 0600) == 0);
    expect(4, "unknown.paper_state");
    restore();
    std::filesystem::remove(fixture.journal());
    REQUIRE(::mkfifo(fixture.journal().c_str(), 0600) == 0);
    expect(4, "unknown.journal_damaged");
    restore();
  }
  // The lock a second name of the owner record.
  spit(fixture.account() / "owner.json", "{}");
  std::filesystem::remove(fixture.account() / "lock");
  std::filesystem::create_hard_link(fixture.account() / "owner.json", fixture.account() / "lock");
  expect(1, "state.unavailable");
  CHECK(slurp(fixture.account() / "owner.json") == "{}");
  restore();
  // The lock a symbolic link.
  std::filesystem::create_symlink(fixture.dir() / "lock-elsewhere", fixture.account() / "lock");
  expect(1, "state.unavailable");
  CHECK(!std::filesystem::exists(fixture.dir() / "lock-elsewhere"));
  restore();
  initialize(fixture);
  CHECK(slurp(fixture.log()) == log);
}

TEST_CASE("a stray owner temporary is never truncated, and the record is skipped") {
  const PaperFixture fixture;
  initialize(fixture);
  const auto owner = slurp(fixture.account() / "owner.json");
  const auto temporary = fixture.account() / ("owner." + std::to_string(::getpid()) + ".tmp");
  // A stray temporary with this process's id: the record is skipped, the account opens.
  spit(temporary, "stray");
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  initialize(fixture);
  CHECK(slurp(temporary) == "stray");
  CHECK(slurp(fixture.account() / "owner.json") == owner);
  // Such a temporary a second name of the journal: the journal, no longer one name, is refused before anything is
  // written, and its bytes stay.
  std::filesystem::remove(temporary);
  std::filesystem::create_hard_link(fixture.journal(), temporary);
  const auto journal_text = slurp(fixture.journal());
  REQUIRE(!journal_text.empty());
  const auto refused = fixture.json({"status"});
  CHECK(refused.exit_ == 4);
  CHECK(code_of(refused.out_) == "unknown.journal_damaged");
  CHECK(slurp(fixture.journal()) == journal_text);
  // A second name of a file outside the account: exclusive creation fails, so it is never truncated.
  std::filesystem::remove(temporary);
  spit(fixture.dir() / "precious", "precious bytes");
  std::filesystem::create_hard_link(fixture.dir() / "precious", temporary);
  initialize(fixture);
  CHECK(slurp(fixture.dir() / "precious") == "precious bytes");
  CHECK(slurp(fixture.account() / "owner.json") == owner);
  std::filesystem::remove(temporary);
  initialize(fixture);
  CHECK(!std::filesystem::exists(temporary));
  CHECK(slurp(fixture.account() / "owner.json") != owner);
}

TEST_CASE("the journal's view: today's placements, what is owed, and what the market shows of it") {
  const PaperFixture fixture;
  fixture.arm();
  initialize(fixture);
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    auto& desk = opened->account_->desk();
    REQUIRE(desk.advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
    REQUIRE(desk.session().place(order("005930", true, "10", "60000")));
  }
  journal(
    fixture,
    {
      // Yesterday's, settled, which does not count today.
      place_entry("j0", runner::JournalOutcome::PENDING, kst("2024-03-25T10:00")),
      place_entry("j0", runner::JournalOutcome::REJECTED, kst("2024-03-25T10:00")),
      place_entry("j1", runner::JournalOutcome::PENDING, kst("2024-03-26T08:59")),
      place_entry("j1", runner::JournalOutcome::ACCEPTED, kst("2024-03-26T09:00"), "1"),
      // A regression: an order whose pending line alone survives is still owed.
      place_entry("j2", runner::JournalOutcome::PENDING, kst("2024-03-26T09:01"), "", "000660"),
    }
  );
  const auto body = reply(fixture, {"account"});
  CHECK(body.at("orders_left_today").as_int64() == 1);
  const auto& owed = body.at("owed").as_array();
  const auto row = [&owed](const std::string& id) {
    for (const auto& r : owed) {
      if (r.as_object().at("journal_id").as_string() == id) {
        return r.as_object();
      }
    }
    FAIL("no owed row " << id);
    return boost::json::object{};
  };
  CHECK(row("j1").at("state").as_string() == "named");
  CHECK(row("j1").at("order_id").as_string() == "1");
  CHECK(row("j1").at("broker_status").as_string() == "pending");
  CHECK(row("j2").at("state").as_string() == "unnamed");
  CHECK(row("j2").at("broker_status").is_null());
  CHECK(row("j2").at("next").as_string() == cmdatl::owed_next("000660", "unnamed", std::nullopt));
  CHECK(row("j2").at("next").as_string().find("cmdatl order release 000660") != std::string::npos);
  CHECK(row("j1").at("next").as_string().find("cmdatl order cancel 005930") != std::string::npos);
  // Listing them settles nothing: the journal is byte for byte what it was.
  const auto journal_text = slurp(fixture.journal());
  reply(fixture, {"status"});
  CHECK(slurp(fixture.journal()) == journal_text);
  CHECK(reply(fixture, {"account", "--symbol", "005930"}).at("owed").as_array().size() == 1);
  // More placements than allowed: zero, never a wrapped count.
  journal(
    fixture,
    {
      place_entry("j3", runner::JournalOutcome::PENDING, kst("2024-03-26T09:02")),
      place_entry("j4", runner::JournalOutcome::PENDING, kst("2024-03-26T09:03")),
    }
  );
  const auto over = reply(fixture, {"status"});
  CHECK(over.at("orders_left_today").as_int64() == 0);
  CHECK(words(over.at("not_accepted")) == std::vector<std::string>{"daily_orders_used"});
  CHECK(!over.at("orders_accepted_now").as_bool());
}

TEST_CASE("the day's count turns over at 00:00 KST, not at 00:00 UTC") {
  const PaperFixture fixture;
  initialize(fixture);
  journal(
    fixture,
    {
      place_entry("late", runner::JournalOutcome::PENDING, kst("2024-03-25T23:30")),
      place_entry("early", runner::JournalOutcome::PENDING, kst("2024-03-26T00:30")),
    }
  );
  // 2024-03-25T16:00 KST: the late one is today's.
  CHECK(reply(fixture, {"account"}).at("orders_left_today").as_int64() == 2);
  // 00:10 KST on the 26th is still the 25th in UTC; the early one is today's now, the late one is not.
  REQUIRE(fixture.json({"clock", "advance", "--to", "2024-03-26T00:10"}).exit_ == 0);
  CHECK(reply(fixture, {"account"}).at("orders_left_today").as_int64() == 2);
  const auto snapshot = cmdatl::load_operator_config();
  CHECK(paper::kst_day(kst("2024-03-26T00:10")) == paper::kst_day(kst("2024-03-26T00:30")));
  CHECK(paper::utc_day(kst("2024-03-26T00:10")) == paper::utc_day(kst("2024-03-25T23:30")));
  journal(fixture, {place_entry("third", runner::JournalOutcome::PENDING, kst("2024-03-26T00:05"))});
  CHECK(reply(fixture, {"account"}).at("orders_left_today").as_int64() == 1);
}

TEST_CASE("a journal that is not this account's, or damaged, makes every account command exit 4 and changes nothing") {
  const PaperFixture fixture;
  fixture.arm();
  initialize(fixture);
  REQUIRE(fixture.json({"clock", "advance", "--to", "2024-03-26T09:00"}).exit_ == 0);
  const auto good = slurp(fixture.journal());
  const auto header = lines_of(good).front();
  auto foreign_entry = place_entry("f1", runner::JournalOutcome::PENDING, kst("2024-03-26T08:00"));
  foreign_entry.scope_ = "paper:paper:other";
  auto unscoped_entry = place_entry("u1", runner::JournalOutcome::PENDING, kst("2024-03-26T08:00"));
  unscoped_entry.scope_.clear();
  const std::vector<std::pair<const char*, std::string>> damaged{
    {"another account's", runner::journal_header_line("paper:paper:other") + "\n"},
    {"a scope differing in case", runner::journal_header_line("PAPER:paper:paper-0001") + "\n"},
    {"no header", runner::journal_entry_to_line(unscoped_entry) + "\n"},
    {"a foreign entry beneath the right header", good + runner::journal_entry_to_line(foreign_entry) + "\n"},
    {"a damaged line under an understated count", good + "not a journal line\n"},
    {"a torn last line", good + "{\"journal\":"},
  };
  const auto log = slurp(fixture.log());
  for (const auto& entry : damaged) {
    INFO(entry.first);
    spit(fixture.journal(), entry.second);
    const auto status = fixture.json({"status"});
    CHECK(status.exit_ == 4);
    CHECK(code_of(status.out_) == "unknown.journal_damaged");
    const auto body = reply_of(status.out_);
    // The read keeps its body, readiness never affirmative, no foreign obligation shown.
    CHECK(body.at("orders_left_today").is_null());
    CHECK(!body.at("orders_accepted_now").as_bool());
    CHECK(words(body.at("not_accepted")).front() == "orders_left_unknown");
    CHECK(body.at("owed").as_array().empty());
    CHECK(body.contains("positions"));
    const auto quote = reply_of(fixture.json({"quote", "005930"}).out_);
    CHECK(quote.at("error").as_object().at("code").as_string() == "unknown.journal_damaged");
    CHECK(!quote.at("quotes").as_array()[0].as_object().at("orders_accepted_now").as_bool());
    // A symbol without bars: the journal's unknown state first, the symbol named, no partial quotes.
    const auto missing = fixture.json({"quote", "005930", "MISSING"});
    CHECK(missing.exit_ == 4);
    const auto missing_reply = reply_of(missing.out_);
    CHECK(missing_reply.at("error").as_object().at("code").as_string() == "unknown.journal_damaged");
    CHECK(
      std::string(missing_reply.at("error").as_object().at("message").as_string()).find("MISSING") != std::string::npos
    );
    CHECK(!missing_reply.contains("quotes"));
    const auto advance = fixture.json({"clock", "advance", "--minutes", "5"});
    CHECK(advance.exit_ == 4);
    CHECK(code_of(advance.out_) == "unknown.journal_damaged");
    CHECK(slurp(fixture.log()) == log);
    CHECK(slurp(fixture.journal()) == entry.second);
    for (const char* command : {"status", "account", "quote", "clock"}) {
      CHECK(declares({command}, 4, "unknown.journal_damaged"));
    }
    CHECK(declares({"clock", "advance"}, 4, "unknown.journal_damaged"));
  }
  // The writer's own stamped entries, and an entry with no scope beneath the right header, are this account's.
  spit(fixture.journal(), good);
  journal(fixture, {place_entry("s1", runner::JournalOutcome::PENDING, kst("2024-03-26T08:00"))});
  const auto stamped = slurp(fixture.journal());
  CHECK(stamped.find("\"scope\":\"" + SCOPE + "\"", header.size()) != std::string::npos);
  CHECK(reply(fixture, {"status"}).at("owed").as_array().size() == 1);
  spit(fixture.journal(), stamped + runner::journal_entry_to_line(unscoped_entry) + "\n");
  CHECK(reply(fixture, {"status"}).at("owed").as_array().size() == 2);
  // Unreadable: refused the same way.
  ::chmod(fixture.journal().c_str(), 0200);
  if (::access(fixture.journal().c_str(), R_OK) != 0) {
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 4);
    CHECK(code_of(ran.out_) == "unknown.journal_damaged");
  }
  ::chmod(fixture.journal().c_str(), 0600);
}

TEST_CASE("a journal not established leaves the paper log unrepaired and writes no owner record") {
  const PaperFixture fixture;
  fixture.arm();
  initialize(fixture);
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    auto& desk = opened->account_->desk();
    REQUIRE(desk.advance_to(kst("2024-03-26T09:00")).status_ == paper::Appended::OK);
    REQUIRE(desk.session().place(order("005930", true, "10", "60000")));
  }
  const auto placed = slurp(fixture.log());
  auto lines = lines_of(placed);
  REQUIRE(lines.size() == 4);
  REQUIRE(lines.back().find("\"op\":\"result\"") != std::string::npos);
  lines.pop_back();
  std::string unanswered;
  for (const auto& line : lines) {
    unanswered += line + "\n";
  }
  const auto good = slurp(fixture.journal());
  auto foreign_entry = place_entry("f1", runner::JournalOutcome::PENDING, kst("2024-03-26T08:00"));
  foreign_entry.scope_ = "paper:paper:other";
  const std::vector<std::pair<const char*, std::string>> journals{
    {"a foreign header", runner::journal_header_line("paper:paper:other") + "\n"},
    {"a foreign entry", good + runner::journal_entry_to_line(foreign_entry) + "\n"},
    {"a damaged line", good + "not a journal line\n"},
  };
  const std::vector<std::pair<const char*, std::string>> logs{
    {"a torn paper tail", placed + "{\"op\":\"adv"},
    {"an unanswered input", unanswered},
  };
  const auto owner = fixture.account() / "owner.json";
  const std::string record = "{\"role\":\"operator\",\"pid\":1}\n";
  for (const auto& j : journals) {
    for (const auto& l : logs) {
      INFO(j.first << " with " << l.first);
      spit(fixture.journal(), j.second);
      spit(fixture.log(), l.second);
      spit(owner, record);
      for (
        const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
          {"status"}, {"account"}, {"quote", "005930"}, {"clock"}, {"clock", "advance", "--minutes", "5"}
        }
      ) {
        INFO(args.front());
        const auto ran = fixture.json(args);
        CHECK(ran.exit_ == 4);
        CHECK(code_of(ran.out_) == "unknown.journal_damaged");
        CHECK(slurp(fixture.log()) == l.second);
        CHECK(slurp(fixture.journal()) == j.second);
        CHECK(slurp(owner) == record);
      }
    }
  }
  // Beneath this account's journal, the same logs are repaired and the record written.
  for (const auto& l : logs) {
    INFO(l.first);
    spit(fixture.journal(), good);
    spit(fixture.log(), l.second);
    spit(owner, record);
    CHECK(fixture.json({"status"}).exit_ == 0);
    CHECK(slurp(fixture.log()) == placed);
    CHECK(slurp(fixture.journal()) == good);
    CHECK(slurp(owner) != record);
  }
}

namespace {

/**
 * @brief An operator-only placement line with its `at` replaced and no `at_text`, its checksum made again by the
 *   library's own helpers.
 * @param at The microseconds `at` states.
 * @return The line.
 */
std::string operator_only_line(std::int64_t at) {
  auto entry = place_entry("op1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:00"));
  entry.requires_operator_ = true;
  entry.scope_ = SCOPE;
  auto line = runner::journal_entry_to_line(entry);
  line = line.substr(0, line.size() - runner::journal_detail::kChecksumSuffixSize) + "}";
  auto row = boost::json::parse(line).as_object();
  row["at"] = at;
  row.erase("at_text");
  line = boost::json::serialize(row);
  const auto checksum = runner::journal_detail::checksum_text(runner::journal_detail::line_checksum(line));
  line.pop_back();
  line += std::string(runner::journal_detail::kChecksumMember) + checksum + "\"}";
  return line;
}

} // namespace

TEST_CASE("an operator-only line at an instant outside the interval makes the count unknown, never an overflow") {
  const PaperFixture fixture;
  fixture.arm();
  initialize(fixture);
  const auto good = slurp(fixture.journal());
  const auto log = slurp(fixture.log());
  const auto micros = [](paper::Clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count();
  };
  const std::vector<std::pair<const char*, std::int64_t>> extreme{
    {"INT64_MAX", std::numeric_limits<std::int64_t>::max()},
    {"INT64_MIN", std::numeric_limits<std::int64_t>::min()},
    {"-1", -1},
    {"just before 1990 KST", micros(paper::first_instant()) - 1},
    {"2101 KST", micros(paper::end_instant())},
  };
  for (const auto& c : extreme) {
    INFO(c.first);
    const auto line = operator_only_line(c.second);
    // The library itself refuses an instant outside 1970-2200; cmdatl's narrower interval catches the rest.
    const bool read = runner::journal_entry_from_line(line).has_value();
    const auto text = good + line + "\n";
    spit(fixture.journal(), text);
    for (const char* command : {"status", "account"}) {
      const auto ran = fixture.json({command});
      INFO(ran.out_);
      CHECK(ran.exit_ == 4);
      CHECK(code_of(ran.out_) == "unknown.journal_damaged");
      CHECK(reply_of(ran.out_).at("orders_left_today").is_null());
      if (read) {
        CHECK(ran.out_.find("outside 1990 to 2100") != std::string::npos);
      }
    }
    CHECK(slurp(fixture.journal()) == text);
    CHECK(slurp(fixture.log()) == log);
  }
  // A salvaged line at the epoch is no placement on any date: not counted, never an error of its own.
  {
    cmdatl::JournalView view;
    auto replay = runner::read_journal_file(fixture.journal().string());
    replay.entries_.clear();
    auto salvaged = place_entry("s~damaged", runner::JournalOutcome::ACCEPTED, paper::Clock::time_point{});
    salvaged.requires_operator_ = true;
    auto counted = place_entry("c1", runner::JournalOutcome::ACCEPTED, kst("2024-03-25T10:00"));
    auto far = place_entry("x1", runner::JournalOutcome::ACCEPTED, paper::Clock::time_point::max());
    far.requires_operator_ = true;
    const auto opened = open_account();
    REQUIRE(opened->account_);
    auto& desk = opened->account_->desk();
    const auto today = paper::kst_day(desk.now());
    replay.entries_ = {counted};
    const auto alone = cmdatl::journal_view(desk.session(), replay, SCOPE, today);
    CHECK(alone.placed_today_ == 1);
    replay.entries_ = {salvaged, counted};
    view = cmdatl::journal_view(desk.session(), replay, SCOPE, today);
    CHECK(view.placed_today_ == 1);
    CHECK(view.intact_ == alone.intact_);
    replay.entries_ = {salvaged, counted, far};
    view = cmdatl::journal_view(desk.session(), replay, SCOPE, today);
    CHECK(!view.intact_);
    CHECK(view.placed_today_ == 1);
    CHECK(!cmdatl::orders_left(opened->snapshot_.config_.value(), view));
  }
  {
    const auto text = good + operator_only_line(0) + "\n";
    spit(fixture.journal(), text);
    const auto ran = fixture.json({"status"});
    INFO(ran.out_);
    CHECK(ran.exit_ == 0);
    CHECK(slurp(fixture.journal()) == text);
    CHECK(slurp(fixture.log()) == log);
  }
}

TEST_CASE("the journal's bytes from the library's writer are the fixture's") {
  auto entry = place_entry("j1", runner::JournalOutcome::PENDING, kst("2024-03-26T09:00"));
  entry.scope_ = SCOPE;
  const auto text = runner::journal_header_line(SCOPE) + "\n" + runner::journal_entry_to_line(entry) + "\n";
  const auto fixture_path = std::filesystem::path(CMDATL_SOURCE_DIR) / "tests" / "fixtures" / "journal.jsonl";
  if (std::getenv("CMDATL_TEST_RECORD") != nullptr) {
    spit(fixture_path, text);
  }
  CHECK(slurp(fixture_path) == text);
}

TEST_CASE("readiness: one view of armed, the day's count, the phase and the market's own hours") {
  SECTION("pre_open at 08:00 is permitted, but the market is closed") {
    ConfigParts parts;
    parts.limits_ += "phases = [\"pre_open\"]\n";
    parts.paper_ = "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-26T08:00\"\n";
    const PaperFixture fixture(parts);
    fixture.arm();
    const auto body = reply(fixture, {"status"});
    CHECK(body.at("phase").as_string() == "pre_open");
    CHECK(!body.at("orders_accepted_now").as_bool());
    CHECK(words(body.at("not_accepted")) == std::vector<std::string>{"market_closed"});
  }
  SECTION("post_close at 16:00 the same") {
    ConfigParts parts;
    parts.limits_ += "phases = [\"post_close\"]\n";
    const PaperFixture fixture(parts);
    fixture.arm();
    const auto body = reply(fixture, {"status"});
    CHECK(body.at("phase").as_string() == "post_close");
    CHECK(words(body.at("not_accepted")) == std::vector<std::string>{"market_closed"});
  }
  SECTION("unarmed, wrong phase, closed: every gate named in order; all passing, none") {
    ConfigParts parts;
    parts.paper_ = "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-26T10:00\"\n";
    const PaperFixture fixture(parts);
    auto body = reply(fixture, {"status"});
    CHECK(words(body.at("not_accepted")) == std::vector<std::string>{"not_armed"});
    fixture.arm();
    body = reply(fixture, {"status"});
    CHECK(body.at("orders_accepted_now").as_bool());
    CHECK(body.at("not_accepted").as_array().empty());
    for (const auto& quote : body.at("quotes").as_array()) {
      CHECK(quote.as_object().at("orders_accepted_now").as_bool());
      CHECK(quote.as_object().at("not_accepted").as_array().empty());
    }
    // On the 27th only 005930 trades: the account accepts, 000660's quote does not.
    REQUIRE(fixture.json({"clock", "advance", "--to", "2024-03-27T10:00"}).exit_ == 0);
    body = reply(fixture, {"status"});
    CHECK(body.at("orders_accepted_now").as_bool());
    const auto quotes = reply(fixture, {"quote", "005930", "000660"}).at("quotes").as_array();
    CHECK(quotes[0].as_object().at("orders_accepted_now").as_bool());
    CHECK(words(quotes[1].as_object().at("not_accepted")) == std::vector<std::string>{"market_closed"});
    REQUIRE(fixture.json({"clock", "advance", "--to", "2024-03-27T20:00"}).exit_ == 0);
    body = reply(fixture, {"status"});
    CHECK(words(body.at("not_accepted")) == std::vector<std::string>{"phase", "market_closed"});
    std::filesystem::remove(fixture.armed());
    body = reply(fixture, {"status"});
    CHECK(words(body.at("not_accepted")) == std::vector<std::string>{"not_armed", "phase", "market_closed"});
  }
  SECTION("a symbol before its first session has no price and no band, and that is not an error") {
    const std::string bars =
      "symbol,date,open,high,low,close,volume\n"
      "005930,2024-03-25,78000,79000,77500,78500,1\n"
      "005930,2024-03-26,78500,80000,78000,79800,1\n"
      "000660,2024-03-27,180000,182000,178000,181000,1\n";
    const PaperFixture fixture(ConfigParts{}, bars);
    fixture.arm();
    auto quote = reply(fixture, {"quote", "000660"}).at("quotes").as_array()[0].as_object();
    CHECK(quote.at("last_price").is_null());
    CHECK(quote.at("upper_limit").is_null());
    CHECK(quote.at("lower_limit").is_null());
    CHECK(words(quote.at("not_accepted")).back() == "no_price");
    // At its first open it has a price and still no band.
    REQUIRE(fixture.json({"clock", "advance", "--to", "2024-03-27T09:00"}).exit_ == 0);
    quote = reply(fixture, {"quote", "000660"}).at("quotes").as_array()[0].as_object();
    CHECK(quote.at("last_price").as_string() == "180000");
    CHECK(quote.at("upper_limit").is_null());
    CHECK(quote.at("orders_accepted_now").as_bool());
  }
}

TEST_CASE("the account commands' replies, in every format, with every declared field selectable") {
  ConfigParts parts;
  parts.paper_ = "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-26T09:00\"\n";
  const PaperFixture fixture(parts, BARS + "035720,2024-03-26,50000,51000,49000,50500,1\n");
  fixture.arm();
  initialize(fixture);
  {
    const auto opened = open_account();
    REQUIRE(opened->account_);
    REQUIRE(opened->account_->desk().session().place(order("005930", true, "10", "78500")));
    REQUIRE(opened->account_->desk().session().place(order("005930", true, "5", "60000")));
  }
  const cmdatl::Table table(cmdatl::command_list());
  const std::vector<std::vector<std::string>> commands{
    {"status"}, {"account"}, {"quote", "005930"}, {"order", "list"}, {"clock"}
  };
  for (const auto& args : commands) {
    INFO(cmdatl::join(args));
    const auto body = reply(fixture, args);
    CHECK(body.at("env").as_string() == "paper");
    const auto& command = *table.resolve(args).command_;
    for (const auto& field : command.returns_) {
      INFO(field.first);
      CHECK(body.contains(field.first));
      auto with_value = args;
      with_value.insert(with_value.end(), {"--value", field.first});
      CHECK(fixture.json(with_value).exit_ == 0);
      auto with_fields = args;
      with_fields.insert(with_fields.end(), {"--fields", field.first});
      const auto selected = reply(fixture, with_fields);
      CHECK(selected.contains(field.first));
    }
    auto text = args;
    text.insert(text.begin(), {"--format", "text"});
    const auto in_text = support::run(text);
    CHECK(in_text.exit_ == 0);
    CHECK(in_text.out_.find("now: ") != std::string::npos);
    auto jsonl = args;
    jsonl.insert(jsonl.begin(), {"--format", "jsonl"});
    const auto in_jsonl = support::run(jsonl);
    CHECK(in_jsonl.exit_ == 0);
    CHECK(lines_of(in_jsonl.out_).size() == 1);
  }
  // The values themselves.
  const auto orders = reply(fixture, {"order", "list"}).at("orders").as_array();
  REQUIRE(orders.size() == 2);
  CHECK(orders[0].as_object().at("order_id").as_string() == "1");
  CHECK(orders[0].as_object().at("price").as_string() == "78500");
  CHECK(orders[0].as_object().at("status").as_string() == "pending");
  const auto quotes = reply(fixture, {"quote", "005930", "035720"}).at("quotes").as_array();
  REQUIRE(quotes.size() == 2);
  CHECK(!quotes[1].as_object().at("symbol_allowed").as_bool());
  CHECK(words(quotes[1].as_object().at("not_accepted")) == std::vector<std::string>{"symbol_not_allowed"});
  const auto missing = fixture.json({"quote", "005930", "NOPE", "ALSO"});
  CHECK(missing.exit_ == 1);
  CHECK(code_of(missing.out_) == "quote.no_data");
  CHECK(
    reply_of(missing.out_).at("error").as_object().at("message").as_string().find("NOPE, ALSO") != std::string::npos
  );
  CHECK(!reply_of(missing.out_).contains("quotes"));
  CHECK(declares({"quote"}, 1, "quote.no_data"));
  for (const auto& bad : std::vector<std::vector<std::string>>{{"quote", "00 59"}, {"account", "--symbol", "a/b"}}) {
    const auto ran = fixture.json(bad);
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "usage.bad_value");
    CHECK(reply_of(ran.out_).at("env").as_string() == "offline");
    CHECK(declares({bad.front()}, 2, "usage.bad_value"));
  }
  // The clock across days: events dated and with their offset.
  const auto advanced = reply(fixture, {"clock", "advance", "--hours", "30"});
  CHECK(advanced.at("from").as_string() == "2024-03-26T09:00:00+09:00");
  CHECK(advanced.at("now").as_string() == "2024-03-27T15:00:00+09:00");
  const auto events = advanced.at("events").as_array();
  REQUIRE(events.size() == 2);
  CHECK(events[0].as_object().at("kind").as_string() == "filled");
  CHECK(events[0].as_object().at("at").as_string().rfind("2024-03-26T09:0", 0) == 0);
  CHECK(events[0].as_object().contains("commission"));
  CHECK(events[0].as_object().at("tax").as_string() == "0");
  CHECK(events[1].as_object().at("kind").as_string() == "expired");
  CHECK(events[1].as_object().at("at").as_string() == "2024-03-26T15:30:00+09:00");
  CHECK(!events[1].as_object().contains("price"));
  const auto account = reply(fixture, {"account", "--symbol", "005930"});
  CHECK(account.at("positions").as_array().size() == 1);
  CHECK(account.at("positions").as_array()[0].as_object().at("quantity").as_string() == "10");
  CHECK(reply(fixture, {"account", "--symbol", "000660"}).at("positions").as_array().empty());
  CHECK(reply(fixture, {"clock", "advance", "--minutes", "5", "--fields", "from,now,phase,events"}).contains("events"));
  // env and schema stay offline.
  CHECK(reply(fixture, {"env"}).at("env").as_string() == "offline");
  CHECK(reply(fixture, {"schema", "status"}).at("env").as_string() == "offline");
}

TEST_CASE("clock advance: exactly one option, checked before the account opens, and its refusals") {
  ConfigParts parts;
  const std::string gap_bars =
    "symbol,date,open,high,low,close,volume\n"
    "005930,2024-03-25,78000,79000,77500,78500,1\n"
    "000660,2024-03-25,180000,182000,178000,181000,1\n"
    "005930,2024-04-20,78500,80000,78000,79800,1\n";
  const PaperFixture fixture(parts, gap_bars);
  initialize(fixture);
  const auto log = slurp(fixture.log());
  const std::vector<std::pair<std::vector<std::string>, std::string>> usage{
    {{"clock", "advance"}, "usage.bad_option"},
    {{"clock", "advance", "--minutes", "5", "--hours", "1"}, "usage.bad_option"},
    {{"clock", "advance", "--minutes", "0"}, "usage.bad_value"},
    {{"clock", "advance", "--minutes", "20161"}, "usage.bad_value"},
    {{"clock", "advance", "--minutes", "9223372036854775807"}, "usage.bad_value"},
    {{"clock", "advance", "--minutes", "1.5"}, "usage.bad_value"},
    {{"clock", "advance", "--hours", "337"}, "usage.bad_value"},
    {{"clock", "advance", "--to", "2024-03-26 09:00"}, "usage.bad_value"},
    {{"clock", "advance", "--to", "2101-01-01T00:00"}, "usage.bad_value"},
  };
  for (const auto& entry : usage) {
    INFO(cmdatl::join(entry.first));
    const auto ran = fixture.json(entry.first);
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == entry.second);
    CHECK(declares({"clock", "advance"}, 2, entry.second));
  }
  const auto backwards = fixture.json({"clock", "advance", "--to", "2024-03-25T15:00"});
  CHECK(backwards.exit_ == 1);
  CHECK(code_of(backwards.out_) == "clock.backwards");
  const auto past = fixture.json({"clock", "advance", "--to", "2024-04-21T15:31"});
  CHECK(past.exit_ == 1);
  CHECK(code_of(past.out_) == "clock.past_data");
  CHECK(slurp(fixture.log()) == log);
  // --next-open finds the next stored session across a 26-day gap.
  const auto next = reply(fixture, {"clock", "advance", "--next-open"});
  CHECK(next.at("now").as_string() == "2024-04-20T09:00:00+09:00");
  CHECK(next.at("phase").as_string() == "open");
  // The last session's close plus a day is as far as it goes; then no session is left.
  CHECK(
    reply(fixture, {"clock", "advance", "--to", "2024-04-21T15:30"}).at("now").as_string() ==
    "2024-04-21T15:30:00+09:00"
  );
  const auto none = fixture.json({"clock", "advance", "--next-open"});
  CHECK(none.exit_ == 1);
  CHECK(code_of(none.out_) == "clock.past_data");
  CHECK(declares({"clock", "advance"}, 1, "clock.past_data"));
  CHECK(declares({"clock", "advance"}, 1, "clock.backwards"));
}

TEST_CASE("the clock at the supported interval's end") {
  ConfigParts parts;
  parts.paper_ = "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2100-12-30T16:00\"\n";
  const std::string bars =
    "symbol,date,open,high,low,close,volume\n"
    "005930,2100-12-30,78000,79000,77500,78500,1\n"
    "000660,2100-12-30,180000,182000,178000,181000,1\n"
    "005930,2100-12-31,78500,80000,78000,79800,1\n";
  const PaperFixture fixture(parts, bars);
  CHECK(
    reply(fixture, {"clock", "advance", "--to", "2100-12-31T23:59"}).at("now").as_string() ==
    "2100-12-31T23:59:00+09:00"
  );
  CHECK(code_of(fixture.json({"clock", "advance", "--minutes", "1"}).out_) == "clock.past_data");
  CHECK(reply(fixture, {"status"}).at("data_end").as_string() == "2100-12-31T15:30:00+09:00");
}

TEST_CASE("the lock: a waiting command says who holds it, then answers busy.locked at 10 s") {
  const Watchdog watchdog("the lock's wait", 60);
  const PaperFixture fixture;
  initialize(fixture);
  Holder holder(fixture.account(), 30000);
  Spawned cmdatl({"--json", "status"});
  const auto first = cmdatl.line(true, 5);
  REQUIRE(first.has_value());
  CHECK(first->find("waiting for the account") != std::string::npos);
  CHECK(cmdatl.out().empty());
  const auto start = std::chrono::steady_clock::now();
  const int status = cmdatl.wait(30);
  const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 1);
  CHECK(code_of(cmdatl.out()) == "busy.locked");
  CHECK(reply_of(cmdatl.out()).at("error").as_object().at("retry").as_bool());
  CHECK(took > 8.0);
  CHECK(declares({"status"}, 1, "busy.locked"));
}

TEST_CASE("the lock: SIGINT while waiting is exit 130 with one interrupted reply") {
  const Watchdog watchdog("an interrupted wait", 60);
  const PaperFixture fixture;
  initialize(fixture);
  Holder holder(fixture.account(), 30000);
  const auto log = slurp(fixture.log());
  Spawned cmdatl({"--json", "clock", "advance", "--minutes", "5"});
  REQUIRE(cmdatl.line(true, 5).has_value());
  cmdatl.signal(SIGINT);
  const int status = cmdatl.wait(10);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 130);
  const auto out = lines_of(cmdatl.out());
  REQUIRE(out.size() == 1);
  CHECK(code_of(out.front()) == "interrupted");
  CHECK(slurp(fixture.log()) == log);
  CHECK(declares({"clock", "advance"}, 130, "interrupted"));
}

TEST_CASE("the lock: replaced while a command waits, the waiter locks the new file") {
  const Watchdog watchdog("a lock replaced during the wait", 60);
  const PaperFixture fixture;
  initialize(fixture);
  Holder holder(fixture.account(), 30000);
  Spawned cmdatl({"--json", "status"});
  REQUIRE(cmdatl.line(true, 5).has_value());
  std::filesystem::rename(fixture.account() / "lock", fixture.account() / "lock.old");
  spit(fixture.account() / "lock", "");
  const int status = cmdatl.wait(10);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  std::filesystem::remove(fixture.account() / "lock.old");
}

TEST_CASE("the lock: replaced after a holder took it, the holder's next write and its reply refuse") {
  const Watchdog watchdog("a lock replaced after acquisition", 60);
  const PaperFixture fixture;
  initialize(fixture);
  const auto opened = open_account();
  REQUIRE(opened->account_);
  auto& account = *opened->account_;
  std::filesystem::rename(fixture.account() / "lock", fixture.account() / "lock.old");
  spit(fixture.account() / "lock", "");
  // A second process locks the new file and goes on.
  Spawned other({"--json", "status"});
  const int status = other.wait(15);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  // The first holder's write refuses, appending nothing.
  const auto log = slurp(fixture.log());
  CHECK(!account.lock().still_at_path());
  const cmdatl::Invocation invocation{{{"minutes", {"5"}}}};
  const auto refused = cmdatl::advance_reply(invocation, account, account.desk(), opened->output_);
  CHECK(code_of_reply(refused) == "unknown.lock_lost");
  CHECK(refused.exit_ == cmdatl::Exit::UNKNOWN);
  CHECK(slurp(fixture.log()) == log);
  CHECK(declares({"clock", "advance"}, 4, "unknown.lock_lost"));
}

TEST_CASE("two operator files naming one account wait on one lock") {
  const Watchdog watchdog("two operator files", 60);
  const PaperFixture fixture;
  initialize(fixture);
  const auto second = fixture.dir() / "other.toml";
  spit(second, ConfigParts{}.text());
  const auto opened = open_account();
  REQUIRE(opened->account_);
  const ScopedVariable named("CMDATL_CONFIG", second.string());
  Spawned cmdatl({"--json", "status"});
  const auto first = cmdatl.line(true, 5);
  REQUIRE(first.has_value());
  CHECK(first->find("waiting for the account") != std::string::npos);
  opened->account_.reset();
  const int status = cmdatl.wait(15);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(reply_of(cmdatl.out()).at("state_dir").as_string() == fixture.account().string());
}

TEST_CASE("twenty concurrent advances and reads end exactly twenty minutes on, with no torn line") {
  const Watchdog watchdog("concurrent commands", 120);
  ConfigParts parts;
  parts.paper_ = "bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-26T09:00\"\n";
  const PaperFixture fixture(parts);
  initialize(fixture);
  std::vector<std::unique_ptr<Spawned>> processes;
  for (int i = 0; i < 20; ++i) {
    processes.push_back(
      std::make_unique<Spawned>(std::vector<std::string>{"--json", "clock", "advance", "--minutes", "1"})
    );
    processes.push_back(std::make_unique<Spawned>(std::vector<std::string>{"--json", "status"}));
  }
  for (auto& process : processes) {
    const int status = process->wait(60);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }
  CHECK(reply(fixture, {"clock"}).at("now").as_string() == "2024-03-26T09:20:00+09:00");
  const auto log = slurp(fixture.log());
  CHECK(log.back() == '\n');
  CHECK(lines_of(log).size() == 21);
  CHECK(no_children());
}

TEST_CASE("an allocation failing anywhere in an account command leaks nothing and leaves the lock free") {
  const Watchdog watchdog("the account sweep", 1200);
  // Sixty days of bars, so an advance never runs out of data during the sweep.
  std::string bars = "symbol,date,open,high,low,close,volume\n";
  for (int day = 0; day < 60; ++day) {
    const auto date = paper::date_text(paper::days_from_civil(2024, 3, 25) + day);
    bars += "005930," + date + ",78000,79000,77500,78500,1\n";
    bars += "000660," + date + ",180000,182000,178000,181000,1\n";
  }
  const PaperFixture fixture(ConfigParts{}, bars);
  fixture.arm();
  const auto lock_free = [&fixture] {
    cmdatl::OwnerLock second(fixture.account().string());
    const auto outcome = second.acquire(
      std::chrono::milliseconds(0),
      [] {
        return false;
      },
      [](const std::string&) {}
    );
    return outcome == cmdatl::OwnerLock::Outcome::HELD;
  };
  const std::vector<std::vector<std::string>> commands{
    {"status"},
    {"account"},
    {"quote", "005930"},
    {"order", "list"},
    {"clock"},
    {"clock", "advance", "--minutes", "1"},
  };
  for (const bool fresh : {true, false}) {
    for (const auto& args : commands) {
      if (fresh && args.front() != "status") {
        continue;
      }
      INFO((fresh ? "fresh " : "") << cmdatl::join(args));
      int before = -1;
      long first_bad = 0;
      long n = 1;
      std::string log;
      for (; n < 400000; ++n) {
        if (fresh) {
          std::filesystem::remove_all(fixture.dir() / "state");
        } else if (before < 0) {
          REQUIRE(fixture.json({"status"}).exit_ == 0);
          log = slurp(fixture.log());
        } else {
          // Each run on the same state, so an advance that went through does not lengthen the next replay.
          spit(fixture.log(), log);
        }
        if (before < 0) {
          before = open_fds();
        }
        std::ostringstream out;
        std::ostringstream err;
        auto line = args;
        line.insert(line.begin(), "--json");
        fail_allocation(n);
        int exit = -1;
        try {
          exit = cmdatl::run(line, out, err, false);
        } catch (const std::bad_alloc&) {
          // Before the command started: main's last resort.
        }
        const bool consumed = fail_at.load() == 0;
        fail_allocation(0);
        const bool exit_ok = exit == -1 || exit == 0 || exit == 1 || exit == 4;
        if (
          (!exit_ok || open_fds() != before || (std::filesystem::exists(fixture.account()) && !lock_free())) &&
          first_bad == 0
        ) {
          first_bad = n;
        }
        if (!consumed) {
          CHECK(exit == 0);
          break;
        }
      }
      INFO("allocations swept: " << n << ", first problem at " << first_bad);
      CHECK(first_bad == 0);
      CHECK(n < 400000);
    }
  }
}

TEST_CASE("a failing output: during the wait, in a read's reply, and after a committed advance") {
  const Watchdog watchdog("failing output", 60);
  const PaperFixture fixture;
  initialize(fixture);
  const int before = open_fds();
  {
    // The waiting line cannot be written: the lock is given back.
    Holder holder(fixture.account(), 3000);
    std::ostringstream out;
    std::ostringstream err;
    err.setstate(std::ios::badbit);
    CHECK(cmdatl::run({"--format", "text", "status"}, out, err, false) == 1);
  }
  {
    std::ostringstream err;
    std::ostringstream out;
    out.setstate(std::ios::badbit);
    CHECK(cmdatl::run({"--json", "status"}, out, err, false) == 1);
    CHECK(err.str().find("its reply could not be written") != std::string::npos);
  }
  {
    // After the advance is committed, a lost reply is unknown, and the advance happened exactly once.
    const auto log = slurp(fixture.log());
    std::ostringstream err;
    std::ostringstream out;
    out.setstate(std::ios::badbit);
    CHECK(cmdatl::run({"--json", "clock", "advance", "--minutes", "5"}, out, err, false) == 4);
    CHECK(err.str().find("read it before any other command") != std::string::npos);
    CHECK(lines_of(slurp(fixture.log())).size() == lines_of(log).size() + 1);
    CHECK(reply(fixture, {"clock"}).at("now").as_string() == "2024-03-25T16:05:00+09:00");
  }
  CHECK(open_fds() == before);
}

TEST_CASE("cmdatl started with stdin, stdout or stderr closed never writes a reply into a state file") {
  const Watchdog watchdog("closed descriptors", 60);
  const PaperFixture fixture;
  initialize(fixture);
  for (const auto closed : {Closed{true, false, false}, Closed{false, true, false}, Closed{false, false, true}}) {
    const auto log = slurp(fixture.log());
    const auto journal_text = slurp(fixture.journal());
    Spawned cmdatl({"--format", "text", "clock", "advance", "--to", "2024-03-25T15:00"}, closed);
    cmdatl.wait(20);
    CHECK(slurp(fixture.log()) == log);
    CHECK(slurp(fixture.journal()) == journal_text);
    CHECK(slurp(fixture.account() / "lock").empty());
    Spawned ok({"--json", "status"}, closed);
    ok.wait(20);
    CHECK(slurp(fixture.log()) == log);
    CHECK(slurp(fixture.journal()) == journal_text);
    CHECK(slurp(fixture.account() / "lock").empty());
  }
  initialize(fixture);
}

TEST_CASE("no account command starts a child, and descriptors stay level over repeated runs") {
  const Watchdog watchdog("repeated account commands", 300);
  const PaperFixture fixture;
  fixture.arm();
  initialize(fixture);
  const std::vector<std::vector<std::string>> commands{
    {"status"},
    {"account"},
    {"quote", "005930"},
    {"order", "list"},
    {"clock"},
    {"clock", "advance", "--minutes", "1"},
    {"quote", "NOPE"},
    {"clock", "advance", "--to", "2024-03-25T15:00"},
  };
  for (const auto& args : commands) {
    INFO(cmdatl::join(args));
    const int before = open_fds();
    for (int i = 0; i < 200; ++i) {
      (void)fixture.json(args);
    }
    CHECK(open_fds() == before);
    CHECK(no_children());
  }
}

TEST_CASE("the account directory read-only, a file size limit, descriptors exhausted, no state root") {
  const Watchdog watchdog("resource limits", 60);
  const PaperFixture fixture;
  initialize(fixture);
  REQUIRE(fixture.json({"clock", "advance", "--minutes", "1"}).exit_ == 0);
  const auto log = slurp(fixture.log());
  {
    // A fresh account in a read-only state root cannot begin: nothing half-made.
    const auto root = fixture.dir() / "readonly";
    std::filesystem::create_directories(root);
    ::chmod(root.c_str(), 0500);
    const ScopedVariable state("XDG_STATE_HOME", root.string());
    const auto ran = fixture.json({"status"});
    if (::access(root.c_str(), W_OK) != 0) {
      CHECK(ran.exit_ == 1);
      CHECK(code_of(ran.out_) == "state.unavailable");
    }
    ::chmod(root.c_str(), 0700);
  }
  {
    // An append past RLIMIT_FSIZE, with SIGXFSZ ignored, fails with EFBIG: the clock has not moved.
    struct rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    struct sigaction ignore{};
    struct sigaction old{};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGXFSZ, &ignore, &old);
    struct rlimit small = saved;
    small.rlim_cur = static_cast<rlim_t>(log.size() + 5);
    REQUIRE(::setrlimit(RLIMIT_FSIZE, &small) == 0);
    const auto ran = fixture.json({"clock", "advance", "--minutes", "1"});
    ::setrlimit(RLIMIT_FSIZE, &saved);
    ::sigaction(SIGXFSZ, &old, nullptr);
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "file.unwritable");
    CHECK(declares({"clock", "advance"}, 1, "file.unwritable"));
    // The partial line is cut by the next open; the clock is where it was.
    CHECK(reply(fixture, {"clock"}).at("now").as_string() == "2024-03-25T16:01:00+09:00");
    CHECK(slurp(fixture.log()) == log);
  }
  {
    // Descriptors exhausted: a refusal, and nothing leaked once they are back.
    struct rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
    const int before = open_fds();
    int highest = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/fd")) {
      highest = std::max(highest, std::stoi(entry.path().filename().string()));
    }
    struct rlimit small = saved;
    small.rlim_cur = static_cast<rlim_t>(highest + 1 + 8);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &small) == 0);
    std::vector<int> taken;
    while (true) {
      const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        break;
      }
      taken.push_back(fd);
    }
    for (int free = 0; free < 1 && !taken.empty(); ++free) {
      ::close(taken.back());
      taken.pop_back();
    }
    std::ostringstream out;
    std::ostringstream err;
    const int exit = cmdatl::run({"--json", "status"}, out, err, false);
    for (const int fd : taken) {
      ::close(fd);
    }
    ::setrlimit(RLIMIT_NOFILE, &saved);
    INFO(out.str());
    CHECK((exit == 1 || exit == 4));
    CHECK(open_fds() == before);
    CHECK(slurp(fixture.log()) == log);
    CHECK(fixture.json({"status"}).exit_ == 0);
  }
}
TEST_CASE("the table: the commands that work, the ones that change the account, and the errors they declare") {
  const cmdatl::Table table(cmdatl::command_list());
  std::vector<std::string> available;
  std::vector<std::string> mutating;
  for (const auto& command : table.commands()) {
    if (command.available_) {
      available.push_back(cmdatl::join(command.path_));
    }
    if (command.mutates_) {
      mutating.push_back(cmdatl::join(command.path_));
    }
    // Only order place, order cancel and run send (a run places on its own mock market).
    const auto name = cmdatl::join(command.path_);
    CHECK(
      (!command.available_ || !command.may_send_ || name == "order place" || name == "order cancel" || name == "run")
    );
  }
  std::sort(available.begin(), available.end());
  std::sort(mutating.begin(), mutating.end());
  const std::vector<std::string> expected{
    "account",
    "auth login",
    "auth logout",
    "auth status",
    "clock",
    "clock advance",
    "env",
    "guide agent",
    "guide strategy",
    "help",
    "order cancel",
    "order list",
    "order place",
    "order plan",
    "order reconcile",
    "order release",
    "quote",
    "run",
    "schema",
    "status",
    "strategy backtest",
    "strategy new",
    "strategy reference",
  };
  CHECK(available == expected);
  CHECK(mutating == std::vector<std::string>{"clock advance", "order release"});
  CHECK(cmdatl::SCHEMA_VERSION == 11);
  CHECK(declares({"clock", "advance"}, 4, "unknown.internal"));
  CHECK(!declares({"clock"}, 4, "unknown.internal"));
  const auto entry = reply_of(run({"schema", "clock.advance"}).out_).at("commands").as_array()[0].as_object();
  CHECK(entry.at("mutates").as_bool());
  CHECK(!entry.at("returns").as_object().contains("fills"));
  CHECK(entry.at("returns").as_object().contains("events"));
  const auto status_schema = reply_of(run({"schema", "status"}).out_);
  CHECK(!status_schema.at("commands").as_array()[0].as_object().at("returns").as_object().contains("run"));
  for (const auto* path : {"status", "account", "quote", "order list", "clock", "clock advance"}) {
    const auto* command = table.resolve(cmdatl::split(path, ' ')).command_;
    REQUIRE(command != nullptr);
    INFO(path);
    CHECK(command->exits_ == cmdatl::ACCOUNT_EXITS);
    CHECK(command->env_ == "account");
    for (const auto& code : cmdatl::ACCOUNT_ERRORS) {
      const auto errors = cmdatl::errors_of(*command);
      CHECK(std::find(errors.begin(), errors.end(), code) != errors.end());
    }
  }
  CHECK(cmdatl::check_table(table).empty());
  CHECK(run({"clock", "advance", "--help"}).out_.find("(changes the account)") != std::string::npos);
}

TEST_CASE("a scripted session against the binary across three days of bars") {
  const Watchdog watchdog("the scripted session", 120);
  const PaperFixture fixture;
  fixture.arm();
  const auto step = [](const std::vector<std::string>& args) {
    std::vector<std::string> line{"--json"};
    line.insert(line.end(), args.begin(), args.end());
    Spawned cmdatl(line);
    const int status = cmdatl.wait(30);
    REQUIRE(WIFEXITED(status));
    INFO(cmdatl::join(args) << ": " << cmdatl.out() << cmdatl.err());
    CHECK(WEXITSTATUS(status) == 0);
    return reply_of(cmdatl.out());
  };
  CHECK(step({"env"}).at("config").as_object().at("state").as_string() == "ok");
  auto status = step({"status"});
  CHECK(status.at("phase").as_string() == "post_close");
  CHECK(status.at("cash").as_string() == "100000000");
  CHECK(step({"quote", "005930", "000660"}).at("quotes").as_array().size() == 2);
  for (const auto* day : {"2024-03-26T09:00:00+09:00", "2024-03-27T09:00:00+09:00"}) {
    const auto advanced = step({"clock", "advance", "--next-open"});
    CHECK(advanced.at("now").as_string() == day);
    status = step({"status"});
    CHECK(status.at("orders_accepted_now").as_bool());
    CHECK(step({"account"}).at("orders_left_today").as_int64() == 3);
  }
  CHECK(step({"clock", "advance", "--hours", "30"}).at("now").as_string() == "2024-03-28T15:00:00+09:00");
  status = step({"status"});
  CHECK(status.at("phase").as_string() == "closed");
  CHECK(words(status.at("not_accepted")) == std::vector<std::string>{"phase", "market_closed"});
}
#endif
