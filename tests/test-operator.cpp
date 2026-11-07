#include "./paper-support.hpp"

#include <cmdatl/account/operator.hpp>
#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace support;

namespace {

/** @brief One operator's file that must be refused, and where. */
struct Refused {
  /** @brief What it tests. */
  std::string what_;
  /** @brief The file. */
  std::string text_;
  /** @brief The code. */
  std::string code_;
  /** @brief The line, or 0 to check only that one is given. */
  std::uint32_t line_;
};

/**
 * @brief The default file with one table's body replaced.
 * @param account The `[account]` body, or empty for the default.
 * @param limits The `[limits]` body, or empty.
 * @param paper The `[paper]` body, or empty.
 * @return The text.
 */
std::string with(const std::string& account, const std::string& limits, const std::string& paper) {
  ConfigParts parts;
  if (!account.empty()) {
    parts.account_ = account;
  }
  if (!limits.empty()) {
    parts.limits_ = limits;
  }
  if (!paper.empty()) {
    parts.paper_ = paper;
  }
  return parts.text();
}

/** @brief The default `[limits]` body with one line appended. */
std::string limits_plus(const std::string& line) {
  return ConfigParts{}.limits_ + line + "\n";
}

/** @brief The default `[paper]` body with one line appended. */
std::string paper_plus(const std::string& line) {
  return ConfigParts{}.paper_ + line + "\n";
}

/**
 * @brief The default `[limits]` body with one key's line replaced.
 * @param key The key, at the start of its line.
 * @param line The new line, or empty to drop it.
 * @return The text.
 */
std::string limits_with(const std::string& key, const std::string& line) {
  std::string out;
  for (const auto& existing : lines_of(ConfigParts{}.limits_)) {
    if (existing.rfind(key + " ", 0) == 0) {
      if (!line.empty()) {
        out += line + "\n";
      }
    } else {
      out += existing + "\n";
    }
  }
  return out;
}

/**
 * @brief The default `[paper]` body with one key's line replaced.
 * @param key The key.
 * @param line The new line, or empty to drop it.
 * @return The text.
 */
std::string paper_with(const std::string& key, const std::string& line) {
  std::string out;
  for (const auto& existing : lines_of(ConfigParts{}.paper_)) {
    if (existing.rfind(key + " ", 0) == 0) {
      if (!line.empty()) {
        out += line + "\n";
      }
    } else {
      out += existing + "\n";
    }
  }
  return out;
}

} // namespace

TEST_CASE("the operator's file: every rule refused alone, with its line and column") {
  const PaperFixture fixture;
  // The default file has [account] on line 1 and its keys on 2-3; [limits] on 5 and its keys on 6-9; [paper] on 11
  // and its keys on 12-14.
  const std::string fifty = [] {
    std::string list;
    for (int i = 0; i < 51; ++i) {
      list += (list.empty() ? "" : ", ") + ("\"S" + std::to_string(i) + "\"");
    }
    return list;
  }();
  const std::vector<Refused> cases{
    {"no [account]", "[limits]\n" + ConfigParts{}.limits_ + "[paper]\n" + ConfigParts{}.paper_, "config.invalid", 0},
    {"a broker that is a number", with("broker = 1\nnumber = \"x\"\n", "", ""), "config.invalid", 2},
    {"kis with a number not 12345678-01", with("broker = \"kis\"\nnumber = \"x\"\n", "", ""), "config.invalid", 3},
    {"kis with an unknown env", with("broker = \"kis\"\nenv = \"paper\"\nnumber = \"12345678-01\"\n", "", ""),
      "config.invalid", 3},
    {"kiwoom", with("broker = \"kiwoom\"\nnumber = \"x\"\n", "", ""), "unsupported.broker", 2},
    {"toss", with("broker = \"toss\"\nnumber = \"x\"\n", "", ""), "unsupported.broker", 2},
    {"a broker no folder can be named", with("broker = \"paper_x\"\nnumber = \"x\"\n", "", ""), "config.invalid", 2},
    {"no broker", with("number = \"x\"\n", "", ""), "config.invalid", 0},
    {"env demo on paper", with("broker = \"paper\"\nenv = \"demo\"\nnumber = \"x\"\n", "", ""), "config.invalid", 3},
    {"env real on paper", with("broker = \"paper\"\nenv = \"real\"\nnumber = \"x\"\n", "", ""), "config.invalid", 3},
    {"a number leaving its directory", with("broker = \"paper\"\nnumber = \"../x\"\n", "", ""), "config.invalid", 3},
    {"a number too long", with("broker = \"paper\"\nnumber = \"" + std::string(65, 'a') + "\"\n", "", ""),
      "config.invalid", 3},
    {"an empty number", with("broker = \"paper\"\nnumber = \"\"\n", "", ""), "config.invalid", 3},
    {"no number", with("broker = \"paper\"\n", "", ""), "config.invalid", 0},
    {"an unknown key in [account]", with(ConfigParts{}.account_ + "nummber = \"x\"\n", "", ""), "config.invalid", 4},
    {"no symbols", with("", limits_with("symbols", ""), ""), "config.invalid", 0},
    {"symbols not an array", with("", limits_with("symbols", "symbols = \"005930\""), ""), "config.invalid", 6},
    {"no symbol", with("", limits_with("symbols", "symbols = []"), ""), "config.invalid", 6},
    {"51 symbols", with("", limits_with("symbols", "symbols = [" + fifty + "]"), ""), "config.invalid", 6},
    {"a symbol that is a number", with("", limits_with("symbols", "symbols = [5930, \"000660\"]"), ""),
      "config.invalid", 6},
    {"a symbol with a space", with("", limits_with("symbols", "symbols = [\"00 5930\", \"000660\"]"), ""),
      "config.invalid", 6},
    {"a repeated symbol", with("", limits_with("symbols", "symbols = [\"005930\", \"005930\"]"), ""), "config.invalid",
      6},
    {"no position", with("", limits_with("position", ""), ""), "config.invalid", 0},
    {"position not a table", with("", limits_with("position", "position = 100"), ""), "config.invalid", 7},
    {"position missing a symbol", with("", limits_with("position", "position = { \"005930\" = 100 }"), ""),
      "config.invalid", 7},
    {"position adding a symbol",
      with("", limits_with("position", "position = { \"005930\" = 100, \"000660\" = 20, \"035720\" = 1 }"), ""),
      "config.invalid", 7},
    {"a float position", with("", limits_with("position", "position = { \"005930\" = 100.0, \"000660\" = 20 }"), ""),
      "config.invalid", 7},
    {"a negative position", with("", limits_with("position", "position = { \"005930\" = -1, \"000660\" = 20 }"), ""),
      "config.invalid", 7},
    {"a position past 10^8",
      with("", limits_with("position", "position = { \"005930\" = 100000001, \"000660\" = 20 }"), ""), "config.invalid",
      7},
    {"no order_value", with("", limits_with("order_value", ""), ""), "config.invalid", 0},
    {"a float order_value", with("", limits_with("order_value", "order_value = 5000000.0"), ""), "config.invalid", 8},
    {"order_value 0", with("", limits_with("order_value", "order_value = 0"), ""), "config.invalid", 8},
    {"order_value past 10^13", with("", limits_with("order_value", "order_value = 10000000000001"), ""),
      "config.invalid", 8},
    {"order_value text", with("", limits_with("order_value", "order_value = \"lots\""), ""), "config.invalid", 8},
    {"order_value five decimals", with("", limits_with("order_value", "order_value = \"1.00001\""), ""),
      "config.invalid", 8},
    {"order_value zero text", with("", limits_with("order_value", "order_value = \"0.0\""), ""), "config.invalid", 8},
    {"order_value text past 10^13", with("", limits_with("order_value", "order_value = \"10000000000000.01\""), ""),
      "config.invalid", 8},
    {"no daily_orders", with("", limits_with("daily_orders", ""), ""), "config.invalid", 0},
    {"daily_orders as text", with("", limits_with("daily_orders", "daily_orders = \"3\""), ""), "config.invalid", 9},
    {"negative daily_orders", with("", limits_with("daily_orders", "daily_orders = -1"), ""), "config.invalid", 9},
    {"daily_orders past 10000", with("", limits_with("daily_orders", "daily_orders = 10001"), ""), "config.invalid", 9},
    {"order_type market", with("", limits_plus("order_type = \"market\""), ""), "config.invalid", 10},
    {"an integer collar", with("", limits_plus("collar = 1"), ""), "config.invalid", 10},
    {"collar nan", with("", limits_plus("collar = nan"), ""), "config.invalid", 10},
    {"collar inf", with("", limits_plus("collar = inf"), ""), "config.invalid", 10},
    {"collar past 0.30", with("", limits_plus("collar = 0.31"), ""), "config.invalid", 10},
    {"collar 0", with("", limits_plus("collar = 0.0"), ""), "config.invalid", 10},
    {"a negative collar", with("", limits_plus("collar = -0.05"), ""), "config.invalid", 10},
    {"phases not an array", with("", limits_plus("phases = \"open\""), ""), "config.invalid", 10},
    {"no phase", with("", limits_plus("phases = []"), ""), "config.invalid", 10},
    {"phase closed", with("", limits_plus("phases = [\"closed\"]"), ""), "config.invalid", 10},
    {"phase unknown", with("", limits_plus("phases = [\"unknown\"]"), ""), "config.invalid", 10},
    {"repeated phases", with("", limits_plus("phases = [\"open\", \"open\"]"), ""), "config.invalid", 10},
    {"plan_ttl 0", with("", limits_plus("plan_ttl = 0"), ""), "config.invalid", 10},
    {"plan_ttl past 3600", with("", limits_plus("plan_ttl = 3601"), ""), "config.invalid", 10},
    {"portfolio_age past a day", with("", limits_plus("portfolio_age = 86401"), ""), "config.invalid", 10},
    {"a misspelt limit", with("", limits_plus("colar = 0.05"), ""), "config.invalid", 10},
    {"no [paper]", "[account]\n" + ConfigParts{}.account_ + "[limits]\n" + ConfigParts{}.limits_, "config.invalid", 0},
    {"no bars", with("", "", paper_with("bars", "")), "config.invalid", 0},
    {"bars a number", with("", "", paper_with("bars", "bars = 1")), "config.invalid", 12},
    {"no cash", with("", "", paper_with("cash", "")), "config.invalid", 0},
    {"cash 0", with("", "", paper_with("cash", "cash = 0")), "config.invalid", 13},
    {"cash as a float", with("", "", paper_with("cash", "cash = 1e8")), "config.invalid", 13},
    {"cash past 10^13", with("", "", paper_with("cash", "cash = 10000000000001")), "config.invalid", 13},
    {"no start", with("", "", paper_with("start", "")), "config.invalid", 0},
    {"a TOML date-time start", with("", "", paper_with("start", "start = 2024-03-25T09:30:00")), "config.invalid", 14},
    {"an impossible date", with("", "", paper_with("start", "start = \"2024-02-30T09:30\"")), "config.invalid", 14},
    {"a date before 1990", with("", "", paper_with("start", "start = \"1989-12-31T09:30\"")), "config.invalid", 14},
    {"a date after 2100", with("", "", paper_with("start", "start = \"2101-01-01T09:30\"")), "config.invalid", 14},
    {"a start with seconds", with("", "", paper_with("start", "start = \"2024-03-25T09:30:00\"")), "config.invalid",
      14},
    {"a negative listing lag", with("", "", paper_plus("listing_lag = -1")), "config.invalid", 15},
    {"a holdings lag past a day", with("", "", paper_plus("holdings_lag = 86401")), "config.invalid", 15},
    {"a state_dir key", with("", "", paper_plus("state_dir = \"/tmp\"")), "config.invalid", 15},
    {"an armed_file key", "armed_file = \"/tmp/a\"\n" + ConfigParts{}.text(), "config.invalid", 1},
    {"an unknown table", ConfigParts{}.text() + "\n[extra]\nx = 1\n", "config.invalid", 16},
    {"atlc a number", "atlc = 1\n" + ConfigParts{}.text(), "config.invalid", 1},
    // What toml++ refuses: the cases CLI11's reader let through.
    {"a repeated key", with("", limits_plus("daily_orders = 4"), ""), "config.invalid", 10},
    {"a table opened twice", ConfigParts{}.text() + "[limits]\ncollar = 0.1\n", "config.invalid", 15},
    {"an unclosed table", ConfigParts{}.text() + "[limits\n", "config.invalid", 15},
    {"an unquoted string", with("broker = paper\nnumber = \"x\"\n", "", ""), "config.invalid", 2},
    {"a line without =", with(ConfigParts{}.account_ + "flag\n", "", ""), "config.invalid", 4},
    {"invalid UTF-8", with("broker = \"paper\"\nnumber = \"\xff\"\n", "", ""), "config.invalid", 3},
    // A table header toml++ 3.4.0 meets with an internal assertion: refused, never an abort.
    {"a header starting with ~", ConfigParts{}.text() + "[~imits]\n", "config.invalid", 0},
    {"a header starting with ~ in place of a table", "[account]\n" + ConfigParts{}.account_ + "[~imits]\n",
      "config.invalid", 0},
  };
  for (const auto& c : cases) {
    INFO(c.what_ << "\n" << c.text_);
    fixture.write_config(c.text_);
    const auto snapshot = cmdatl::load_operator_config();
    REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::INVALID);
    CHECK(snapshot.error_.code_ == c.code_);
    if (c.line_ != 0) {
      CHECK(snapshot.error_.line_ == c.line_);
      CHECK(snapshot.error_.column_ > 0);
    }
    // Through a command: the code, the file, the line and the column, with env offline (no account was read).
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 1);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("env").as_string() == "offline");
    const auto& error = reply.at("error").as_object();
    CHECK(error.at("code").as_string() == c.code_);
    CHECK(error.at("file").as_string() == fixture.config().string());
    if (c.line_ != 0) {
      CHECK(error.at("line").as_int64() == c.line_);
      CHECK(error.at("column").as_int64() > 0);
    }
    CHECK(declares({"status"}, 1, c.code_));
  }
}

TEST_CASE("a valid operator's file: defaults filled, paths beside it, the armed file fixed") {
  const PaperFixture fixture;
  fixture.write_config(
    "atlc = \"bin/atlc\"\n" +
    with("broker = \"paper\"\nenv = \"paper\"\nnumber = \"acct_1-A\"\n", limits_plus("order_type = \"limit\""), "")
  );
  auto snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
  const auto& c = *snapshot.config_;
  CHECK(c.atlc_ == (fixture.dir() / "bin" / "atlc").string());
  CHECK(c.paper_.bars_ == (fixture.dir() / "bars.csv").string());
  CHECK(c.armed_path() == (fixture.dir() / "paper.armed").string());
  CHECK(c.scope() == "paper:paper:acct_1-A");
  CHECK(c.limits_.collar_ == 0.05);
  CHECK(c.limits_.phases_ == std::vector<std::string>{"open"});
  CHECK(c.limits_.plan_ttl_ == 60);
  CHECK(c.limits_.portfolio_age_ == 30);
  CHECK(c.limits_.order_value_ == "5000000");
  CHECK(c.paper_.listing_lag_ == 0);
  // A standard table for position, an order value written as a decimal, phases and bounds at their limits.
  fixture.write_config(with(
    "",
    "symbols = [\"005930\", \"000660\"]\norder_value = \"10000000000000.0\"\ndaily_orders = 0\ncollar = 0.30\n"
    "phases = [\"pre_open\", \"open\", \"post_close\"]\nplan_ttl = 3600\nportfolio_age = 86400\n"
    "[limits.position]\n\"005930\" = 100000000\n\"000660\" = 0\n",
    "bars = \"/abs/bars.csv\"\ncash = 10000000000000\nstart = \"2100-12-31T23:59\"\nlisting_lag = 86400\nholdings_lag = 0\n"
  ));
  snapshot = cmdatl::load_operator_config();
  REQUIRE(snapshot.state_ == cmdatl::ConfigSnapshot::State::OK);
  CHECK(snapshot.config_->limits_.position_.at("005930") == 100000000);
  CHECK(snapshot.config_->paper_.bars_ == "/abs/bars.csv");
  CHECK(snapshot.config_->limits_.phases_.size() == 3);
}

TEST_CASE("CMDATL_CONFIG unset, empty, missing, a directory, a FIFO, a dangling link, too large") {
  const PaperFixture fixture;
  {
    const ScopedVariable unset("CMDATL_CONFIG", std::nullopt);
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::UNSET);
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "config.missing");
  }
  {
    const ScopedVariable empty("CMDATL_CONFIG", std::string());
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::UNSET);
    CHECK(code_of(fixture.json({"account"}).out_) == "config.missing");
  }
  {
    const ScopedVariable missing("CMDATL_CONFIG", (fixture.dir() / "nope.toml").string());
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::MISSING);
    CHECK(code_of(fixture.json({"clock"}).out_) == "config.missing");
  }
  {
    std::filesystem::create_symlink(fixture.dir() / "gone.toml", fixture.dir() / "dangling.toml");
    const ScopedVariable dangling("CMDATL_CONFIG", (fixture.dir() / "dangling.toml").string());
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::MISSING);
  }
  {
    const ScopedVariable directory("CMDATL_CONFIG", fixture.dir().string());
    const auto snapshot = cmdatl::load_operator_config();
    CHECK(snapshot.state_ == cmdatl::ConfigSnapshot::State::INVALID);
    CHECK(code_of(fixture.json({"status"}).out_) == "config.invalid");
  }
  {
    const Watchdog watchdog("a FIFO as the operator's file", 20);
    REQUIRE(::mkfifo((fixture.dir() / "fifo.toml").c_str(), 0600) == 0);
    const ScopedVariable fifo("CMDATL_CONFIG", (fixture.dir() / "fifo.toml").string());
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::INVALID);
  }
  {
    const auto text = ConfigParts{}.text();
    spit(fixture.dir() / "big.toml", text + std::string(cmdatl::MAX_CONFIG_BYTES + 1 - text.size(), '#'));
    const ScopedVariable big("CMDATL_CONFIG", (fixture.dir() / "big.toml").string());
    const auto snapshot = cmdatl::load_operator_config();
    CHECK(snapshot.state_ == cmdatl::ConfigSnapshot::State::INVALID);
    spit(fixture.dir() / "big.toml", text + "\n" + std::string(cmdatl::MAX_CONFIG_BYTES - text.size() - 2, '#') + "\n");
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::OK);
  }
  {
    spit(fixture.dir() / "locked.toml", ConfigParts{}.text());
    ::chmod((fixture.dir() / "locked.toml").c_str(), 0);
    const ScopedVariable locked("CMDATL_CONFIG", (fixture.dir() / "locked.toml").string());
    if (::access((fixture.dir() / "locked.toml").c_str(), R_OK) != 0) {
      CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::INVALID);
    }
    ::chmod((fixture.dir() / "locked.toml").c_str(), 0600);
  }
}

TEST_CASE("an operator's file with a character outside ASCII outside a string is refused before toml++ reads it") {
  const PaperFixture fixture;
  // U+3001 after a value: toml++ 3.4 reaches a path it marks unreachable there.
  fixture.write_config(ConfigParts{}.text() + "x = 1\xe3\x80\x81\n");
  const auto ran = fixture.json({"status"});
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "config.invalid");
  CHECK(
    reply_of(ran.out_).at("error").as_object().at("message").as_string().find("outside ASCII") != std::string::npos
  );
  // In a comment it is read, as before.
  fixture.write_config(ConfigParts{}.text() + "# \xe3\x80\x81\n");
  CHECK(fixture.json({"status"}).exit_ == 0);
}

TEST_CASE("an operator's file nesting tables past the bound is refused before toml++ recurses into it") {
  const PaperFixture fixture;
  REQUIRE(fixture.json({"status"}).exit_ == 0);
  const auto log = slurp(fixture.log());
  const auto journal = slurp(fixture.journal());
  const auto dots = [](const std::string& segment, int count) {
    std::string path = segment;
    for (int i = 1; i < count; ++i) {
      path += "." + segment;
    }
    return path;
  };
  struct Deep {
    std::string what_;
    std::string text_;
    std::int64_t line_;
  };
  const std::vector<Deep> cases{
    // 16,000 segments in 32 KiB: deep enough to exhaust toml++'s stack.
    {"a dotted header", "[" + dots("x", 16000) + "]\na = 1\n", 1},
    {"a dotted header of array tables", ConfigParts{}.text() + "[[" + dots("x", 16000) + "]]\n", 15},
    {"a dotted key", ConfigParts{}.text() + dots("x", 16000) + " = 1\n", 15},
    {"a dotted key of quoted segments", ConfigParts{}.text() + dots("\"x\"", 9000) + " = 1\n", 15},
    {"a dotted key in an inline table", ConfigParts{}.text() + "t = { " + dots("x", 16000) + " = 1 }\n", 15},
    {"nested arrays", ConfigParts{}.text() + "t = " + std::string(16000, '[') + std::string(16000, ']') + "\n", 15},
    {"nested inline tables",
      ConfigParts{}.text() + "t = { a = { a = { a = { a = { a = { a = { a = { a = { a = 1 } } } } } } } } }\n", 15},
    {"nine segments", ConfigParts{}.text() + dots("x", 9) + " = 1\n", 15},
  };
  for (const auto& c : cases) {
    INFO(c.what_);
    fixture.write_config(c.text_);
    Spawned cmdatl({"--json", "status"});
    const int status = cmdatl.wait(30);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    const auto reply = reply_of(cmdatl.out());
    const auto& error = reply.at("error").as_object();
    CHECK(error.at("code").as_string() == "config.invalid");
    CHECK(error.at("line").as_int64() == c.line_);
    CHECK(error.at("column").as_int64() > 0);
    CHECK(slurp(fixture.log()) == log);
    CHECK(slurp(fixture.journal()) == journal);
  }
  // What the bound leaves alone: dots in strings and comments, floats, times, and eight segments.
  auto parts = ConfigParts{};
  parts.top_ = "# a.b.c.d.e.f.g.h.i.j.k\n";
  parts.limits_ += "collar = 0.05 # 1.2.3.4.5.6.7.8.9\n";
  parts.paper_ = "bars = \"./x/../bars.csv\"\ncash = 100000000\nstart = '2024-03-25T16:00'\n";
  fixture.write_config(parts.text());
  CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::OK);
  using cmdatl::config_detail::nesting_refusal;
  CHECK(!nesting_refusal("[a.b.c.d]\n" + dots("x", 8) + " = { y = [[1.5, 2.5], [\"a.b.c.d.e.f.g.h.i\"]] }\n"));
  CHECK(!nesting_refusal("s = \"\"\"a.b.c.d.e.f.g.h.i\"\"\"\"\"\nt = '''a.b.c.d.e.f.g.h.i'''''\n"));
  CHECK(!nesting_refusal("x = [\n  [1, 2],\n]\n  [ a . b ]\n"));
  CHECK(nesting_refusal("s = \"\"\"a\"\"\"\"\"\nt = { " + dots("x", 9) + " = 1 }\n"));
}

TEST_CASE("the state root: XDG_STATE_HOME when absolute, else HOME, else state.unavailable; the directory 0700") {
  const PaperFixture fixture;
  CHECK(cmdatl::state_root() == (fixture.dir() / "state" / "cmdatl").string());
  {
    const ScopedVariable relative("XDG_STATE_HOME", std::string("relative/state"));
    const ScopedVariable home("HOME", (fixture.dir() / "home").string());
    CHECK(cmdatl::state_root() == (fixture.dir() / "home" / ".local" / "state" / "cmdatl").string());
  }
  {
    const ScopedVariable unset("XDG_STATE_HOME", std::nullopt);
    const ScopedVariable home("HOME", (fixture.dir() / "home").string());
    CHECK(cmdatl::state_root() == (fixture.dir() / "home" / ".local" / "state" / "cmdatl").string());
    REQUIRE(fixture.json({"status"}).exit_ == 0);
    CHECK(
      std::filesystem::exists(
        fixture.dir() / "home" / ".local" / "state" / "cmdatl" / "accounts" / "paper-paper-paper-0001" / "paper.jsonl"
      )
    );
  }
  {
    const ScopedVariable unset("XDG_STATE_HOME", std::nullopt);
    const ScopedVariable home("HOME", std::nullopt);
    CHECK(!cmdatl::state_root());
    const auto ran = fixture.json({"status"});
    CHECK(ran.exit_ == 1);
    CHECK(code_of(ran.out_) == "state.unavailable");
    CHECK(declares({"status"}, 1, "state.unavailable"));
  }
  REQUIRE(fixture.json({"status"}).exit_ == 0);
  struct stat info{};
  REQUIRE(::stat(fixture.account().c_str(), &info) == 0);
  CHECK((info.st_mode & 0777) == 0700);
  REQUIRE(::stat((fixture.dir() / "state" / "cmdatl").c_str(), &info) == 0);
  CHECK((info.st_mode & 0777) == 0700);
}

TEST_CASE("the config route: backtest and env read one snapshot, and a named file is never bypassed") {
  const PaperFixture fixture;
  // A directory with an atlc on it, to tell PATH's atlc apart.
  const auto bin = fixture.dir() / "bin";
  std::filesystem::create_directories(bin);
  std::filesystem::create_symlink(CMDATL_FAKE_ATLC, bin / "atlc");
  const auto* path = std::getenv("PATH");
  const ScopedVariable on_path("PATH", bin.string() + ":" + (path == nullptr ? "/usr/bin:/bin" : path));
  spit(fixture.dir() / "script", "version out atlc 0.0.0-9000\n");
  const ScopedVariable script("FAKE_ATLC_SCRIPT", (fixture.dir() / "script").string());
  // A run spec whose settings read, so a backtest gets as far as finding atlc.
  spit(fixture.dir() / "s.toml", FAKE_SPEC);
  const InDir here(fixture.dir());
  const auto env = [&fixture] {
    return reply_of(fixture.json({"env"}).out_);
  };
  {
    // Unset: PATH.
    const ScopedVariable unset("CMDATL_CONFIG", std::nullopt);
    const auto reply = env();
    CHECK(reply.at("atlc").as_object().at("source").as_string() == "PATH");
    CHECK(reply.at("config").as_object().at("state").as_string() == "unset");
    CHECK(reply.at("account").is_null());
  }
  {
    // Named but missing: no lookup, the config's error in both.
    const ScopedVariable missing("CMDATL_CONFIG", (fixture.dir() / "nope.toml").string());
    const auto backtest = fixture.json({"strategy", "backtest", "s.toml"});
    CHECK(backtest.exit_ == 1);
    CHECK(code_of(backtest.out_) == "config.missing");
    CHECK(declares({"strategy", "backtest"}, 1, "config.missing"));
    const auto reply = fixture.json({"env"});
    CHECK(reply.exit_ == 0);
    const auto body = reply_of(reply.out_);
    CHECK(body.at("atlc").as_object().at("path").is_null());
    CHECK(body.at("atlc").as_object().at("error").as_object().at("code").as_string() == "config.missing");
    CHECK(body.at("config").as_object().at("state").as_string() == "missing");
  }
  {
    // Invalid: the same, with the line.
    fixture.write_config("[account\n");
    const auto backtest = fixture.json({"strategy", "backtest", "s.toml"});
    CHECK(backtest.exit_ == 1);
    CHECK(code_of(backtest.out_) == "config.invalid");
    CHECK(reply_of(backtest.out_).at("error").as_object().at("line").as_int64() == 1);
    CHECK(declares({"strategy", "backtest"}, 1, "config.invalid"));
    const auto body = env();
    CHECK(body.at("config").as_object().at("state").as_string() == "invalid");
    CHECK(body.at("config").as_object().at("error").as_object().at("line").as_int64() == 1);
    CHECK(body.at("atlc").as_object().at("error").as_object().at("code").as_string() == "config.invalid");
    // An unsupported broker reads as invalid for backtest, and as itself in env's config.
    fixture.write_config(with("broker = \"kiwoom\"\nnumber = \"x\"\n", "", ""));
    CHECK(code_of(fixture.json({"strategy", "backtest", "s.toml"}).out_) == "config.invalid");
    CHECK(env().at("config").as_object().at("error").as_object().at("code").as_string() == "unsupported.broker");
  }
  {
    // Valid with an atlc key that does not run: atlc.missing from the config, never PATH's.
    fixture.write_config("atlc = \"nowhere/atlc\"\n" + ConfigParts{}.text());
    const auto backtest = fixture.json({"strategy", "backtest", "s.toml"});
    CHECK(backtest.exit_ == 1);
    CHECK(code_of(backtest.out_) == "atlc.missing");
    const auto body = env();
    CHECK(body.at("atlc").as_object().at("source").as_string() == "config");
    CHECK(body.at("atlc").as_object().at("path").as_string() == (fixture.dir() / "nowhere" / "atlc").string());
    CHECK(!body.at("atlc").as_object().at("ok").as_bool());
  }
  {
    // Valid with an atlc key that runs.
    fixture.write_config("atlc = \"bin/atlc\"\n" + ConfigParts{}.text());
    const auto body = env();
    CHECK(body.at("atlc").as_object().at("source").as_string() == "config");
    CHECK(body.at("atlc").as_object().at("version").as_string() == "0.0.0-9000");
    CHECK(body.at("account").as_object().at("number").as_string() == "paper-0001");
    CHECK(body.at("modes") == boost::json::array{"paper", "kis"});
    CHECK(body.at("schema_version").as_int64() == cmdatl::SCHEMA_VERSION);
    CHECK(body.at("paper").as_object().at("cash").as_string() == "100000000");
    CHECK(body.at("capabilities").as_object().at("clock").as_bool());
    CHECK(!body.at("capabilities").as_object().at("trigger_price").as_bool());
    CHECK(body.at("limits").as_object().at("collar").as_string() == "0.05");
    const auto& costs = body.at("paper").as_object().at("costs").as_object();
    CHECK(costs.at("commission").as_string() == "0.00015");
    CHECK(costs.at("sell_tax").as_string() == "0.002");
    CHECK(cmdatl::decimal_text(0.3) == "0.3");
    CHECK(cmdatl::decimal_text(0.00001) == "0.00001");
    CHECK(!body.at("armed").as_object().at("exists").as_bool());
    CHECK(body.at("owner").is_null());
  }
  {
    // Valid without the key: PATH.
    fixture.write_config(ConfigParts{}.text());
    CHECK(env().at("atlc").as_object().at("source").as_string() == "PATH");
  }
  {
    // CMDATL_ATLC over an invalid config: backtest reaches atlc; env reports the config's error beside it.
    fixture.write_config("[account\n");
    const FakeAtlc fake("check out " + INFO_OK + "\ndriver out " + REPORT + "\n");
    const auto backtest = fixture.json({"strategy", "backtest", "s.toml"});
    CHECK(backtest.exit_ == 0);
    const auto body = env();
    CHECK(body.at("atlc").as_object().at("source").as_string() == "CMDATL_ATLC");
    CHECK(!body.at("atlc").as_object().contains("error"));
    CHECK(body.at("config").as_object().at("state").as_string() == "invalid");
  }
  {
    // One snapshot per invocation: an atlc that spoils the file while it answers --version leaves env reporting the
    // file as it was read.
    const auto spoiled = fixture.dir() / "script.cfg";
    spit(spoiled, "atlc = \"bin/atlc\"\n" + ConfigParts{}.text());
    spit(fixture.dir() / "script", "version out atlc 0.0.0-9000\nversion spoil cfg\n");
    const ScopedVariable again("FAKE_ATLC_SCRIPT", (fixture.dir() / "script").string());
    const ScopedVariable named("CMDATL_CONFIG", spoiled.string());
    const auto body = env();
    CHECK(body.at("config").as_object().at("state").as_string() == "ok");
    CHECK(body.at("atlc").as_object().at("source").as_string() == "config");
    CHECK(slurp(spoiled).find("!!! not toml") != std::string::npos);
    CHECK(cmdatl::load_operator_config().state_ == cmdatl::ConfigSnapshot::State::INVALID);
  }
}
#endif
