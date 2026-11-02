// Run settings (`strategy/spec.hpp`): the file read by toml++, command-line settings typed by key, each key's rule,
// and the rules against the program's facts, ported from atlc's run-spec tests.

#include "./support.hpp"

#include <cmdatl/strategy/spec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#if !defined(_WIN32)
using namespace support;
namespace sp = cmdatl::spec;

namespace {

/** @brief Where the in-memory specs say they are. */
const std::string FILE_PATH = "/specs/s.toml";

/** @brief A spec read from text: its checked entries, or its errors as a person reads them. */
struct Parsed {
  /** @brief The entries. */
  std::vector<sp::Entry> entries_;
  /** @brief The errors. */
  std::vector<std::string> errors_;
};

/**
 * @brief The errors as a person reads them.
 * @param errors The errors.
 * @return Their texts.
 */
std::vector<std::string> texts(const std::vector<sp::Error>& errors) {
  std::vector<std::string> out;
  for (const auto& e : errors) {
    out.push_back(e.text());
  }
  return out;
}

/**
 * @brief Reads a spec's text as `/specs/s.toml`.
 * @param src The text.
 * @return What was read.
 */
Parsed spec(const std::string& src) {
  const auto read = sp::read_text(src, FILE_PATH);
  return {read.entries_, texts(read.errors_)};
}

/**
 * @brief The one error a spec has.
 * @param src The text.
 * @return It.
 */
std::string one_error(const std::string& src) {
  const auto parsed = spec(src);
  INFO(src);
  REQUIRE(parsed.errors_.size() == 1);
  return parsed.errors_.front();
}

/**
 * @brief A spec's entry's value as the replay reads it.
 * @param parsed The spec.
 * @param key The key.
 * @return The value.
 */
std::string value_of(const Parsed& parsed, const std::string& key) {
  for (const auto& e : parsed.entries_) {
    if (e.key_ == key) {
      return e.value_.text();
    }
  }
  return "(none)";
}

/**
 * @brief Command-line settings, read and checked.
 * @param words The words.
 * @param errors Set to the errors.
 * @return The entries.
 */
std::vector<sp::Entry> cli(const std::vector<std::string>& words, std::vector<std::string>& errors) {
  std::vector<sp::Entry> out;
  for (const auto& word : words) {
    REQUIRE(sp::is_setting(word));
    sp::Entry e;
    if (auto error = sp::from_command_line(word, e)) {
      errors.push_back(error->text());
      continue;
    }
    if (auto error = sp::check(e)) {
      errors.push_back(error->text());
      continue;
    }
    out.push_back(std::move(e));
  }
  return out;
}

/**
 * @brief One command-line setting's error, or empty.
 * @param word The word.
 * @return The error.
 */
std::string cli_error(const std::string& word) {
  std::vector<std::string> errors;
  (void)cli({word}, errors);
  return errors.empty() ? std::string() : errors.front();
}

/**
 * @brief One command-line setting's value as the replay reads it.
 * @param word The word.
 * @return The value.
 */
std::string cli_value(const std::string& word) {
  std::vector<std::string> errors;
  const auto entries = cli({word}, errors);
  INFO(word << ": " << (errors.empty() ? "" : errors.front()));
  REQUIRE(entries.size() == 1);
  return entries.front().value_.text();
}

/**
 * @brief A single-instrument program with an int, a real and a bool parameter.
 * @return It.
 */
sp::Program single() {
  sp::Program p;
  p.mode_ = "single";
  p.params_ = {{"lookback", sp::ParamType::INT}, {"entry", sp::ParamType::REAL}, {"flag", sp::ParamType::BOOL}};
  return p;
}

/** @brief A resolved run, or its errors. */
struct Run {
  /** @brief The result. */
  sp::Resolved resolved_;
  /** @brief The errors. */
  std::vector<std::string> errors_;

  /**
   * @brief An argument's value.
   * @param key The key.
   * @return The value, or nothing.
   */
  std::optional<std::string> arg(const std::string& key) const {
    for (const auto& [k, v] : resolved_.args_) {
      if (k == key) {
        return v;
      }
    }
    return std::nullopt;
  }
};

/**
 * @brief Resolves layers against a program.
 * @param layers The layers.
 * @param program The program.
 * @return The run.
 */
Run resolve(const std::vector<std::vector<sp::Entry>>& layers, const sp::Program& program) {
  Run run;
  run.errors_ = texts(sp::resolve(layers, program, run.resolved_));
  return run;
}

/**
 * @brief Resolves a spec's text against a program.
 * @param src The text.
 * @param program The program.
 * @return The run.
 */
Run run(const std::string& src, const sp::Program& program) {
  const auto parsed = spec(src);
  INFO(src);
  REQUIRE(parsed.errors_.empty());
  return resolve({parsed.entries_}, program);
}

/** @brief A spec of wide tables with only the closes. */
const std::string WIDE_SPEC = "strategy = \"m.atls\"\n[data]\nfile = \"closes.csv\"\n";

/**
 * @brief Whether a text holds another.
 * @param text The text.
 * @param part The other.
 * @return Whether it does.
 */
bool has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("the run-spec reference lists every key") {
  const auto doc = slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "docs" / "run-spec.md");
  REQUIRE(!doc.empty());
  for (const auto& key : sp::keys()) {
    INFO(key.name_);
    CHECK(has(doc, "`" + key.name_ + "`"));
  }
  CHECK(has(doc, "`instruments.NAME`"));
  CHECK(has(doc, "`params.NAME`"));
  CHECK(has(doc, "`build.algotradelib`"));
}

TEST_CASE("values are checked key by key, each error at its file and line") {
  const std::vector<std::pair<std::string, std::string>> cases{
    {"stratgy = \"m.atls\"", "unknown setting `stratgy`"},
    {"[data]\nsorce = \"csv\"", "unknown setting `data.sorce`"},
    {"[build]\ncache = \"x\"", "unknown setting `build.cache` ([build] takes duckdb and indicators)"},
    {"[orders]\nprice = \"last\"", "unknown setting `orders.price` ([orders] takes type and offset)"},
    {"[build]\nindicators = \"pairwise\"", "`build.indicators` is one of \"incremental\", \"naive\", \"exact\""},
    {"[build]\nalgotradelib = \"/opt/atl\"", "cmdatl builds against the algotradelib it was built with"},
    {"[output]\nfills = \"f.csv\"", "tables: [data], [instruments], [replay], [params], [orders], [build]"},
    {"[run]\nid = \"r0123456789ab\"", "unknown setting `run.id`"},
    {"[params.x]\ny = 1", "unknown setting `params.x.y`"},
    {"[params]\nlookback = \"20\"", "is a number or a boolean"},
    {"[instruments]\ny = 5930", "`instruments.y` is a string, not 5930"},
    {"[replay]\ncash = 0", "`replay.cash` is a number above 0, not 0"},
    {"[replay]\ncash = \"1000\"", "a number above 0"},
    {"[replay]\ncommission = -0.1", "a number, 0 or more"},
    {"[replay]\nslippage_ticks = 1.5", "a whole number"},
    {"[replay]\nslippage_ticks = 2.0", "written as an integer"},
    {"[replay]\nshorts = \"yes\"", "`true` or `false`"},
    {"[replay]\nfill = \"close\"", "one of \"next-open\""},
    {"[replay]\nfrom = 20230230", "a date"},
    {"[replay]\nfrom = \"2023-13-01\"", "a date"},
    {"[replay]\nmarket = \"jp\"", "one of \"us\", \"kr\""},
    {"[data]\nstore = \"kis/1d\"", "is not a store coordinate"},
    {"[data]\nstore = \"kis/2d/005930\"", "the interval is one of"},
    {"[data]\nstore = [\"a/1d/x\", 3]", "a string or a list of strings"},
    {"[data]\nas_of = \"2026-10-01 25:00\"", "an instant"},
    {"[data]\nas_of = \"가나다라마\"", "an instant"},
    {"[data]\nas_of = \"2026-10-01T05:00+가:00\"", "an instant"},
    {"[data]\ntimezone = \"Asia/Seoul\"", "a fixed offset"},
    {"[data]\ninterval = \"2h\"", "one of \"1m\""},
    {"[data]\nfiles = [\"a,b.csv\"]", "holds a comma"},
    {"[data]\nsymbol = \"\"", "a non-empty string"},
    {"strategy = 3", "`strategy` is a string, not 3"},
  };
  for (const auto& [src, says] : cases) {
    const auto e = one_error(src);
    INFO(src << ": " << e);
    CHECK(has(e, says));
    CHECK(e.rfind(FILE_PATH + ":", 0) == 0);
  }
  // Every key's error, not only the first, each at its line.
  const auto parsed = spec("strategy = \"m.atls\"\nfoo = 1\n[replay]\ncash = 0\nfill = \"close\"\n");
  REQUIRE(parsed.errors_.size() == 3);
  CHECK(parsed.errors_[0].rfind(FILE_PATH + ":2: ", 0) == 0);
  CHECK(parsed.errors_[1].rfind(FILE_PATH + ":4: `replay.cash` is a number above 0, not 0", 0) == 0);
  CHECK(parsed.errors_[2].rfind(FILE_PATH + ":5: ", 0) == 0);
}

TEST_CASE("errors as diagnostics: a line in a file, the command line, and no one place") {
  const auto read = sp::read_text("[replay]\ncash = 0\n", FILE_PATH);
  REQUIRE(read.errors_.size() == 1);
  const auto line = read.errors_.front().to_json();
  CHECK(line.at("severity").as_string() == "error");
  CHECK(line.at("code").as_string() == "spec");
  CHECK(line.at("file").as_string() == FILE_PATH);
  CHECK(line.at("line").as_int64() == 2);
  CHECK(line.at("origin").as_string() == "file");
  sp::Entry e;
  REQUIRE(!sp::from_command_line("replay.cash=0", e));
  const auto cl = sp::check(e)->to_json();
  CHECK(cl.at("file").is_null());
  CHECK(cl.at("line").is_null());
  CHECK(cl.at("origin").as_string() == "command_line");
  CHECK(sp::check(e)->text() == "the command line: `replay.cash` is a number above 0, not 0");
  const auto nowhere = sp::Error::nowhere("give the closes").to_json();
  CHECK(nowhere.at("file").is_null());
  CHECK(nowhere.at("origin").as_string() == "file");
}

TEST_CASE("decimals lose their exponent exactly and keep their text") {
  const std::vector<std::pair<std::string, std::string>> exact{
    {"1.5e-4", "0.00015"},
    {"1e5", "100000"},
    {"1E+2", "100"},
    {"0.00015", "0.00015"},
    {"100000", "100000"},
    {"1.50", "1.5"},
    {"12.5e1", "125"},
    {"-2.5e-1", "-0.25"},
    {"0e7", "0"},
    {"0.000", "0"},
    {"123456789012345678e-18", "0.123456789012345678"},
  };
  for (const auto& [text, plain] : exact) {
    INFO(text);
    CHECK(sp::plain_decimal(text) == std::optional<std::string>(plain));
  }
  for (const auto* text : {"1e19", "1234567890123456789", "1e-256", "1e9999999999999999999"}) {
    INFO(text);
    CHECK(!sp::plain_decimal(text));
  }
  const auto parsed = spec("strategy = \"m.atls\"\n[replay]\ncash = 1e5\ncommission = 1.5e-4\n");
  CHECK(value_of(parsed, "replay.cash") == "100000");
  CHECK(value_of(parsed, "replay.commission") == "0.00015");
  // Read from the file's text, wherever on its line the number sits.
  const auto placed = spec("strategy = \"m.atls\"\n[data]\nsymbol = \"삼성전자\"; \n");
  CHECK(!placed.errors_.empty());
  const auto inline_table = spec("replay = { market = \"kr\", cash = 1_000_000.50, sell_tax = 0.002 }\n");
  REQUIRE(inline_table.errors_.empty());
  CHECK(value_of(inline_table, "replay.cash") == "1000000.5");
  CHECK(value_of(inline_table, "replay.sell_tax") == "0.002");
  const auto after_korean = spec(
    "data = { symbol = \"삼성전자 우선주\", files = [\"가.csv\"] }\nreplay = { fill = \"next-open\", commission = 0.000_15 }\n"
  );
  REQUIRE(after_korean.errors_.empty());
  CHECK(value_of(after_korean, "replay.commission") == "0.00015");
  const auto non_ascii = spec("instruments = { y = \"한국\", x = \"é\" }; \n");
  CHECK(!non_ascii.errors_.empty());
  const auto korean_then_number = spec("p = { a = \"한국어 텍스트\", b = 1.25 }\n");
  REQUIRE(korean_then_number.errors_.size() == 2);
  CHECK(has(korean_then_number.errors_[1], "unknown setting `p.b`"));
  const auto tabbed = spec("[replay]\n\tcash\t=\t2.50\t# tabs\ncommission =\t1.5e-4\r\nsell_tax = 0.002\r\n");
  REQUIRE(tabbed.errors_.empty());
  CHECK(value_of(tabbed, "replay.cash") == "2.5");
  CHECK(value_of(tabbed, "replay.commission") == "0.00015");
  CHECK(value_of(tabbed, "replay.sell_tax") == "0.002");
  // After a byte-order mark, on the first line.
  const auto marked = spec("\xEF\xBB\xBFreplay.cash = 2.50\nreplay.commission = 0.001\n");
  REQUIRE(marked.errors_.empty());
  CHECK(value_of(marked, "replay.cash") == "2.5");
  const auto params = run(WIDE_SPEC + "[params]\nentry = 1.50\nlookback = +30\n", single());
  REQUIRE(params.errors_.empty());
  CHECK(params.arg("param.entry") == std::optional<std::string>("1.50"));
  CHECK(params.arg("param.lookback") == std::optional<std::string>("30"));
  const auto offset = run(WIDE_SPEC + "[orders]\ntype = \"limit\"\noffset = 0.000_1\n", single());
  CHECK(offset.resolved_.offset_ == "0.0001");
}

TEST_CASE("numbers that are not decimals are refused, as are dates and times written bare") {
  for (const auto* value : {"0x10", "0o17", "0b1", "inf", "+inf", "-inf", "nan", "+nan"}) {
    const auto e = one_error(std::string("[replay]\ncash = ") + value + "\n");
    INFO(value << ": " << e);
    CHECK(has(e, "write a decimal number"));
    CHECK(e.rfind(FILE_PATH + ":2: ", 0) == 0);
  }
  for (const auto* value : {"2024-01-02", "2024-01-02T09:00:00", "09:00:00", "2024-01-02T09:00:00+09:00"}) {
    const auto e = one_error(std::string("[replay]\nfrom = ") + value + "\n");
    INFO(value << ": " << e);
    CHECK(has(e, "write a date or a time as a string"));
  }
  // In an array too.
  CHECK(has(one_error("[data]\nfiles = [\"a\", 0x10]\n"), "write a decimal number"));
}

TEST_CASE("what TOML holds is read by toml++: escapes, inline tables and multi-line strings") {
  const auto parsed = spec(
    "strategy = \"m\\u002Eatls\"\n[data]\nsymbol = 'A'\ncolumns = { open = \"Open\", close = \"\"\"Close\"\"\" }\n"
  );
  REQUIRE(parsed.errors_.empty());
  CHECK(value_of(parsed, "strategy") == "/specs/m.atls");
  CHECK(value_of(parsed, "data.columns.open") == "Open");
  CHECK(value_of(parsed, "data.columns.close") == "Close");
  // File order, whatever toml++'s table order.
  REQUIRE(parsed.entries_.size() == 4);
  CHECK(parsed.entries_[1].key_ == "data.symbol");
  CHECK(parsed.entries_[2].key_ == "data.columns.open");
}

TEST_CASE("decoded control characters are refused, with the advice for a Windows path") {
  for (
    const auto* src :
    {"[data]\nfile = \"C:\\temp\\new.csv\"\n", "[data]\nfile = \"a\\u0000b\"\n", "[data]\nsymbol = \"A\\tB\"\n",
      "[data]\nsymbol = \"A\\u007F\"\n", "[data]\nsymbols = [\"A\", \"B\\n\"]\n"}
  ) {
    const auto e = one_error(src);
    INFO(src << ": " << e);
    CHECK(has(e, "holds a control character"));
    CHECK(has(e, "single quotes or with forward slashes"));
  }
  CHECK(spec("[data]\nfile = 'C:\\temp\\new.csv'\n").errors_.empty());
  CHECK(has(cli_error(std::string("data.symbol=A\x01")), "control character (U+0001)"));
  CHECK(has(cli_error("data.files=[\"a\\tb\"]"), "control character (U+0009)"));
}

TEST_CASE("toml++ refuses what TOML forbids: keys and tables twice, a value and a table, characters") {
  const std::vector<std::pair<std::string, std::size_t>> cases{
    {"a = 1\na = 2", 2},
    {"[data]\nfile = \"x\"\n[data]\n", 3},
    {"[data]\ncolumns.open = \"o\"\n[data.columns]\nopen = \"p\"", 3},
    {"data = \"x\"\n[data]\nfile = \"y\"", 2},
    {"a.b = 1\na.b.c = 2", 2},
    {"data.file = \"x\"\n[data]\nsymbol = \"A\"", 2},
    {"[data]\nsymbol\xc2\xa0= \"A\"", 2},
    {"[data]\nsymbol = \"A\x01\"", 2},
    {"a = 1 # \x7f", 1},
    {"a = 1\r", 1},
    {"a = 012", 1},
    {"a = 1__0", 1},
    {"a = 1.", 1},
    {"a = yes", 1},
    {"a = \"open", 1},
    {"a = [1,\n", 1},
  };
  for (const auto& [src, line] : cases) {
    const auto parsed = spec(src);
    INFO(src);
    REQUIRE(parsed.errors_.size() == 1);
    INFO(parsed.errors_.front());
    CHECK(parsed.errors_.front().rfind(FILE_PATH + ":" + std::to_string(line) + ": ", 0) == 0);
  }
  CHECK(has(one_error("[[a]]\nb = 1\n"), "arrays of tables"));
  CHECK(has(one_error("a = [true]\n"), "an array holds strings or numbers"));
  CHECK(has(one_error("a = [[1]]\n"), "an array holds strings or numbers"));
  CHECK(has(one_error("\"a.b\" = 1\n"), "keys are bare"));
  CHECK(has(one_error("\"\" = 1\n"), "keys are bare"));
  // A tab and a no-break space inside a string, CRLF line ends and comments.
  const auto spaced = spec(
    "[data]\nsymbol = \"x\xc2\xa0y\"\t# a\xe3\x80\x80"
    "comment\r\nnames = 'n.csv'\n"
  );
  CHECK(spaced.errors_.empty());
  // Nesting is bounded before toml++ parses.
  CHECK(has(one_error("a = " + std::string(40, '[') + std::string(40, ']') + "\n"), "not TOML this version reads"));
  CHECK(has(one_error("a" + std::string(30, '.') + " = 1\n"), "not"));
}

TEST_CASE("values are normalized: paths against the spec's directory, lists, dates and counts") {
  const auto parsed = spec(
    "strategy = \"../m.atls\"\n[data]\nfiles = [\"a.csv\", \"/abs/b.csv\"]\nstore = \"x/1d/y\"\n"
    "as_of = \"2026-10-01T05:00:00Z\"\n[replay]\nfrom = \"2020-01-02\"\nto = 20201231\nslippage_ticks = 2\n"
  );
  REQUIRE(parsed.errors_.empty());
  CHECK(value_of(parsed, "strategy") == "/specs/../m.atls");
  CHECK(value_of(parsed, "data.files") == "/specs/a.csv,/abs/b.csv");
  CHECK(value_of(parsed, "data.store") == "x/1d/y");
  CHECK(value_of(parsed, "replay.from") == "20200102");
  CHECK(value_of(parsed, "replay.to") == "20201231");
  CHECK(value_of(parsed, "replay.slippage_ticks") == "2");
  CHECK(sp::instant_text("2026-10-01"));
  CHECK(sp::instant_text("2026-10-01 05:00"));
  CHECK(sp::instant_text("2026-10-01T05:00:00+09:00"));
  CHECK(!sp::instant_text("2026-10-01T5:00"));
  CHECK(!sp::instant_text("2026/10/01"));
  CHECK(!sp::instant_text("2026-10-01T05:00+9"));
}

TEST_CASE("a spec file names its program, and an unreadable one is one error") {
  const ScratchDir dir;
  const auto path = (dir.path() / "s.toml").string();
  std::string program;
  spit(path, "[replay]\ncash = 1\n");
  auto read = sp::read_file(path, program);
  REQUIRE(read.errors_.size() == 1);
  CHECK(has(read.errors_.front().text(), "no `strategy"));
  CHECK(read.errors_.front().to_json().at("line").is_null());
  spit(path, "strategy = 'm.atls'\n");
  read = sp::read_file(path, program);
  CHECK(read.errors_.empty());
  CHECK(program == (dir.path() / "m.atls").string());
  spit(path, "strategy = 'm.atls'\nfoo = 1\nbar = 2\n");
  CHECK(sp::read_file(path, program).errors_.size() == 2);
  read = sp::read_file((dir.path() / "none.toml").string(), program);
  REQUIRE(read.errors_.size() == 1);
  CHECK(read.errors_.front().code_ == "unreadable");
  CHECK(has(read.errors_.front().text(), "cannot read"));
  std::filesystem::create_directories(dir.path() / "d.toml");
  read = sp::read_file((dir.path() / "d.toml").string(), program);
  CHECK(read.errors_.front().code_ == "unreadable");
  spit(path, std::string(sp::MAX_SPEC_BYTES + 1, '#'));
  read = sp::read_file(path, program);
  CHECK(read.errors_.front().code_ == "unreadable");
}

TEST_CASE("the target among settings: a setting is a dotted key before its first equals sign") {
  CHECK(sp::is_setting("replay.cash=1e8"));
  CHECK(sp::is_setting("params.lookback=30"));
  CHECK(sp::is_setting("instruments.y=EWC"));
  CHECK(sp::is_setting("data.file=a=b.csv"));
  CHECK(sp::is_setting("data.columns.open=Open"));
  CHECK(sp::is_setting("a.b="));
  CHECK(!sp::is_setting("dip.toml"));
  CHECK(!sp::is_setting("dip.atls"));
  CHECK(!sp::is_setting("lookback=30"));
  CHECK(!sp::is_setting("./a=b.toml"));
  CHECK(!sp::is_setting("dir.v2/x=1.toml"));
  CHECK(!sp::is_setting(".a=1"));
  CHECK(!sp::is_setting("a..b=1"));
  CHECK(!sp::is_setting("a.=1"));
  CHECK(!sp::is_setting("a b.c=1"));
  CHECK(!sp::is_setting("é.x=1"));
}

TEST_CASE("command-line values are typed by their key, never read as TOML values") {
  CHECK(cli_value("instruments.stock=005930") == "005930");
  CHECK(cli_value("data.symbol=true") == "true");
  CHECK(cli_value("data.symbol=005930") == "005930");
  CHECK(cli_value("replay.cash=1e8") == "100000000");
  CHECK(cli_value("replay.cash=1_000.50") == "1000.5");
  CHECK(cli_value("replay.commission=1.5e-4") == "0.00015");
  CHECK(cli_value("replay.slippage_ticks=3") == "3");
  CHECK(cli_value("replay.shorts=true") == "true");
  CHECK(cli_value("replay.from=2020-01-02") == "20200102");
  CHECK(cli_value("replay.to=20201231") == "20201231");
  CHECK(cli_value("replay.market=kr") == "kr");
  CHECK(cli_value("data.timezone=+09:00") == "+09:00");
  CHECK(cli_value("data.as_of=2026-10-01T05:00Z") == "2026-10-01T05:00Z");
  CHECK(cli_value("data.symbols=A") == "A");
  CHECK(cli_value("data.symbols=[\"A\", \"B\"]") == "A,B");
  CHECK(cli_value("data.store=[\"kis/1d/005930\"]") == "kis/1d/005930");
  CHECK(cli_value("orders.offset=0.002") == "0.002");
  CHECK(cli_value("params.lookback=30") == "30");
  CHECK(cli_value("params.flag=true") == "true");
  CHECK(has(cli_error("replay.cash=abc"), "`replay.cash` is a number above 0, not \"abc\""));
  CHECK(has(cli_error("replay.cash=0x10"), "a number above 0"));
  CHECK(has(cli_error("replay.cash=inf"), "a number above 0"));
  CHECK(has(cli_error("replay.slippage_ticks=1.5"), "a whole number"));
  CHECK(has(cli_error("replay.shorts=1"), "`true` or `false`"));
  CHECK(has(cli_error("replay.shorts=yes"), "`true` or `false`"));
  CHECK(has(cli_error("replay.from=2020-02-30"), "a date"));
  CHECK(has(cli_error("orders.offset=2e-3"), "without an exponent"));
  CHECK(has(cli_error("data.symbols=[\"A\", 3]"), "an array holds strings"));
  CHECK(has(cli_error("data.symbols=[\"A\""), "is not a TOML array of strings"));
  CHECK(has(cli_error("data.file="), "a non-empty string"));
  CHECK(has(cli_error("params.lookback=ten"), "is a number or a boolean, not \"ten\""));
  CHECK(has(cli_error("strategy.x=1"), "unknown setting `strategy.x`"));
  CHECK(has(cli_error("output.fills=/tmp/f.csv"), "unknown setting `output.fills`"));
  CHECK(has(cli_error("run.id=r0123456789ab"), "unknown setting `run.id`"));
  CHECK(has(cli_error("build.algotradelib=/opt"), "cmdatl builds against the algotradelib it was built with"));
  CHECK(cli_error("replay.cash=1e8").empty());
  // A relative path is read against the working directory.
  const ScratchDir dir;
  const InDir here(dir.path());
  CHECK(cli_value("data.file=bars.csv") == (dir.path() / "bars.csv").string());
  CHECK(cli_value("data.files=[\"a.csv\", \"/b.csv\"]") == (dir.path() / "a.csv").string() + ",/b.csv");
}

TEST_CASE("layers merge, the command line winning, and paths are read against the file that sets them") {
  const auto file = spec(WIDE_SPEC + "[replay]\ncash = 7\nshorts = true\n[params]\nlookback = 30\n");
  REQUIRE(file.errors_.empty());
  std::vector<std::string> errors;
  const auto line = cli({"params.lookback=40", "replay.cash=8"}, errors);
  REQUIRE(errors.empty());
  const auto r = resolve({file.entries_, line}, single());
  REQUIRE(r.errors_.empty());
  CHECK(r.arg("replay.cash") == std::optional<std::string>("8"));
  CHECK(r.arg("replay.shorts") == std::optional<std::string>("true"));
  CHECK(r.arg("param.lookback") == std::optional<std::string>("40"));
  CHECK(r.arg("data.source") == std::optional<std::string>("wide"));
  CHECK(r.arg("data.file") == std::optional<std::string>("/specs/closes.csv"));
  CHECK(r.resolved_.indicators_ == "incremental");
  CHECK(!r.resolved_.store_);
  // data.source first, then the settings in the order first given, then the parameters.
  REQUIRE(r.resolved_.args_.size() == 5);
  CHECK(r.resolved_.args_.front().first == "data.source");
  CHECK(r.resolved_.args_.back().first == "param.lookback");
  // Odd directory names: quotes, non-ASCII and a backslash.
  const ScratchDir dir;
  const auto odd = dir.path() / "a dir \"q\" é 한 back\\slash";
  std::filesystem::create_directories(odd / "specs");
  std::filesystem::create_directories(dir.path() / "elsewhere");
  spit(odd / "specs" / "s.toml", "strategy = '../programs/p.atls'\n[data]\nfile = 'closes.csv'\nopen = 'opens.csv'\n");
  const InDir here(dir.path() / "elsewhere");
  std::string program;
  const auto read = sp::read_file((odd / "specs" / "s.toml").string(), program);
  REQUIRE(read.errors_.empty());
  CHECK(program == (odd / "specs" / "../programs/p.atls").string());
  const auto given = cli({"data.high=highs.csv"}, errors);
  const auto layered = resolve({read.entries_, given}, single());
  REQUIRE(layered.errors_.empty());
  CHECK(layered.arg("data.file") == std::optional<std::string>((odd / "specs" / "closes.csv").string()));
  CHECK(layered.arg("data.high") == std::optional<std::string>((dir.path() / "elsewhere" / "highs.csv").string()));
}

TEST_CASE("parameters are checked against the program, from the file and the command line") {
  const auto p = single();
  const auto ok = run(WIDE_SPEC + "[params]\nlookback = 30\nentry = 1.50\nflag = true\n", p);
  REQUIRE(ok.errors_.empty());
  CHECK(ok.arg("param.lookback") == std::optional<std::string>("30"));
  CHECK(ok.arg("param.entry") == std::optional<std::string>("1.50"));
  CHECK(ok.arg("param.flag") == std::optional<std::string>("1"));
  const std::vector<std::pair<std::string, std::string>> cases{
    {"lokback = 3", "no parameter `lokback` (it has lookback, entry, flag)"},
    {"lookback = 2.5", "`lookback` is an int parameter"},
    {"lookback = 1e16", "`lookback` is an int parameter"},
    {"lookback = 3e1", "written as an integer"},
    {"lookback = 2.0000000000000001", "written as an integer"},
    {"lookback = true", "`lookback` is an int parameter"},
    {"entry = false", "`entry` is a real parameter"},
    {"flag = 1", "`flag` is a bool parameter"},
  };
  for (const auto& [params, says] : cases) {
    const auto r = run(WIDE_SPEC + "[params]\n" + params + "\n", p);
    INFO(params);
    REQUIRE(r.errors_.size() == 1);
    CHECK(has(r.errors_.front(), says));
  }
  const auto file = spec(WIDE_SPEC);
  for (
    const auto& [word, says] : std::vector<std::pair<std::string, std::string>>{
      {"params.flag=1", "`flag` is a bool parameter"},
      {"params.lookback=2.5", "`lookback` is an int parameter"},
      {"params.nope=1", "the program has no parameter `nope`"},
    }
  ) {
    std::vector<std::string> errors;
    const auto line = cli({word}, errors);
    REQUIRE(errors.empty());
    const auto r = resolve({file.entries_, line}, p);
    INFO(word);
    REQUIRE(r.errors_.size() == 1);
    CHECK(has(r.errors_.front(), says));
    CHECK(r.errors_.front().rfind("the command line: ", 0) == 0);
  }
  std::vector<std::string> errors;
  const auto line = cli({"params.lookback=+2", "params.entry=0.5", "params.flag=false"}, errors);
  const auto r = resolve({file.entries_, line}, p);
  REQUIRE(r.errors_.empty());
  CHECK(r.arg("param.lookback") == std::optional<std::string>("2"));
  CHECK(r.arg("param.entry") == std::optional<std::string>("0.5"));
  CHECK(r.arg("param.flag") == std::optional<std::string>("0"));
  sp::Program none;
  none.mode_ = "single";
  CHECK(has(run(WIDE_SPEC + "[params]\nx = 1\n", none).errors_.front(), "(it has none)"));
}

TEST_CASE("data keys belong to their source") {
  const auto p = single();
  const std::vector<std::pair<std::string, std::string>> cases{
    {"[data]\nfile = \"c.csv\"\ncolumns.open = \"Open\"", "`data.columns.open` applies to data.source = \"csv\""},
    {"[data]\nsource = \"csv\"\nfile = \"c.csv\"\nsymbol = \"A\"\nnames = \"n.csv\"",
      "`data.names` applies to data.source = \"wide\""},
    {"[data]\nstore = \"a/1d/b\"\nstore_root = \"/s\"\nfile = \"c.csv\"",
      "`data.file` applies to data.source = \"wide\" or \"csv\""},
    {"[data]\nsource = \"wide\"\nfile = \"c.csv\"\nstore = \"a/1d/b\"",
      "`data.store` applies to data.source = \"store\""},
    {"[data]\nnames = \"n.csv\"", "give the closes"},
    {"[data]\nfile = \"c.csv\"\nclose = \"c.csv\"", "name the same thing"},
    {"[data]\nsource = \"csv\"", "give `data.file` or `data.files`"},
    {"[data]\nsource = \"csv\"\nfile = \"c.csv\"", "give `data.symbol`"},
    {"[data]\nsource = \"csv\"\nfiles = [\"a\", \"b\"]\nsymbols = \"A\"", "2 files and data.symbols 1 symbols"},
    {"[data]\nsource = \"csv\"\nfile = \"c\"\nsymbol = \"A\"\nsymbol_column = \"s\"", "both name symbols"},
    {"[data]\nsource = \"csv\"\nlayout = \"wide\"\nfile = \"c\"\nsymbol = \"A\"", "applies to data.layout = \"long\""},
    {"[data]\nstore = \"a/1d/b\"", "give `data.store_root`"},
    {"[data]\nsource = \"store\"\nstore_root = \"/s\"", "give `data.store`"},
    {"[data]\nfile = \"c\"\n[replay]\nfrom = 20200102\nto = 20200101", "replay.from is after replay.to"},
  };
  for (const auto& [src, says] : cases) {
    const auto r = run("strategy = \"m.atls\"\n" + src + "\n", p);
    INFO(src);
    REQUIRE(r.errors_.size() == 1);
    CHECK(has(r.errors_.front(), says));
  }
  const auto store = run("strategy = \"m.atls\"\n[data]\nstore = \"a/1d/b\"\nstore_root = \"/s\"\n", p);
  REQUIRE(store.errors_.empty());
  CHECK(store.resolved_.store_);
  CHECK(store.arg("data.source") == std::optional<std::string>("store"));
  const auto csv =
    run("strategy = \"m.atls\"\n[data]\nsource = \"csv\"\nfiles = [\"a\", \"b\"]\nsymbols = [\"A\", \"B\"]\n", p);
  REQUIRE(csv.errors_.empty());
  CHECK(csv.arg("data.symbols") == std::optional<std::string>("A,B"));
}

TEST_CASE("fills match the program and the data") {
  sp::Program legs;
  legs.mode_ = "universe";
  legs.legs_ = true;
  const auto plain = single();
  const std::vector<std::tuple<std::string, const sp::Program*, std::string>> cases{
    {"[data]\nfile = \"c\"", &legs, "set replay.fill = \"legs\""},
    {"[data]\nfile = \"c\"\n[replay]\nfill = \"legs\"", &plain, "for a program with on_open or on_close"},
    {"[data]\nfile = \"c\"\nopen = \"o\"\nhigh = \"h\"\n[replay]\nfill = \"legs\"", &legs, "needs `data.low`"},
    {"[data]\nsource = \"csv\"\nfile = \"c\"\nsymbol = \"A\"\n[replay]\nfill = \"open-prev-close\"", &plain,
      "reads wide tables"},
    {"[data]\nsource = \"csv\"\nfile = \"c\"\nsymbol = \"A\"\ninterval = \"1h\"\n[replay]\nfill = \"legs\"", &legs,
      "needs daily bars"},
    {"[data]\nstore = [\"a/1d/b\", \"a/1m/c\"]\nstore_root = \"/s\"\n[replay]\nfill = \"legs\"", &legs,
      "not \"a/1m/c\""},
  };
  for (const auto& [src, program, says] : cases) {
    const auto r = run("strategy = \"m.atls\"\n" + src + "\n", *program);
    INFO(src);
    REQUIRE(r.errors_.size() == 1);
    CHECK(has(r.errors_.front(), says));
  }
}

TEST_CASE("instruments match the program") {
  sp::Program named;
  named.mode_ = "named";
  named.instruments_ = {"y", "x"};
  const auto ok = run(WIDE_SPEC + "[instruments]\ny = \"USO\"\nx = \"GLD\"\n", named);
  REQUIRE(ok.errors_.empty());
  CHECK(ok.arg("instrument.y") == std::optional<std::string>("USO"));
  const auto e = run(WIDE_SPEC + "[instruments]\ny = \"USO\"\nz = \"GLD\"\n", named).errors_;
  REQUIRE(e.size() == 2);
  CHECK(has(e[0], "map it to a symbol, `x = \"...\"`"));
  CHECK(has(e[1], "no instrument `z` (it names y, x)"));
  CHECK(has(run(WIDE_SPEC + "[instruments]\na = \"A\"\nb = \"B\"\n", single()).errors_.front(), "at most one"));
  sp::Program universe;
  universe.mode_ = "universe";
  CHECK(run(WIDE_SPEC + "[instruments]\na = \"A\"\nb = \"B\"\n", universe).errors_.empty());
  // From the command line, a code stays a string.
  const auto file = spec(WIDE_SPEC);
  std::vector<std::string> errors;
  const auto line = cli({"instruments.y=005930", "instruments.x=000660"}, errors);
  const auto r = resolve({file.entries_, line}, named);
  REQUIRE(r.errors_.empty());
  CHECK(r.arg("instrument.y") == std::optional<std::string>("005930"));
}

TEST_CASE("orders are checked on their text, across the file and the command line") {
  const auto p = single();
  const auto none = run(WIDE_SPEC, p);
  CHECK(none.resolved_.order_type_ == "market");
  CHECK(none.resolved_.offset_.empty());
  for (const auto& [k, v] : none.resolved_.args_) {
    CHECK(k.rfind("orders.", 0) != 0);
  }
  const auto limit = run(WIDE_SPEC + "[orders]\ntype = \"limit\"\n", p);
  CHECK(limit.resolved_.order_type_ == "limit");
  CHECK(limit.arg("orders.type") == std::optional<std::string>("limit"));
  CHECK(!limit.arg("orders.offset"));
  // Kept as written: the double nearest 0.299999999999999999 is 0.3, which the check never sees.
  for (const auto* offset : {"0", "0.0", "0.002", "0.000_1", "0.299999999999999999", "0.29"}) {
    const auto r = run(WIDE_SPEC + "[orders]\ntype = \"limit\"\noffset = " + offset + "\n", p);
    INFO(offset);
    REQUIRE(r.errors_.empty());
    std::string written = offset;
    written.erase(std::remove(written.begin(), written.end(), '_'), written.end());
    CHECK(r.arg("orders.offset") == std::optional<std::string>(written));
    CHECK(r.resolved_.offset_ == written);
  }
  const std::vector<std::pair<std::string, std::string>> refused{
    {"2e-3", "without an exponent"},
    {"2E-3", "without an exponent"},
    {"0.3", "at least 0 and below 0.3"},
    {"0.30", "at least 0 and below 0.3"},
    {"1", "at least 0 and below 0.3"},
    {"1.0", "at least 0 and below 0.3"},
    {"-0.001", "at least 0 and below 0.3"},
    {"0.2999999999999999999", "at most 18 fractional digits"},
    {"\"0.002\"", "a fraction"},
    {"true", "a fraction"},
  };
  for (const auto& [offset, says] : refused) {
    const auto parsed = spec(WIDE_SPEC + "[orders]\ntype = \"limit\"\noffset = " + offset + "\n");
    INFO(offset);
    REQUIRE(parsed.errors_.size() == 1);
    CHECK(has(parsed.errors_.front(), says));
    CHECK(parsed.errors_.front().rfind(FILE_PATH + ":6: `orders.offset` is ", 0) == 0);
  }
  for (const auto* orders : {"offset = 0.002", "type = \"market\"\noffset = 0"}) {
    const auto r = run(WIDE_SPEC + "[orders]\n" + orders + "\n", p);
    REQUIRE(r.errors_.size() == 1);
    CHECK(has(r.errors_.front(), "`orders.offset` prices a limit"));
  }
  CHECK(has(spec(WIDE_SPEC + "[orders]\ntype = \"stop\"\n").errors_.front(), "one of \"market\", \"limit\""));
  // The offset in the file, the order type on the command line, and the other way round.
  const auto offset_only = spec(WIDE_SPEC + "[orders]\noffset = 0.299999999999999999\n");
  REQUIRE(offset_only.errors_.empty());
  std::vector<std::string> errors;
  const auto typed = resolve({offset_only.entries_, cli({"orders.type=limit"}, errors)}, p);
  REQUIRE(typed.errors_.empty());
  CHECK(typed.resolved_.offset_ == "0.299999999999999999");
  const auto untyped = resolve({offset_only.entries_}, p);
  REQUIRE(untyped.errors_.size() == 1);
  CHECK(untyped.errors_.front().rfind(FILE_PATH + ":5: `orders.offset` prices a limit", 0) == 0);
  const auto limit_file = spec(WIDE_SPEC + "[orders]\ntype = \"limit\"\n");
  const auto offset_given = resolve({limit_file.entries_, cli({"orders.offset=0.002"}, errors)}, p);
  REQUIRE(offset_given.errors_.empty());
  CHECK(offset_given.resolved_.offset_ == "0.002");
  CHECK(errors.empty());
}

TEST_CASE("build paths: build.algotradelib refused, build.duckdb checked for CMake only for a store run") {
  const auto p = single();
  CHECK(has(one_error(WIDE_SPEC + "[build]\nalgotradelib = \"/a\"\n"), FILE_PATH + ":5: `build.algotradelib`"));
  CHECK(run(WIDE_SPEC + "[build]\nduckdb = '/a\"b'\n", p).errors_.empty());
  const auto store =
    run("strategy = \"m.atls\"\n[data]\nstore = \"a/1d/b\"\nstore_root = \"/s\"\n[build]\nduckdb = '/a$b'\n", p);
  REQUIRE(store.errors_.size() == 1);
  CHECK(store.errors_.front().rfind(FILE_PATH + ":6: `build.duckdb`", 0) == 0);
  const auto found = run(
    "strategy = \"m.atls\"\n[data]\nstore = \"a/1d/b\"\nstore_root = \"/s\"\n[build]\nduckdb = 'duck'\nindicators = "
    "\"exact\"\n",
    p
  );
  REQUIRE(found.errors_.empty());
  CHECK(found.resolved_.duckdb_ == "/specs/duck");
  CHECK(found.resolved_.indicators_ == "exact");
  for (const auto& [k, v] : found.resolved_.args_) {
    CHECK(k.rfind("build.", 0) != 0);
  }
}

TEST_CASE("the driver's arguments are bounded and only ever settings' keys") {
  sp::Program universe;
  universe.mode_ = "universe";
  const auto file = spec(WIDE_SPEC);
  std::vector<std::string> errors;
  const auto huge = cli({"data.names=" + std::string(sp::MAX_VALUE, 'n')}, errors);
  REQUIRE(errors.empty());
  const auto r = resolve({file.entries_, huge}, universe);
  REQUIRE(r.errors_.size() == 1);
  CHECK(has(r.errors_.front(), "is longer than 4096 bytes"));
  CHECK(r.errors_.front().rfind("the command line: ", 0) == 0);
  const auto fits = cli({"data.names=/" + std::string(sp::MAX_VALUE - 1, 'n')}, errors);
  CHECK(resolve({file.entries_, fits}, universe).errors_.empty());
  const auto long_key = cli({"instruments." + std::string(130, 'k') + "=A"}, errors);
  REQUIRE(errors.empty());
  CHECK(has(resolve({file.entries_, long_key}, universe).errors_.front(), "longer than a key may be"));
  std::string many = WIDE_SPEC + "[instruments]\n";
  for (int k = 0; k < 1000; ++k) {
    many += "i" + std::to_string(k) + " = \"S" + std::to_string(k) + "\"\n";
  }
  const auto too_many = run(many, universe);
  REQUIRE(too_many.errors_.size() == 1);
  CHECK(has(too_many.errors_.front(), "more than 1000"));
  // Whatever is given, the arguments' keys are settings' keys: never the driver's own run.id or output.fills.
  const auto every = run(
    WIDE_SPEC +
      "[replay]\nmarket = \"kr\"\ncash = 1\n[orders]\ntype = \"limit\"\noffset = 0.01\n[instruments]\na = \"A\"\n",
    universe
  );
  REQUIRE(every.errors_.empty());
  for (const auto& [k, v] : every.resolved_.args_) {
    INFO(k);
    CHECK(k != "run.id");
    CHECK(k != "output.fills");
    CHECK((sp::known(k) != nullptr || k.rfind("instrument.", 0) == 0 || k.rfind("param.", 0) == 0));
  }
}
#endif
