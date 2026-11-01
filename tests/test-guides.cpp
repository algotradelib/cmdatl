// The guides and skills: `cmdatl guide`, the skills' files and the plugin's manifests, the words they share with the
// help and the README, and the agent guide's session run as written on a paper account.

#include "./order-support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace support;

namespace {

/** @brief The repository. */
const std::filesystem::path ROOT(CMDATL_SOURCE_DIR);

/** @brief The guides, by the command's last word. */
const std::vector<std::string> GUIDES{"agent", "strategy"};

/**
 * @brief A skill's file.
 * @param name `agent` or `strategy`.
 * @return Its text.
 */
std::string skill(const std::string& name) {
  return slurp(ROOT / "skills" / ("cmdatl-" + name) / "SKILL.md");
}

/**
 * @brief The frontmatter's keys and values, in order: the `key: value` lines between the opening and closing `---`.
 * @param file The file.
 * @return The pairs; empty when the file opens with no frontmatter.
 */
std::vector<std::pair<std::string, std::string>> frontmatter(const std::string& file) {
  std::vector<std::pair<std::string, std::string>> out;
  const auto lines = lines_of(file);
  if (lines.empty() || lines.front() != "---") {
    return out;
  }
  for (std::size_t i = 1; i < lines.size() && lines[i] != "---"; ++i) {
    const auto colon = lines[i].find(": ");
    out.emplace_back(lines[i].substr(0, colon), colon == std::string::npos ? "" : lines[i].substr(colon + 2));
  }
  return out;
}

/**
 * @brief The text cmdatl writes for a command line, run in process.
 * @param args The command line.
 * @return stdout.
 */
std::string text_of(const std::vector<std::string>& args) {
  return run(args).out_;
}

/** @brief A stream buffer that refuses every write, as a closed pipe would. */
class RefusingBuffer : public std::streambuf {
protected:
  /**
   * @brief Refuses a character.
   * @return End of file.
   */
  int_type overflow(int_type) override {
    return traits_type::eof();
  }
};

/**
 * @brief Whether a reply's field is a JSON string.
 * @param object The object.
 * @param key The field.
 * @return Whether it is present and a string.
 */
bool is_text(const boost::json::object& object, const std::string& key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string();
}

} // namespace

TEST_CASE("guide agent and guide strategy print their skill's body in every format") {
  for (const auto& name : GUIDES) {
    INFO(name);
    const auto body = cmdatl::guide_body(skill(name));
    REQUIRE(!body.empty());
    CHECK(body.rfind("# ", 0) == 0);
    CHECK(text_of({"--format", "text", "guide", name}) == body);
    // --value prints the string bare and ends the line, as for every value.
    CHECK(text_of({"--value", "text", "guide", name}) == body + "\n");
    const auto json = reply_of(text_of({"--json", "guide", name}));
    CHECK(json.at("text").as_string() == body);
    CHECK(json.at("env").as_string() == "offline");
    CHECK(body.find("name: cmdatl-") == std::string::npos);
  }
  CHECK(cmdatl::guide_body("# no frontmatter\n") == "# no frontmatter\n");
  CHECK(cmdatl::guide_body("---\nname: x\nno closing line\n") == "---\nname: x\nno closing line\n");
  CHECK(cmdatl::guide_body("---\nname: x\n---\n\n\n# Body\n") == "# Body\n");
  CHECK(cmdatl::guide_body("---\n---\n").empty());
  CHECK(cmdatl::guide_reply("nonexistent").body_.at("error").as_object().at("code").as_string() == "internal.error");
}

TEST_CASE("cmdatl guide names its two guides and a mistyped one gets the closest") {
  const auto typo = run({"--json", "guide", "agnet"});
  CHECK(typo.exit_ == 2);
  CHECK(code_of(typo.out_) == "usage.unknown_command");
  CHECK(reply_of(typo.out_).at("error").as_object().at("next").as_string().find("guide agent") != std::string::npos);
  const auto group = run({"--json", "guide"});
  CHECK(group.exit_ == 2);
  const auto help = text_of({"help", "guide"});
  CHECK(help.find("guide agent") != std::string::npos);
  CHECK(help.find("guide strategy") != std::string::npos);
  CHECK(text_of({"--help"}).find("cmdatl guide agent or strategy") != std::string::npos);
}

TEST_CASE("the exit words are the same in the help and the README and the agent guide") {
  const auto help = text_of({"--help"});
  const auto readme = slurp(ROOT / "README.md");
  const auto agent = skill("agent");
  for (const auto* words : {cmdatl::EXIT_REFUSED_TEXT, cmdatl::EXIT_UNKNOWN_TEXT}) {
    INFO(words);
    CHECK(help.find(words) != std::string::npos);
    CHECK(readme.find(words) != std::string::npos);
    CHECK(agent.find(words) != std::string::npos);
  }
  for (const auto& text : {help, readme, agent}) {
    CHECK(text.find("safe to retry") == std::string::npos);
  }
}

TEST_CASE("each skill's frontmatter is the open format's and its body is short and plain ASCII") {
  const std::regex name_rule("[a-z0-9]+(-[a-z0-9]+)*");
  for (const auto& name : GUIDES) {
    INFO(name);
    const auto file = skill(name);
    const auto front = frontmatter(file);
    REQUIRE(front.size() == 2);
    CHECK(front[0].first == "name");
    CHECK(front[0].second == "cmdatl-" + name);
    CHECK(front[0].second.size() <= 64);
    CHECK(std::regex_match(front[0].second, name_rule));
    CHECK(front[1].first == "description");
    CHECK(!front[1].second.empty());
    CHECK(front[1].second.size() <= 1024);
    const auto body = cmdatl::guide_body(file);
    CHECK(body.size() <= 9000);
    bool ascii = true;
    for (const unsigned char c : file) {
      ascii = ascii && c < 0x80;
    }
    CHECK(ascii);
  }
  // No other skill is shipped unchecked.
  std::size_t folders = 0;
  for (const auto& entry : std::filesystem::directory_iterator(ROOT / "skills")) {
    CHECK(std::filesystem::exists(entry.path() / "SKILL.md"));
    ++folders;
  }
  CHECK(folders == GUIDES.size());
}

TEST_CASE("the plugin's manifests name cmdatl and its root holds nothing a plugin would load by itself") {
  const auto plugin = boost::json::parse(slurp(ROOT / ".claude-plugin" / "plugin.json")).as_object();
  CHECK(plugin.at("name").as_string() == "cmdatl");
  if (const auto* version = plugin.if_contains("version")) {
    CHECK(std::string(CMDATL_VERSION).rfind(std::string(version->as_string()).substr(0, 5), 0) == 0);
  }
  const auto market = boost::json::parse(slurp(ROOT / ".claude-plugin" / "marketplace.json")).as_object();
  CHECK(market.at("name").as_string() == "cmdatl");
  CHECK(!market.at("owner").as_object().at("name").as_string().empty());
  const auto& plugins = market.at("plugins").as_array();
  REQUIRE(plugins.size() == 1);
  CHECK(plugins[0].as_object().at("name").as_string() == "cmdatl");
  CHECK(plugins[0].as_object().at("source").as_string() == "./");
  for (
    const auto* path : {"bin", "commands", "agents", "hooks", ".mcp.json", ".lsp.json", "output-styles", "monitors"}
  ) {
    INFO(path);
    CHECK(!std::filesystem::exists(ROOT / path));
  }
}

TEST_CASE("no skill names a command that is not built and every error code the docs name exists") {
  const cmdatl::Table table(cmdatl::command_list());
  std::set<std::string> codes;
  std::set<std::string> families;
  for (const auto& entry : cmdatl::error_catalog()) {
    codes.insert(entry.code_);
    const auto dot = entry.code_.find('.');
    if (dot != std::string::npos) {
      families.insert(entry.code_.substr(0, dot));
    }
  }
  // Families whose names are also file stems in the docs (paper.toml, run.json): not read as codes.
  families.erase("paper");
  families.erase("file");
  families.erase("run");
  const std::set<std::string> extensions{
    "toml", "atls", "csv", "md", "json", "jsonl", "armed", "release", "stop", "lock"
  };
  const std::regex token("`([a-z_]+)\\.([a-z_]+)`");
  const std::regex reference("`cmdatl ([a-z][a-z ]*[a-z])");
  std::vector<std::filesystem::path> files{ROOT / "README.md"};
  for (const auto& name : GUIDES) {
    files.push_back(ROOT / "skills" / ("cmdatl-" + name) / "SKILL.md");
  }
  std::size_t checked = 0;
  for (const auto& file : files) {
    const auto text = slurp(file);
    for (std::sregex_iterator it(text.begin(), text.end(), token), end; it != end; ++it) {
      const auto family = (*it)[1].str();
      if (families.count(family) == 0 || extensions.count((*it)[2].str()) != 0) {
        continue;
      }
      INFO(file.filename().string() << ": " << (*it)[0].str());
      CHECK(codes.count(family + "." + (*it)[2].str()) == 1);
      ++checked;
    }
    if (file.filename() != "SKILL.md") {
      continue;
    }
    for (const auto& line : lines_of(text)) {
      const auto first = line.find_first_not_of(' ');
      if (first == std::string::npos || line.compare(first, 7, "cmdatl ") != 0) {
        continue;
      }
      auto words = shell_words(line.substr(first));
      if (!words) {
        continue;
      }
      words->erase(words->begin());
      const auto found = table.resolve(*words);
      if (found.command_ != nullptr) {
        INFO(line);
        CHECK(found.command_->available_);
      }
    }
    for (std::sregex_iterator it(text.begin(), text.end(), reference), end; it != end; ++it) {
      const auto words = shell_words((*it)[1].str());
      if (!words) {
        continue;
      }
      const auto found = table.resolve(*words);
      if (found.command_ != nullptr) {
        INFO(file.parent_path().filename().string() << ": " << (*it)[0].str());
        CHECK(found.command_->available_);
      }
    }
  }
  CHECK(checked > 20);
}

TEST_CASE("the strategy guide states the README's parity rule word for word") {
  const auto readme = slurp(ROOT / "README.md");
  std::size_t found = 0;
  for (const auto& line : lines_of(skill("strategy"))) {
    if (line.rfind("A run's fills are those of", 0) == 0 || line.rfind("The run starts at the first bar", 0) == 0) {
      INFO(line);
      CHECK(readme.find(line) != std::string::npos);
      ++found;
    }
  }
  CHECK(found == 2);
}

TEST_CASE("every next cmdatl gives leaves the operator's decisions with the operator") {
  // Words that ask the reader to act outside cmdatl's commands, or to start what the operator may have stopped.
  const std::vector<std::string> outside{
    "aside", "remov", "creat", "delete", "rename", "edit", "restore", "write", "smaller", "start another run",
    "CMDATL_CONFIG", "XDG_STATE_HOME", " HOME", "order release"
  };
  // Phrases that hand such an act to the operator; a bare mention ("the operator's file") does not.
  const std::vector<std::string> handed{
    "to the operator", "ask the operator", "the operator, who", "only the operator", "the operator can",
    "the operator's to allow", "the operator sets", "the operator fixes", "the operator writes",
    "the operator withdraws"
  };
  std::vector<std::pair<std::string, std::string>> texts;
  for (const auto& entry : cmdatl::error_catalog()) {
    texts.emplace_back(entry.code_, entry.next_);
  }
  texts.emplace_back("the run's own reply", cmdatl::run_unknown_next());
  std::vector<cmdatl::Reply> replies{
    cmdatl::paper_state_reply("damaged"), cmdatl::journal_missing_reply("missing"), cmdatl::lock_lost_reply()
  };
  texts.emplace_back("owed row: operator", cmdatl::owed_next("005930", "operator", std::nullopt));
  texts.emplace_back("owed row: unnamed", cmdatl::owed_next("005930", "unnamed", std::nullopt));
  for (const auto* code : {"config.missing", "config.invalid", "unsupported.broker"}) {
    cmdatl::ConfigSnapshot snapshot;
    snapshot.error_.code_ = code;
    replies.push_back(cmdatl::config_reply(snapshot));
  }
  for (const auto& reply : replies) {
    const auto& error = reply.body_.at("error").as_object();
    texts.emplace_back(std::string(error.at("code").as_string()), std::string(error.at("next").as_string()));
  }
  for (const auto& [code, next] : texts) {
    INFO(code << ": " << next);
    CHECK(next.find("start a new run") == std::string::npos);
    for (const auto& word : outside) {
      if (next.find(word) != std::string::npos) {
        bool hands = false;
        for (const auto& phrase : handed) {
          hands = hands || next.find(phrase) != std::string::npos;
        }
        CHECK(hands);
      }
    }
  }
  // The codes whose next carries a rule the agent guide relies on keep it, word for word.
  const std::vector<std::pair<std::string, std::string>> pinned{
    {"unknown.journal_missing", "stop and report to the operator"},
    {"unknown.journal_damaged", "stop and report to the operator"},
    {"unknown.paper_state", "stop and report to the operator"},
    {"unknown.lock_lost", "stop and report to the operator"},
    {"refused.phase", "names nothing but phase or market_closed"},
    {"refused.awaiting_confirmation", "place nothing"},
    {"unknown.broker_refused", "do not place again"},
    {"unknown.unaccounted", "report to the operator"},
    {"refused.not_armed", "ask the operator"},
    {"refused.daily_order_limit", "ask the operator"},
    {"refused.order_value", "ask the operator"},
  };
  for (const auto& [code, words] : pinned) {
    INFO(code);
    CHECK(cmdatl::catalog_next(code).find(words) != std::string::npos);
  }
  // The caps only the operator lifts never invite a retry.
  const std::vector<std::string> caps{
    "refused.not_armed", "refused.daily_order_limit", "refused.order_value", "refused.position_limit",
    "refused.symbol_not_allowed", "refused.guard"
  };
  for (const auto& entry : cmdatl::error_catalog()) {
    for (const auto& cap : caps) {
      if (entry.code_ == cap) {
        INFO(cap);
        CHECK(!entry.retry_);
      }
    }
  }
}

TEST_CASE("a run command whose reply is lost names cmdatl run status and not order reconcile") {
  const PaperFixture fixture;
  const auto ran = std::make_shared<int>(0);
  std::vector<cmdatl::Command> commands;
  commands.push_back(
    cmdatl::make_command({"run", "go"}, "Run", "a stand-in run that starts")
      .example("cmdatl run go")
      .returns("run", "the run id")
      .exits(cmdatl::SEND_EXITS)
      .sends()
      .on_config([ran](const cmdatl::Invocation&, const cmdatl::ConfigSnapshot&, cmdatl::Output&) {
        ++*ran;
        cmdatl::Reply reply;
        reply.body_["run"] = "r1";
        return reply;
      })
  );
  const cmdatl::Table table(std::move(commands));
  RefusingBuffer refusing;
  std::ostream refused(&refusing);
  std::ostringstream err;
  CHECK(cmdatl::run({"--json", "run", "go"}, refused, err, false, table) == 4);
  CHECK(*ran == 1);
  CHECK(err.str().find("run cmdatl run status before anything else") != std::string::npos);
  CHECK(err.str().find("order reconcile") == std::string::npos);
}

TEST_CASE("the agent guide's session runs as written and each reply holds what the guide says") {
  const auto session = block_after(skill("agent"), "## A session", "```sh");
  // The walkthrough this case checks, line for line: a changed walkthrough changes this case in the same commit.
  const std::vector<std::string> expected{
    "cmdatl status",
    "cmdatl env",
    "cmdatl clock advance --next-open",
    "cmdatl quote 005930",
    "cmdatl order plan 005930 --target 10 --limit 78500",
    "cmdatl order place PLAN_ID",
    "cmdatl clock advance --minutes 5",
    "cmdatl order reconcile 005930",
    "cmdatl account --fields cash,positions",
  };
  REQUIRE(session == expected);
  const PaperFixture fixture;
  fixture.arm();
  std::string plan;
  std::vector<Answer> answers;
  for (const auto& line : session) {
    auto words = *shell_words(line);
    words.erase(words.begin());
    for (auto& word : words) {
      if (word == "PLAN_ID") {
        word = plan;
      }
    }
    const auto journal_before = std::filesystem::exists(fixture.journal()) ? slurp(fixture.journal()) : "";
    const auto log_before = std::filesystem::exists(fixture.log()) ? slurp(fixture.log()) : "";
    auto answer = ask_checked(fixture, words);
    INFO(line << " -> " << boost::json::serialize(answer.body_));
    REQUIRE(answer.exit_ == 0);
    const auto& body = answer.body_;
    if (line == "cmdatl status") {
      const std::vector<std::string> keys{
        "now", "phase", "orders_accepted_now", "not_accepted", "positions", "cash", "owed", "orders", "quotes"
      };
      for (const auto& key : keys) {
        CHECK(body.contains(key));
      }
      CHECK(is_text(body, "cash"));
      // The account opens after the close: closed, and `not_accepted` names the phase, as the guide's closed-market line says.
      CHECK(!body.at("orders_accepted_now").as_bool());
      // The closed-market rule applies only when nothing but the phase and the closed market refuse.
      bool phase = false;
      for (const auto& gate : body.at("not_accepted").as_array()) {
        phase = phase || gate.as_string() == "phase";
        CHECK((gate.as_string() == "phase" || gate.as_string() == "market_closed"));
      }
      CHECK(phase);
    } else if (line == "cmdatl env") {
      const auto& limits = body.at("limits").as_object();
      for (const auto* key : {"symbols", "position", "order_value", "daily_orders", "collar", "phases", "plan_ttl"}) {
        CHECK(limits.contains(key));
      }
      CHECK(is_text(limits, "order_value"));
      CHECK(is_text(limits, "collar"));
    } else if (line == "cmdatl clock advance --next-open") {
      CHECK(body.at("phase").as_string() == "open");
    } else if (line == "cmdatl quote 005930") {
      const auto& quote = body.at("quotes").as_array().at(0).as_object();
      CHECK(is_text(quote, "last_price"));
      CHECK(quote.at("last_price").as_string() == "78500");
      CHECK(quote.at("orders_accepted_now").as_bool());
    } else if (line.rfind("cmdatl order plan", 0) == 0) {
      CHECK(answer.reason_ == "plan");
      CHECK(is_text(body, "quantity"));
      plan = std::string(body.at("plan_id").as_string());
      // A plan sends nothing: neither the journal nor the market's log gains a line.
      CHECK(slurp(fixture.journal()) == journal_before);
      CHECK(ops_of(slurp(fixture.log()), "place").size() == ops_of(log_before, "place").size());
    } else if (line.rfind("cmdatl order place", 0) == 0) {
      CHECK(answer.reason_ == "placed");
      CHECK(body.at("result").as_string() == "done");
      CHECK(ops_of(slurp(fixture.log()), "place").size() == ops_of(log_before, "place").size() + 1);
    } else if (line == "cmdatl clock advance --minutes 5") {
      const auto& events = body.at("events").as_array();
      REQUIRE(events.size() == 1);
      const auto& fill = events[0].as_object();
      CHECK(fill.at("kind").as_string() == "filled");
      for (const auto* key : {"quantity", "price", "commission", "tax"}) {
        CHECK(is_text(fill, key));
      }
    } else if (line == "cmdatl order reconcile 005930") {
      CHECK(body.at("owed").as_array().empty());
      CHECK(body.at("checked").as_array().at(0).as_object().at("state").as_string() == "clean");
    } else if (line == "cmdatl account --fields cash,positions") {
      const auto& position = body.at("positions").as_array().at(0).as_object();
      CHECK(position.at("quantity").as_string() == "10");
      const auto& fill = answers[6].body_.at("events").as_array().at(0).as_object();
      const long price = std::stol(std::string(fill.at("price").as_string()));
      const long commission = std::stol(std::string(fill.at("commission").as_string()));
      CHECK(std::stol(std::string(body.at("cash").as_string())) == 100000000 - 10 * price - commission);
    }
    answers.push_back(std::move(answer));
  }
}

TEST_CASE("the agent guide's order rules each on its own account") {
  const PaperFixture fixture;
  fixture.arm();
  REQUIRE(ask_checked(fixture, {"clock", "advance", "--next-open"}).exit_ == 0);

  SECTION("a target counts working orders: met while a buy rests, and to reprice, cancel and plan again") {
    const auto id = plan_id(fixture, "005930", "10", "78000");
    REQUIRE(ask_checked(fixture, {"order", "place", id}).reason_ == "placed");
    CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "78500"}).reason_ == "met");
    const auto cancel = ask_checked(fixture, {"order", "cancel", "005930"});
    CHECK(cancel.reason_ == "withdrawal_sent");
    CHECK(ask_checked(fixture, {"order", "list"}).body_.at("orders").as_array().empty());
    CHECK(ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "78500"}).reason_ == "plan");
  }
  SECTION("holding 10, --target 30 buys 20") {
    REQUIRE(ask_checked(fixture, {"order", "place", plan_id(fixture, "005930", "10", "78500")}).exit_ == 0);
    REQUIRE(ask_checked(fixture, {"clock", "advance", "--minutes", "5"}).exit_ == 0);
    REQUIRE(ask_checked(fixture, {"order", "reconcile", "005930"}).exit_ == 0);
    const auto more = ask_checked(fixture, {"order", "plan", "005930", "--target", "30", "--limit", "78500"});
    CHECK(more.body_.at("would").as_string() == "buy");
    CHECK(more.body_.at("quantity").as_string() == "20");
  }
  SECTION("a used plan is refused.plan_used, a lapsed one refused.stale_plan, both exit 3") {
    const auto id = plan_id(fixture, "005930", "10", "78000");
    REQUIRE(ask_checked(fixture, {"order", "place", id}).exit_ == 0);
    const auto again = ask_checked(fixture, {"order", "place", id});
    CHECK(again.exit_ == 3);
    CHECK(again.code_ == "refused.plan_used");
    const auto old = plan_id(fixture, "000660", "1", "181000");
    REQUIRE(ask_checked(fixture, {"clock", "advance", "--minutes", "2"}).exit_ == 0);
    const auto lapsed = ask_checked(fixture, {"order", "place", old});
    CHECK(lapsed.exit_ == 3);
    CHECK(lapsed.code_ == "refused.stale_plan");
  }
  SECTION("over the cap, outside the collar, or unarmed: exit 3 and nothing sent; a cancel needs no armed file") {
    const auto cap = ask_checked(fixture, {"order", "plan", "005930", "--target", "101", "--limit", "78500"});
    CHECK(cap.exit_ == 3);
    CHECK(cap.code_ == "refused.position_limit");
    const auto collar = ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "90000"});
    CHECK(collar.exit_ == 3);
    CHECK(collar.code_ == "refused.collar");
    REQUIRE(ask_checked(fixture, {"order", "place", plan_id(fixture, "005930", "10", "78000")}).exit_ == 0);
    const auto id = plan_id(fixture, "000660", "1", "181000");
    std::filesystem::remove(fixture.armed());
    const auto unarmed = ask_checked(fixture, {"order", "place", id});
    CHECK(unarmed.exit_ == 3);
    CHECK(unarmed.code_ == "refused.not_armed");
    CHECK(!unarmed.body_.at("error").as_object().at("retry").as_bool());
    CHECK(ask_checked(fixture, {"order", "cancel", "005930"}).reason_ == "withdrawal_sent");
  }
  SECTION("the daily count: the operator's cap, exit 3 once used up") {
    for (const auto* limit : {"78000", "77900", "77800"}) {
      const auto id = plan_id(fixture, "005930", "10", limit);
      REQUIRE(ask_checked(fixture, {"order", "place", id}).exit_ == 0);
      REQUIRE(ask_checked(fixture, {"order", "cancel", "005930"}).exit_ == 0);
    }
    const auto over = ask_checked(fixture, {"order", "plan", "005930", "--target", "10", "--limit", "78000"});
    CHECK(over.exit_ == 3);
    CHECK(over.code_ == "refused.daily_order_limit");
    CHECK(!over.body_.at("error").as_object().at("retry").as_bool());
  }
  SECTION("reconcile with an order still resting is refused.awaiting") {
    REQUIRE(ask_checked(fixture, {"order", "place", plan_id(fixture, "005930", "10", "77000")}).exit_ == 0);
    const auto awaiting = ask_checked(fixture, {"order", "reconcile", "005930"});
    CHECK(awaiting.exit_ == 3);
    CHECK(awaiting.code_ == "refused.awaiting");
  }
}
