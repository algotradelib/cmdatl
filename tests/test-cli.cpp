#include "./support.hpp"

#include <cmdatl/cli.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <new>

/**
 * @brief The test binary's operator new: the usual one, except for the allocation `support::fail_allocation` arms.
 * @param size The size.
 * @return The memory.
 */
void* operator new(std::size_t size) {
  long armed = support::fail_at.load();
  while (armed > 0 && !support::fail_at.compare_exchange_weak(armed, armed - 1)) {
  }
  if (armed == 1) {
    throw std::bad_alloc();
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

/**
 * @brief The matching operator delete.
 * @param memory The memory.
 */
void operator delete(void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief The matching sized operator delete.
 * @param memory The memory.
 */
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

using namespace support;

namespace {


/**
 * @brief A table with one command that "does something" and records that it ran, as a sending command would.
 * @param ran Set when the command runs.
 * @return The table.
 */
cmdatl::Table acting_table(const std::shared_ptr<int>& ran) {
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"trade", "plan"}, "Trade", "a stand-in that acts")
      .alias({"order", "plan"})
      .example("cmdatl trade plan 005930")
      .arg("SYMBOL", "an instrument")
      .returns("order", "what was done")
      .exits(cmdatl::SEND_EXITS)
      .sends()
      .runs([ran](const cmdatl::Invocation& invocation, const cmdatl::Table&, cmdatl::Output&) {
        ++*ran;
        cmdatl::Reply reply;
        reply.body_["result"] = "done";
        reply.body_["order"] = boost::json::object{{"symbol", invocation.get("SYMBOL")}, {"id", "o1"}};
        reply.body_["env"] = "live";
        return reply;
      })
  );
  return cmdatl::Table(std::move(commands));
}

/**
 * @brief The inline cmdatl references of a Markdown line: each code span starting with `cmdatl `, whole.
 * @param line The line.
 * @return The spans' texts, without the backticks.
 */
std::vector<std::string> inline_references(const std::string& line) {
  static const std::regex span("`(cmdatl [^`]*)`");
  std::vector<std::string> out;
  for (std::sregex_iterator it(line.begin(), line.end(), span), end; it != end; ++it) {
    out.push_back((*it)[1].str());
  }
  return out;
}

/**
 * @brief Whether a fenced documentation line's cmdatl invocation is one cmdatl accepts (parsed, never run).
 * @param line The line, starting with `cmdatl ` (after optional `$ `).
 * @param table The commands.
 * @return Whether `prepare` accepts it.
 */
bool accepts_invocation(const std::string& line, const cmdatl::Table& table) {
  const auto at = line.find("cmdatl ");
  const auto args = shell_words(line.substr(at + 7));
  return args && cmdatl::prepare(*args, table, false).action_ != cmdatl::Prepared::Action::REFUSE;
}

/**
 * @brief Whether an inline code span such as `cmdatl order plan` names a group exactly, or a command with no more
 *   words than its arguments take (an inline reference may leave out required arguments).
 * @param span The span's text, without the backticks.
 * @param table The commands.
 * @return Whether it names something cmdatl has.
 */
bool names_command(const std::string& span, const cmdatl::Table& table) {
  const auto read = shell_words(span);
  if (!read) {
    return false;
  }
  auto words = *read;
  if (words.empty() || words.front() != "cmdatl") {
    return false;
  }
  words.erase(words.begin());
  std::vector<std::string> path;
  for (const auto& word : words) {
    if (word.front() == '-') {
      break;
    }
    path.push_back(word);
  }
  if (path.empty()) {
    return cmdatl::prepare(words, table, false).action_ != cmdatl::Prepared::Action::REFUSE;
  }
  const auto found = table.resolve(path);
  if (found.command_ == nullptr) {
    return table.is_group(path);
  }
  const auto& args = found.command_->args_;
  const bool variadic = !args.empty() && args.back().variadic_;
  return variadic || path.size() - found.used_ <= args.size();
}

}  // namespace

TEST_CASE("the table is sound: paths, arguments, options, errors and exit codes") {
  const cmdatl::Table table(cmdatl::command_list());
  const auto problems = cmdatl::check_table(table);
  for (const auto& problem : problems) {
    INFO(problem);
  }
  CHECK(problems.empty());
}

TEST_CASE("the help and the schema list every command from the table") {
  const cmdatl::Table table(cmdatl::command_list());
  const auto help = run({"--help"}).out_;
  const auto schema = reply_of(run({"schema"}).out_);
  CHECK(schema.at("schema_version").as_int64() == cmdatl::SCHEMA_VERSION);
  CHECK(schema.at("commands").as_array().size() == table.commands().size());
  CHECK(schema.at("global_options").as_array().size() == cmdatl::global_options().size());
  for (const auto& command : table.commands()) {
    INFO(cmdatl::join(command.path_));
    CHECK(help.find("  " + cmdatl::join(command.path_)) != std::string::npos);
  }
  CHECK(reply_of(run({"schema", "order"}).out_).at("commands").as_array().size() == 6);
  const auto one = reply_of(run({"schema", "order.plan"}).out_);
  REQUIRE(one.at("commands").as_array().size() == 1);
  const auto& entry = one.at("commands").as_array()[0].as_object();
  CHECK(entry.at("may_send").as_bool() == false);
  CHECK(!entry.at("errors").as_array().empty());
  const auto errors = reply_of(run({"schema", "errors"}).out_);
  CHECK(errors.at("errors").as_array().size() == cmdatl::error_catalog().size());
}

TEST_CASE("a mistyped command gets the closest names") {
  const auto ran = run({"stauts"});
  CHECK(ran.exit_ == 2);
  CHECK(code_of(ran.out_) == "usage.unknown_command");
  CHECK(ran.out_.find("status") != std::string::npos);
  const auto group = run({"order"});
  CHECK(group.exit_ == 2);
  CHECK(code_of(group.out_) == "usage.unknown_command");
}

TEST_CASE("arguments are checked against the command before anything runs") {
  CHECK(code_of(run({"order", "plan", "005930", "--target", "10"}).out_) == "usage.missing_argument");
  CHECK(code_of(run({"env", "--nonsense"}).out_) == "usage.bad_option");
  CHECK(code_of(run({"--format", "yaml", "env"}).out_) == "usage.bad_format");
  // One value where one is declared: a second plan id or file is refused, not silently dropped.
  const auto two = run({"order", "place", "p1", "p2"});
  CHECK(two.exit_ == 2);
  CHECK(code_of(two.out_) == "usage.bad_option");
  CHECK(run({"strategy", "backtest", "a.toml", "b.toml"}).exit_ == 2);
  // A repeated option takes one value per occurrence; the line that parses runs, and finds no atlc here.
  CHECK(run({"strategy", "backtest", "s.toml", "-P", "a=1", "stray"}).exit_ == 2);
  ::setenv("CMDATL_ATLC", "/nonexistent/atlc", 1);
  const auto parsed = run({"strategy", "backtest", "s.toml", "-P", "a=1", "-P", "b=2"});
  ::unsetenv("CMDATL_ATLC");
  CHECK(parsed.exit_ == 1);
  CHECK(code_of(parsed.out_) == "atlc.missing");
}

TEST_CASE("tune and check are gone; new and backtest are the strategy commands that work") {
  const cmdatl::Table table(cmdatl::command_list());
  const auto help = run({"--help"}).out_;
  const auto schema = reply_of(run({"schema"}).out_);
  for (const auto* gone : {"tune", "check"}) {
    INFO(gone);
    CHECK(help.find(std::string("strategy ") + gone) == std::string::npos);
    CHECK(table.resolve({"strategy", gone}).command_ == nullptr);
    const auto ran = run({"strategy", gone, "dip.toml"});
    CHECK(ran.exit_ == 2);
    CHECK(code_of(ran.out_) == "usage.unknown_command");
    for (const auto& entry : schema.at("commands").as_array()) {
      CHECK(entry.as_object().at("command").as_string() != std::string("strategy.") + gone);
    }
  }
  CHECK(help.find("tuning") == std::string::npos);
  std::vector<std::string> available;
  for (const auto* command : table.select({"strategy"})) {
    if (command->available_) {
      available.push_back(cmdatl::join(command->path_));
    }
  }
  CHECK(available == std::vector<std::string>{"strategy new", "strategy backtest"});
  const auto reference = table.resolve({"strategy", "reference"});
  REQUIRE(reference.command_ != nullptr);
  CHECK(!reference.command_->available_);
  CHECK(cmdatl::SCHEMA_VERSION == 3);
  const auto backtest = reply_of(run({"schema", "strategy.backtest"}).out_).at("commands").as_array()[0].as_object();
  CHECK(backtest.at("returns").as_object().contains("params_given"));
  CHECK(!backtest.at("returns").as_object().contains("params"));
  const auto& exits = backtest.at("exits").as_array();
  CHECK(std::find(exits.begin(), exits.end(), boost::json::value(130)) != exits.end());
  CHECK(std::find(exits.begin(), exits.end(), boost::json::value(5)) != exits.end());
  CHECK(declares({"strategy", "new"}, 130, "interrupted"));
  // Each catalog code's own exit is among the exits of every command that declares it.
  for (const auto& command : table.commands()) {
    for (const auto& code : command.errors_) {
      const auto spec = std::find_if(cmdatl::error_catalog().begin(), cmdatl::error_catalog().end(), [&code](const auto& e) {
        return e.code_ == code;
      });
      REQUIRE(spec != cmdatl::error_catalog().end());
      INFO(cmdatl::join(command.path_) << " " << code);
      CHECK(declares(command.path_, static_cast<int>(spec->exit_), code));
    }
  }
}

TEST_CASE("global options never take another option as their value and stop at --") {
  const auto fields = run({"env", "--fields", "--json"});
  CHECK(fields.exit_ == 2);
  CHECK(code_of(fields.out_) == "usage.bad_option");
  CHECK(run({"env", "--value", "--json"}).exit_ == 2);
  CHECK(run({"env", "--fields=cmdatl"}).exit_ == 0);
  // After --, --help is the schema's selector, not a request for help.
  const auto after = run({"schema", "--", "--help"});
  CHECK(after.exit_ == 2);
  CHECK(code_of(after.out_) == "usage.unknown_command");
}

TEST_CASE("a planned command answers that it is not built yet") {
  const auto ran = run({"order", "plan", "005930", "--target", "10", "--limit", "78200"});
  CHECK(ran.exit_ == 1);
  CHECK(code_of(ran.out_) == "unavailable.not_yet");
  CHECK(reply_of(ran.out_).at("env").as_string() == "offline");
}

TEST_CASE("fields and value select from a reply, nested selections included") {
  const auto selected = reply_of(run({"env", "--fields", "cmdatl,atlc.minimum"}).out_);
  CHECK(selected.contains("env"));
  CHECK(selected.contains("cmdatl"));
  CHECK(selected.at("atlc").as_object().size() == 1);
  const auto selector = "commands.args.name,commands.args.required";
  const auto args = reply_of(run({"schema", "order.plan", "--fields", selector}).out_);
  const auto& arg = args.at("commands").as_array()[0].as_object().at("args").as_array()[0].as_object();
  CHECK(arg.contains("name"));
  CHECK(arg.contains("required"));
  CHECK(!arg.contains("variadic"));
  const auto both = cmdatl::select_fields(
    boost::json::parse(R"({"a":{"b":{"x":1,"y":2,"z":3},"c":4}})"),
    {"a.b.y", "a.b.x", "a.b"}
  );
  CHECK(both.as_object().at("a").as_object().at("b").as_object().size() == 3);
  const auto value = run({"--value", "cmdatl", "env"});
  CHECK(value.exit_ == 0);
  CHECK(value.out_ == std::string(CMDATL_VERSION) + "\n");
}

TEST_CASE("a field typo never hides what a command did") {
  const auto ran = std::make_shared<int>(0);
  const auto table = acting_table(ran);
  // A field no reply of the command has: refused before the command runs.
  const auto early = run_with({"trade", "plan", "005930", "--value", "ordr.id"}, table);
  CHECK(early.exit_ == 2);
  CHECK(code_of(early.out_) == "usage.no_such_field");
  CHECK(*ran == 0);
  // A path missing only after it ran: the whole reply, with a warning, and the command's own exit code.
  const auto late = run_with({"trade", "plan", "005930", "--value", "order.typo"}, table);
  CHECK(*ran == 1);
  CHECK(late.exit_ == 0);
  const auto reply = reply_of(late.out_);
  CHECK(reply.at("result").as_string() == "done");
  CHECK(reply.at("order").as_object().at("id").as_string() == "o1");
  CHECK(reply.at("warning").as_object().at("code").as_string() == "usage.no_such_field");
  // A command cannot set its own env: cmdatl does.
  CHECK(reply.at("env").as_string() == "offline");
  // An index far past any array is a missing field, never a crash.
  const auto huge = run({"schema", "--value", "commands.99999999999999999999999999999999"});
  CHECK(huge.exit_ == 0);
  CHECK(reply_of(huge.out_).contains("warning"));
}

TEST_CASE("an alias works in the parser, the schema and the help alike") {
  const auto ran = std::make_shared<int>(0);
  const auto table = acting_table(ran);
  const auto called = run_with({"order", "plan", "005930"}, table);
  CHECK(called.exit_ == 0);
  CHECK(reply_of(called.out_).at("note").as_string() == "cmdatl order plan is now cmdatl trade plan");
  std::vector<cmdatl::Command> with_schema;
  const auto acting = acting_table(ran);
  with_schema.push_back(acting.commands().front());
  for (const auto& command : cmdatl::command_list()) {
    if (command.path_ == std::vector<std::string>{"schema"}) {
      with_schema.push_back(command);
    }
  }
  const cmdatl::Table renamed(std::move(with_schema));
  const auto by_alias = reply_of(run_with({"schema", "order.plan"}, renamed).out_);
  REQUIRE(by_alias.contains("commands"));
  CHECK(by_alias.at("commands").as_array()[0].as_object().at("command").as_string() == "trade.plan");
  CHECK(run_with({"order", "--help"}, renamed).out_.find("trade plan") != std::string::npos);
}

TEST_CASE("the format follows the terminal unless asked, and text errors keep env") {
  CHECK(run({"env"}, false).out_.front() == '{');
  CHECK(run({"env"}, true).out_.front() != '{');
  CHECK(run({"--json", "env"}, true).out_.front() == '{');
  CHECK(run({"--format", "text", "env"}, false).out_.front() != '{');
  CHECK(run({"--format=json", "env"}, true).out_.front() == '{');
  const auto error = run({"--format", "text", "env", "--bad"});
  CHECK(error.out_.empty());
  CHECK(error.err_.find("error: ") == 0);
  CHECK(error.err_.find("env: offline") != std::string::npos);
}

TEST_CASE("every cmdatl invocation in the docs and skills parses, and every inline reference exists") {
  const cmdatl::Table table(cmdatl::command_list());
  const std::filesystem::path root(CMDATL_SOURCE_DIR);
  std::vector<std::filesystem::path> files{root / "README.md"};
  for (const auto* dir : {"docs", "skills"}) {
    if (std::filesystem::is_directory(root / dir)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root / dir)) {
        if (entry.path().extension() == ".md") {
          files.push_back(entry.path());
        }
      }
    }
  }
  std::size_t checked = 0;
  for (const auto& file : files) {
    std::ifstream in(file);
    std::string line;
    bool fenced = false;
    while (std::getline(in, line)) {
      if (line.rfind("```", 0) == 0) {
        fenced = !fenced;
        continue;
      }
      INFO(file.filename().string() << ": " << line);
      if (fenced) {
        const auto at = line.find("cmdatl ");
        if (at != std::string::npos && line.find_first_not_of(" $") == at) {
          CHECK(accepts_invocation(line, table));
          ++checked;
        }
        continue;
      }
      for (const auto& reference : inline_references(line)) {
        CHECK(names_command(reference, table));
        ++checked;
      }
    }
  }
  CHECK(checked > 0);
}

TEST_CASE("a missing selection after the command ran keeps the whole reply") {
  const auto ran = std::make_shared<int>(0);
  const auto table = acting_table(ran);
  const auto typo_ran = run_with({"trade", "plan", "005930", "--fields", "order.typo"}, table);
  CHECK(typo_ran.exit_ == 0);
  const auto typo = reply_of(typo_ran.out_);
  CHECK(typo.at("order").as_object().at("id").as_string() == "o1");
  CHECK(typo.at("warning").as_object().at("fields").as_array().size() == 1);
  const auto mixed = reply_of(run_with({"trade", "plan", "005930", "--fields", "order.id,order.typo"}, table).out_);
  CHECK(mixed.contains("warning"));
  const auto good = reply_of(run_with({"trade", "plan", "005930", "--fields", "order.id"}, table).out_);
  CHECK(!good.contains("warning"));
  CHECK(good.at("order").as_object().size() == 1);
  CHECK(*ran == 3);
  // An empty list is not a missing field.
  cmdatl::Reply empty;
  empty.body_["rows"] = boost::json::array{};
  std::ostringstream out;
  cmdatl::Rendering rendering;
  rendering.fields_ = {"rows.symbol"};
  cmdatl::write_reply(out, empty, rendering, "offline");
  CHECK(!reply_of(out.str()).contains("warning"));
}

TEST_CASE("the schema declares the errors its commands return") {
  const auto own = reply_of(run({"schema", "schema"}).out_);
  const auto& errors = own.at("commands").as_array()[0].as_object().at("errors").as_array();
  const auto returned = code_of(run({"schema", "nonexistent"}).out_);
  CHECK(std::find(errors.begin(), errors.end(), boost::json::value(returned)) != errors.end());
  const auto plan = reply_of(run({"schema", "order.plan"}).out_);
  const auto& plan_errors = plan.at("commands").as_array()[0].as_object().at("errors").as_array();
  const auto declares = [&plan_errors](const char* code) {
    return std::find(plan_errors.begin(), plan_errors.end(), boost::json::value(code)) != plan_errors.end();
  };
  CHECK(declares("unavailable.not_yet"));
  CHECK(declares("usage.missing_argument"));
  const auto backtest = reply_of(run({"schema", "strategy.backtest"}).out_);
  const auto& options = backtest.at("commands").as_array()[0].as_object().at("options").as_array();
  CHECK(options[0].as_object().at("short").as_string() == "-P");
  std::vector<cmdatl::Command> bad;
  bad.push_back(cmdatl::make_command({"x"}, "Read", "x").example("cmdatl x").returns("x", "x").error("bogus.code"));
  const auto problems = cmdatl::check_table(cmdatl::Table(std::move(bad)));
  const auto names_bogus = [](const std::string& problem) { return problem.find("bogus.code") != std::string::npos; };
  CHECK(std::any_of(problems.begin(), problems.end(), names_bogus));
}

TEST_CASE("an alias makes its old group a group") {
  const auto ran = std::make_shared<int>(0);
  const auto table = acting_table(ran);
  CHECK(table.is_group({"order"}));
  CHECK(run_with({"order", "--help"}, table).out_.rfind("cmdatl order COMMAND", 0) == 0);
}

TEST_CASE("a command streams events before its final reply") {
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"stream"}, "Read", "two events, then the end")
      .example("cmdatl stream")
      .returns("events", "how many")
      .runs([](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output& output) {
        output.emit(boost::json::object{{"event", "start"}});
        output.emit(boost::json::object{{"event", "heartbeat"}});
        cmdatl::Reply reply;
        reply.body_["events"] = 2;
        return reply;
      })
  );
  const auto out = run_with({"stream"}, cmdatl::Table(std::move(commands))).out_;
  std::istringstream lines(out);
  std::string line;
  std::vector<boost::json::object> objects;
  while (std::getline(lines, line)) {
    objects.push_back(reply_of(line));
  }
  REQUIRE(objects.size() == 3);
  CHECK(objects[0].at("event").as_string() == "start");
  CHECK(objects[1].at("event").as_string() == "heartbeat");
  CHECK(objects[2].at("events").as_int64() == 2);
  CHECK(objects[0].at("env").as_string() == "offline");
}

TEST_CASE("versions are read without overflow, and help and version need no command") {
  CHECK(!cmdatl::release_of("99999999999.0.0"));
  CHECK(cmdatl::at_least("0.1.0-9000", "0.0.0"));
  CHECK(!cmdatl::at_least("not a version", "0.0.0"));
  const cmdatl::Table table(cmdatl::command_list());
  CHECK(cmdatl::prepare({"--help"}, table, false).action_ == cmdatl::Prepared::Action::HELP);
  CHECK(cmdatl::prepare({"--version"}, table, false).action_ == cmdatl::Prepared::Action::VERSION);
}

#if !defined(_WIN32)
TEST_CASE("a bare CMDATL_ATLC is looked up on PATH only, never in the current directory") {
  const auto dir = std::filesystem::temp_directory_path() / ("cmdatl-atlc-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  {
    std::ofstream script(dir / "local-atlc");
    script << "#!/bin/sh\necho 'atlc 0.9.9'\n";
  }
  std::filesystem::permissions(dir / "local-atlc", std::filesystem::perms::owner_all);
  const auto here = std::filesystem::current_path();
  const auto* path = std::getenv("PATH");
  const std::string saved_path = path == nullptr ? "" : path;
  std::filesystem::current_path(dir);
  ::setenv("PATH", "/usr/bin:/bin", 1);
  ::setenv("CMDATL_ATLC", "local-atlc", 1);
  const auto bare = cmdatl::find_atlc();
  ::setenv("CMDATL_ATLC", "./local-atlc", 1);
  const auto explicit_path = cmdatl::find_atlc();
  ::unsetenv("CMDATL_ATLC");
  ::setenv("PATH", saved_path.c_str(), 1);
  std::filesystem::current_path(here);
  std::filesystem::remove_all(dir);
  CHECK(bare.path_.empty());
  CHECK(bare.requested_ == "local-atlc");
  CHECK(bare.version_.empty());
  CHECK(explicit_path.path_ == "./local-atlc");
  CHECK(explicit_path.version_ == "0.9.9");
}
#endif

TEST_CASE("the documentation scanner reads what a shell would, and names only what exists") {
  const cmdatl::Table table(cmdatl::command_list());
  const auto quoted = shell_words(R"(order release 005930 --reason "operator checked holdings" # why)");
  REQUIRE(quoted);
  CHECK(quoted->size() == 5);
  CHECK(!shell_words(R"(order release 005930 --reason "unfinished)"));
  CHECK(!accepts_invocation(R"(cmdatl order release 005930 --reason "unfinished)", table));
  CHECK(accepts_invocation(R"(cmdatl order release 005930 --reason "operator checked holdings")", table));
  CHECK(accepts_invocation("cmdatl --help", table));
  CHECK(accepts_invocation("cmdatl --version", table));
  CHECK(!accepts_invocation("cmdatl env --fields nonexistent", table));
  CHECK(!accepts_invocation("cmdatl run start spec.toml --bogus", table));
  CHECK(names_command("cmdatl order plan", table));
  CHECK(names_command("cmdatl order", table));
  CHECK(names_command("cmdatl quote 005930 000660", table));
  CHECK(!names_command("cmdatl clock2", table));
  CHECK(!names_command("cmdatl clock.nonexistent-subcommand", table));
  CHECK(!names_command("cmdatl clock nonexistent-subcommand", table));
  // Through extraction, as the documentation test reads a Markdown line.
  for (const auto* line : {"Use `cmdatl clock2`.", "Use `cmdatl clock.nonexistent-subcommand`."}) {
    const auto references = inline_references(line);
    REQUIRE(references.size() == 1);
    CHECK(!names_command(references.front(), table));
  }
  CHECK(inline_references("Run `cmdatl env` and `cmdatl schema order`.").size() == 2);
}

TEST_CASE("a failure is written whole, whatever was selected") {
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"send"}, "Trade", "an order whose outcome is unknown")
      .example("cmdatl send")
      .returns("order", "the order")
      .exits(cmdatl::SEND_EXITS)
      .sends()
      .runs([](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output&) {
        auto reply = cmdatl::error_reply(cmdatl::Exit::UNKNOWN, "unknown.no_reply", "no reply", "reconcile");
        reply.body_["order"] = boost::json::object{{"id", "o9"}, {"symbol", "005930"}};
        return reply;
      })
  );
  const cmdatl::Table table(std::move(commands));
  const std::vector<std::vector<std::string>> selectors{{"--fields", "order.id"}, {"--value", "order.id"}};
  for (const auto& selector : selectors) {
    auto args = std::vector<std::string>{"send"};
    args.insert(args.end(), selector.begin(), selector.end());
    const auto ran = run_with(args, table);
    CHECK(ran.exit_ == 4);
    const auto reply = reply_of(ran.out_);
    CHECK(reply.at("error").as_object().at("code").as_string() == "unknown.no_reply");
    CHECK(reply.at("order").as_object().at("symbol").as_string() == "005930");
  }
}

TEST_CASE("cmdatl help is --help as a command, and help leads with examples") {
  const auto top = run({"help"}, true);
  CHECK(top.exit_ == 0);
  CHECK(top.out_ == run({"--help"}, true).out_);
  CHECK(top.out_.find("Examples") != std::string::npos);
  CHECK(top.out_.find(cmdatl::HOME_PAGE) != std::string::npos);
  const auto plan = run({"help", "order", "plan"}, true);
  CHECK(plan.out_ == run({"order", "plan", "--help"}, true).out_);
  CHECK(plan.out_.find("Examples:\n  cmdatl order plan 005930") != std::string::npos);
  const auto json = reply_of(run({"help", "order"}).out_);
  CHECK(json.at("text").as_string().find("order plan SYMBOL") != std::string::npos);
  const auto unknown = run({"help", "ordr"});
  CHECK(unknown.exit_ == 2);
  CHECK(code_of(unknown.out_) == "usage.unknown_command");
}

TEST_CASE("every example in the table parses as cmdatl would read it") {
  const cmdatl::Table table(cmdatl::command_list());
  std::size_t checked = 0;
  for (const auto& command : table.commands()) {
    for (const auto& example : command.examples_) {
      INFO(example);
      CHECK(accepts_invocation(example, table));
      const auto words = shell_words(example.substr(7));
      REQUIRE(words);
      CHECK(cmdatl::prepare(*words, table, false).found_.command_ == &command);
      ++checked;
    }
  }
  for (const auto& example : cmdatl::TOP_EXAMPLES) {
    INFO(example);
    CHECK(accepts_invocation(example, table));
    ++checked;
  }
  CHECK(checked > table.commands().size());
}

TEST_CASE("a usage error names the command's usage, and a text failure goes to stderr") {
  const auto missing = run({"order", "plan", "005930", "--target", "10"});
  CHECK(reply_of(missing.out_).at("usage").as_string() == "cmdatl order plan SYMBOL --target N --limit P");
  CHECK(missing.err_.empty());
  const auto text = run({"order", "plan", "005930", "--target", "10"}, true);
  CHECK(text.out_.empty());
  CHECK(text.err_.find("usage: cmdatl order plan SYMBOL --target N --limit P") != std::string::npos);
  const auto field = reply_of(run({"env", "--value", "nope"}).out_);
  CHECK(field.at("usage").as_string() == "cmdatl env");
}

TEST_CASE("an example belongs to its own command, and the schema keeps the examples") {
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"clock"}, "Paper", "time").example("cmdatl clock advance --minutes 30").returns("now", "t")
  );
  commands.push_back(
    cmdatl::make_command({"clock", "advance"}, "Paper", "move").example("cmdatl clock advance").returns("now", "t")
  );
  const auto problems = cmdatl::check_table(cmdatl::Table(std::move(commands)));
  const auto about_clock = [](const std::string& problem) { return problem.rfind("clock: the example", 0) == 0; };
  CHECK(std::count_if(problems.begin(), problems.end(), about_clock) == 1);
  const auto env = reply_of(run({"schema", "env"}).out_);
  CHECK(!env.at("commands").as_array()[0].as_object().at("examples").as_array().empty());
}

TEST_CASE("a document never hides the fields beside it") {
  cmdatl::Reply reply;
  reply.body_["result"] = "done";
  reply.body_["order"] = "o1";
  reply.text_ = "Operation completed\n";
  std::ostringstream out;
  std::ostringstream err;
  cmdatl::Rendering rendering;
  rendering.format_ = cmdatl::Format::TEXT;
  cmdatl::Output output(out, err, rendering, "paper");
  CHECK(output.finish(reply) == cmdatl::Exit::OK);
  CHECK(out.str().rfind("Operation completed\n", 0) == 0);
  CHECK(out.str().find("order: o1") != std::string::npos);
  CHECK(out.str().find("result: done") != std::string::npos);
  CHECK(run({"help"}, true).out_ == run({"--help"}, true).out_);
}

TEST_CASE("--help wins over a malformed option, as clig.dev asks") {
  const auto help = run({"env", "--format", "yaml", "--help"}, true);
  CHECK(help.exit_ == 0);
  CHECK(help.out_.rfind("cmdatl env", 0) == 0);
  CHECK(run({"env", "--fields", "--help"}, true).exit_ == 0);
  CHECK(run({"env", "--format", "yaml"}).exit_ == 2);
}

TEST_CASE("an exception in a command becomes a reply: unknown if it may have sent") {
  std::vector<cmdatl::Command> commands;
  const auto thrower = [](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output&) -> cmdatl::Reply {
    throw std::runtime_error("boom");
  };
  commands.push_back(
    cmdatl::make_command({"read"}, "Read", "reads")
      .example("cmdatl read")
      .returns("x", "x")
      .exits(cmdatl::READ_EXITS)
      .runs(thrower)
  );
  commands.push_back(
    cmdatl::make_command({"send"}, "Trade", "sends")
      .example("cmdatl send")
      .returns("x", "x")
      .exits(cmdatl::SEND_EXITS)
      .sends()
      .runs(thrower)
  );
  const cmdatl::Table table(std::move(commands));
  const auto read = run_with({"read"}, table);
  CHECK(read.exit_ == 1);
  CHECK(code_of(read.out_) == "internal.error");
  const auto send = run_with({"send"}, table);
  CHECK(send.exit_ == 4);
  CHECK(code_of(send.out_) == "unknown.internal");
  CHECK(cmdatl::check_table(table).empty());
}

namespace {

/** @brief A stream buffer that refuses every write, as a closed pipe or a full disk would. */
class RefusingBuffer : public std::streambuf {
protected:
  /**
   * @brief Refuses a character.
   * @return End of file.
   */
  int_type overflow(int_type) override { return traits_type::eof(); }
};

/** @brief A stream buffer that takes every write but fails when flushed, as a buffered pipe that broke would. */
class FailingFlushBuffer : public std::stringbuf {
protected:
  /**
   * @brief Fails the flush.
   * @return -1.
   */
  int sync() override { return -1; }
};

/**
 * @brief A table of one sending command that places an order successfully, and one reader.
 * @param ran Counts the runs.
 * @return The table.
 */
cmdatl::Table placing_table(const std::shared_ptr<int>& ran) {
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"place"}, "Trade", "places an order")
      .example("cmdatl place")
      .returns("order", "the order")
      .exits(cmdatl::SEND_EXITS)
      .sends()
      .runs([ran](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output&) {
        ++*ran;
        cmdatl::Reply reply;
        reply.body_["result"] = "placed";
        reply.body_["order"] = "o7";
        return reply;
      })
  );
  commands.push_back(
    cmdatl::make_command({"read"}, "Read", "reads")
      .example("cmdatl read")
      .returns("x", "x")
      .exits(cmdatl::READ_EXITS)
      .runs([](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output&) {
        cmdatl::Reply reply;
        reply.body_["x"] = 1;
        return reply;
      })
  );
  return cmdatl::Table(std::move(commands));
}

/**
 * @brief A table of one sending command that returns an unknown outcome and can arm an allocation failure first.
 * @param ran Counts the runs.
 * @param fail_after_return Arms a failure of the next allocation after the handler returns, when true.
 * @return The table.
 */
cmdatl::Table outcome_table(const std::shared_ptr<int>& ran, bool fail_after_return) {
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"send"}, "Trade", "sends, outcome unknown")
      .alias({"old", "send"})
      .example("cmdatl send")
      .returns("order", "the order")
      .exits(cmdatl::SEND_EXITS)
      .sends()
      .runs([ran, fail_after_return](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output&) {
        ++*ran;
        auto reply = cmdatl::error_reply(cmdatl::Exit::UNKNOWN, "unknown.no_reply", "no reply", "reconcile");
        reply.body_["order"] = "o1";
        if (fail_after_return) {
          fail_allocation(1);
        }
        return reply;
      })
  );
  return cmdatl::Table(std::move(commands));
}

}  // namespace

TEST_CASE("after a sending command ran, a failure to report keeps exit 4") {
  const auto ran = std::make_shared<int>(0);
  for (const auto& args : std::vector<std::vector<std::string>>{{"send"}, {"old", "send"}}) {
    const auto table = outcome_table(ran, true);
    std::ostringstream out;
    std::ostringstream err;
    const auto exit = cmdatl::run(args, out, err, false, table);
    fail_allocation(0);
    CHECK(exit == 4);
  }
  CHECK(*ran == 2);
  const auto table = outcome_table(ran, false);
  RefusingBuffer refusing;
  std::ostream broken(&refusing);
  broken.exceptions(std::ios::badbit);
  std::ostringstream err;
  CHECK(cmdatl::run({"send"}, broken, err, false, table) == 4);
  CHECK(err.str().find("outcome is unknown") != std::string::npos);
  CHECK(*ran == 3);
}

TEST_CASE("a successful send whose reply is lost silently, or at the flush, still exits 4") {
  const auto ran = std::make_shared<int>(0);
  const auto table = placing_table(ran);
  for (const auto* format : {"json", "jsonl", "text"}) {
    INFO(format);
    RefusingBuffer refusing;
    std::ostream refused(&refusing);
    std::ostringstream err;
    CHECK(cmdatl::run({"--format", format, "place"}, refused, err, false, table) == 4);
    CHECK(err.str().find("outcome is unknown") != std::string::npos);
    FailingFlushBuffer flushing;
    std::ostream deferred(&flushing);
    std::ostringstream err2;
    CHECK(cmdatl::run({"--format", format, "place"}, deferred, err2, false, table) == 4);
    CHECK(err2.str().find("outcome is unknown") != std::string::npos);
    RefusingBuffer refusing_read;
    std::ostream read_out(&refusing_read);
    std::ostringstream err3;
    CHECK(cmdatl::run({"--format", format, "read"}, read_out, err3, false, table) == 1);
  }
  CHECK(*ran == 6);
  RefusingBuffer refusing;
  std::ostream refused(&refusing);
  std::ostringstream err;
  CHECK(cmdatl::run({"--help"}, refused, err, false, table) == 1);
  CHECK(cmdatl::run({"--version"}, refused, err, false, table) == 1);
}
