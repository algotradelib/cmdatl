#include <cmdatl/cli.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

/** @brief What one command line wrote and returned. */
struct Ran {
  /** @brief The exit code. */
  int exit_;
  /** @brief What it wrote. */
  std::string out_;
};

/**
 * @brief Runs a command line in process against cmdatl's commands.
 * @param args The command line, without the program's name.
 * @param terminal Whether to behave as if writing to a terminal.
 * @return What it wrote and returned.
 */
Ran run(const std::vector<std::string>& args, bool terminal = false) {
  std::ostringstream out;
  const auto exit = cmdatl::run(args, out, terminal);
  return {exit, out.str()};
}

/**
 * @brief Runs a command line in process against a given table.
 * @param args The command line, without the program's name.
 * @param table The commands.
 * @return What it wrote and returned.
 */
Ran run_with(const std::vector<std::string>& args, const cmdatl::Table& table) {
  std::ostringstream out;
  const auto exit = cmdatl::run(args, out, false, table);
  return {exit, out.str()};
}

/**
 * @brief A JSON reply's object.
 * @param text The reply.
 * @return The object.
 */
boost::json::object reply_of(const std::string& text) {
  return boost::json::parse(text).as_object();
}

/**
 * @brief The error code of a JSON reply.
 * @param text The reply.
 * @return The code.
 */
std::string code_of(const std::string& text) {
  return std::string(reply_of(text).at("error").as_object().at("code").as_string());
}

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
 * @brief Splits a shell-like line into words without running anything: double and single quotes keep spaces,
 *   and `#` after a space, outside quotes, starts a comment.
 * @param text The line.
 * @return The words.
 */
std::vector<std::string> shell_words(const std::string& text) {
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
  if (in_word) {
    words.push_back(word);
  }
  return words;
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
  return cmdatl::prepare(args, table, false).action_ != cmdatl::Prepared::Action::REFUSE;
}

/**
 * @brief Whether an inline code span such as `cmdatl order plan` names a group exactly, or a command with no more
 *   words than its arguments take (an inline reference may leave out required arguments).
 * @param span The span's text, without the backticks.
 * @param table The commands.
 * @return Whether it names something cmdatl has.
 */
bool names_command(const std::string& span, const cmdatl::Table& table) {
  auto words = shell_words(span);
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
  CHECK(run({"strategy", "check", "a.atls", "b.atls"}).exit_ == 2);
  // A repeated option takes one value per occurrence.
  CHECK(run({"strategy", "backtest", "s.toml", "-P", "a=1", "stray"}).exit_ == 2);
  CHECK(run({"strategy", "backtest", "s.toml", "-P", "a=1", "-P", "b=2"}).exit_ == 1);
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
  const auto error = run({"--format", "text", "env", "--bad"}).out_;
  CHECK(error.find("error: ") == 0);
  CHECK(error.find("env: offline") != std::string::npos);
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
  bad.push_back(cmdatl::make_command({"x"}, "Read", "x").returns("x", "x").error("bogus.code"));
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
  CHECK(shell_words(R"(order release 005930 --reason "operator checked holdings" # why)").size() == 5);
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
