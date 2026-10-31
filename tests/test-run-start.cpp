// `run start` and `run status`: the pure parts (grammars, the cache key, the build's file writes, the spec's fields,
// the run's state) in process, and the commands as processes against the real atlc and algotradelib (tagged
// `[acceptance][runs]`: they build a driver with CMake, and skip, reported, without CMDATL_TEST_ATLC and
// CMDATL_TEST_PREFIX).

#include "./paper-support.hpp"
#include "./support.hpp"

#include <cmdatl/cmdatl>

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <cstdlib>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace support;
namespace json = boost::json;
namespace runs = cmdatl::runs;
namespace build = cmdatl::build;

namespace {

/**
 * @brief Records as `run_state` reads them, built field by field.
 * @return Empty records.
 */
runs::Records records() {
  return runs::Records{};
}

/**
 * @brief A reply record with an exit, and an error code when given.
 * @param exit The exit.
 * @param code The code, or empty.
 * @param summary Whether it carries a run's summary.
 * @return The record.
 */
json::object reply_record(int exit, const std::string& code = "", bool summary = false) {
  json::object reply{{"run", "r000000000001"}, {"exit", exit}};
  if (!code.empty()) {
    reply["error"] = json::object{{"code", code}};
  }
  if (summary) {
    reply["outcome"] = "clean";
  }
  return reply;
}

}  // namespace

TEST_CASE("run grammars: run ids, argument keys, structs and offsets") {
  CHECK(cmdatl::valid_run_id("r0123456789ab"));
  CHECK(!cmdatl::valid_run_id("r0123456789aB"));
  CHECK(!cmdatl::valid_run_id("r0123456789a"));
  CHECK(!cmdatl::valid_run_id("x0123456789ab"));
  CHECK(runs::valid_key("param.lookback"));
  CHECK(runs::valid_key(std::string(128, 'a')));
  CHECK(!runs::valid_key(std::string(129, 'a')));
  CHECK(!runs::valid_key("a b"));
  CHECK(!runs::valid_key(""));
  CHECK(runs::valid_struct("MeanReversion"));
  CHECK(runs::valid_struct("M" + std::string(63, 'a')));
  CHECK(!runs::valid_struct("M" + std::string(64, 'a')));
  CHECK(!runs::valid_struct("Params"));
  CHECK(!runs::valid_struct("Lane"));
  CHECK(!runs::valid_struct("meanReversion"));
  CHECK(!runs::valid_struct("Mean_Reversion"));
  CHECK(runs::valid_offset("0.01"));
  CHECK(runs::valid_offset("0"));
  CHECK(!runs::valid_offset("1"));
  CHECK(!runs::valid_offset("1.5"));
  CHECK(!runs::valid_offset("-0.1"));
  CHECK(!runs::valid_offset("0.01x"));
}

TEST_CASE("the market settings a run refuses before anything is built") {
  CHECK(!cmdatl::spec_refusal({"replay.market=kr"}));
  CHECK(cmdatl::spec_refusal({})->first == "replay.market");
  CHECK(cmdatl::spec_refusal({"replay.market=us"})->first == "replay.market");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "data.store=kis/krx"})->first == "data.store");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "replay.shorts=true"})->first == "replay.shorts");
  CHECK(cmdatl::spec_refusal({"replay.market=kr", "replay.fill=legs"})->first == "replay.fill");
  // The last value wins, as atlc's settings read it.
  CHECK(!cmdatl::spec_refusal({"replay.market=us", "replay.market=kr"}));
}

TEST_CASE("a shape's key changes with each input it holds, and with nothing else") {
  std::map<std::string, std::string> env;
  const auto lookup = [&env](const std::string& name) -> std::optional<std::string> {
    const auto found = env.find(name);
    return found == env.end() ? std::nullopt : std::optional<std::string>(found->second);
  };
  const auto base = build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg");
  CHECK(base.rfind("A-", 0) == 0);
  CHECK(base.size() == 2 + 16);
  CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") == base);
  CHECK(build::shape_name("/p/b.atls", "A", "incremental", lookup, "/pkg") != base);
  CHECK(build::shape_name("/p/a.atls", "A", "exact", lookup, "/pkg") != base);
  CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/other") != base);
  for (const auto& name : build::KEY_VARIABLES) {
    env = {{name, "x"}};
    CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") != base);
    // Set and empty differ from unset.
    env = {{name, ""}};
    CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") != base);
  }
  env = {{"CMAKE_PREFIX_PATH", "/x"}, {"PATH", "/y"}};
  CHECK(build::shape_name("/p/a.atls", "A", "incremental", lookup, "/pkg") == base);
}

TEST_CASE("a build's children run without the locators the key does not hold") {
  const auto out = build::child_environment(
    {"PATH=/bin", "CMAKE_PREFIX_PATH=/x", "algotradelib_DIR=/y", "ALGOTRADELIB_ROOT=/z", "OPENSSL_ROOT_DIR=/o", "BOOST_ROOT=/b",
     "Foo_DIR=/f", "HOME=/h", "CXX=clang++"}
  );
  CHECK(out == std::vector<std::string>{"PATH=/bin", "OPENSSL_ROOT_DIR=/o", "BOOST_ROOT=/b", "HOME=/h", "CXX=clang++"});
}

TEST_CASE("the build writes a file only when its bytes change, and clears a killed build's temporaries") {
  const ScratchDir dir;
  const auto file = dir.path() / "src" / "deep" / "a.hpp";
  CHECK(build::write_if_changed(file, "one") == build::Written::REPLACED);
  struct stat before{};
  REQUIRE(::stat(file.c_str(), &before) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(build::write_if_changed(file, "one") == build::Written::UNCHANGED);
  struct stat same{};
  REQUIRE(::stat(file.c_str(), &same) == 0);
  CHECK(same.st_ino == before.st_ino);
  CHECK(same.st_mtime == before.st_mtime);
  CHECK(build::write_if_changed(file, "two") == build::Written::REPLACED);
  CHECK(slurp(file) == "two");
  spit(dir.path() / "src" / ".a.hpp.123.00ff.tmp", "half");
  spit(dir.path() / "src" / "keep.tmp", "not ours");
  build::remove_stale_temporaries(dir.path() / "src");
  CHECK(!std::filesystem::exists(dir.path() / "src" / ".a.hpp.123.00ff.tmp"));
  CHECK(std::filesystem::exists(dir.path() / "src" / "keep.tmp"));
  CHECK(build::files_under(dir.path() / "src") == std::set<std::string>{"deep/a.hpp", "keep.tmp"});
}

TEST_CASE("the driver's sources are carried inside cmdatl, and only the modules it may include") {
  std::set<std::string> paths;
  for (const auto& file : cmdatl::DRIVER_FILES) {
    paths.insert(file.path_);
    std::size_t size = 0;
    for (std::size_t k = 0; k < file.count_; ++k) {
      CHECK(file.sizes_[k] <= 16u * 1024u);
      size += file.sizes_[k];
    }
    CHECK((size > 0 || std::string(file.path_).find("LICENSE") != std::string::npos));
  }
  CHECK(paths.count("main.cpp") == 1);
  CHECK(paths.count("CMakeLists.txt") == 1);
  CHECK(paths.count("cmdatl/driver/run.hpp") == 1);
  CHECK(paths.count("third_party/toml++/toml.hpp") == 1);
  for (const auto& path : paths) {
    CHECK(path.find("cmdatl/cli/") == std::string::npos);
    CHECK(path.find("cmdatl/strategy/") == std::string::npos);
  }
  // A file's bytes come back whole.
  for (const auto& file : cmdatl::DRIVER_FILES) {
    if (std::string(file.path_) == "cmdatl/core/runs.hpp") {
      std::string text;
      for (std::size_t k = 0; k < file.count_; ++k) {
        text.append(file.chunks_[k], file.sizes_[k]);
      }
      CHECK(text == slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "include" / "cmdatl" / "core" / "runs.hpp"));
    }
  }
}

TEST_CASE("atlc spec's fields a run consumes are checked at and past each bound") {
  const ScratchDir dir;
  spit(dir.path() / "p.atls", "");
  const auto pairs = [](std::vector<std::pair<std::string, std::string>> given) {
    json::array out;
    for (auto& [key, value] : given) {
      json::array pair;
      pair.push_back(json::value(key));
      pair.push_back(json::value(value));
      out.push_back(std::move(pair));
    }
    return out;
  };
  const auto envelope = [&dir](const json::array& args) {
    return json::object{
      {"program", json::object{{"path", (dir.path() / "p.atls").string()}, {"struct", "P"}}},
      {"build", json::object{{"indicators", "incremental"}}},
      {"store", false},
      {"orders", json::object{{"type", "limit"}, {"offset", "0.01"}}},
      {"args", args},
    };
  };
  runs::SpecFields fields;
  CHECK(!runs::spec_fields(envelope(pairs({{"param.a", "1 = 2 é"}})), fields));
  CHECK(fields.args_ == std::vector<std::string>{"param.a=1 = 2 é"});
  json::array many;
  for (int k = 0; k < 1000; ++k) {
    json::array pair;
    pair.push_back(json::value("k" + std::to_string(k)));
    pair.push_back(json::value("v"));
    many.push_back(std::move(pair));
  }
  runs::SpecFields thousand;
  CHECK(!runs::spec_fields(envelope(many), thousand));
  {
    json::array pair;
    pair.push_back(json::value("k1000"));
    pair.push_back(json::value("v"));
    many.push_back(std::move(pair));
  }
  runs::SpecFields over;
  CHECK(runs::spec_fields(envelope(many), over)->exit_ == cmdatl::Exit::FAILED);
  const auto refused = [&envelope](json::object e) {
    runs::SpecFields f;
    return runs::spec_fields(e, f).has_value();
  };
  CHECK(refused(envelope(pairs({{"a", "1"}, {"a", "2"}}))));
  CHECK(refused(envelope(pairs({{std::string(129, 'k'), "1"}}))));
  CHECK(!refused(envelope(pairs({{std::string(128, 'k'), "1"}}))));
  CHECK(refused(envelope(pairs({{"a", std::string("x\0y", 3)}}))));
  CHECK(refused(envelope(pairs({{"a", std::string(4097, 'v')}}))));
  CHECK(!refused(envelope(pairs({{"a", std::string(4096, 'v')}}))));
  auto e = envelope(json::array{});
  e["program"].as_object()["struct"] = "Params";
  CHECK(refused(e));
  e = envelope(json::array{});
  e["program"].as_object()["path"] = "relative.atls";
  CHECK(refused(e));
  e = envelope(json::array{});
  e["build"].as_object()["indicators"] = "fast";
  CHECK(refused(e));
  e = envelope(json::array{});
  e["orders"].as_object()["offset"] = "2";
  CHECK(refused(e));
  e = envelope(json::array{});
  e["store"] = true;
  runs::SpecFields f;
  CHECK(runs::spec_fields(e, f)->exit_ == cmdatl::Exit::USAGE);
}

TEST_CASE("a run's state from its records, every row") {
  using runs::Alive;
  using runs::Probe;
  const auto state = [](const runs::Records& r, Alive sup, Alive drv, Probe probe) { return runs::run_state(r, sup, drv, probe); };
  auto r = records();
  // No pid yet: starting while the supervisor may live, else unknown.
  CHECK(state(r, Alive::MAYBE, Alive::NO_PID, Probe::UNKNOWN).state_ == "starting");
  CHECK(state(r, Alive::GONE, Alive::NO_PID, Probe::UNKNOWN).state_ == "unknown");
  CHECK(state(r, Alive::NO_PID, Alive::NO_PID, Probe::UNKNOWN).run_error_ == "unknown.run_ended");
  // A pid and no start line (the driver checking its refusals): starting while either may live.
  r.pid_ = 42;
  CHECK(state(r, Alive::GONE, Alive::MAYBE, Probe::UNKNOWN).state_ == "starting");
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::UNKNOWN).state_ == "unknown");
  // A start line: the probe decides.
  r.start_ = json::object{{"event", "start"}};
  CHECK(state(r, Alive::GONE, Alive::MAYBE, Probe::HELD).state_ == "running");
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::HELD).state_ == "unknown");
  CHECK(state(r, Alive::GONE, Alive::MAYBE, Probe::FREE).state_ == "lost");
  CHECK(state(r, Alive::GONE, Alive::MAYBE, Probe::FREE).run_error_ == "unknown.run_ended");
  CHECK(state(r, Alive::GONE, Alive::MAYBE, Probe::UNKNOWN).state_ == "unknown");
  // A complete reply, with or without exit.json.
  r.reply_ = reply_record(0, "", true);
  r.reply_exit_ = 0;
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "ended");
  CHECK(*state(r, Alive::GONE, Alive::GONE, Probe::FREE).run_exit_ == 0);
  r.reply_ = reply_record(4, "unknown.owed_at_end", true);
  r.reply_exit_ = 4;
  const auto owed = state(r, Alive::GONE, Alive::GONE, Probe::FREE);
  CHECK(owed.state_ == "ended");
  CHECK(owed.run_error_ == "unknown.owed_at_end");
  CHECK(runs::exit_for(owed) == cmdatl::Exit::UNKNOWN);
  r.reply_ = reply_record(3, "refused.not_armed");
  r.reply_exit_ = 3;
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "refused");
  r.reply_ = reply_record(5, "program.live_unsupported");
  r.reply_exit_ = 5;
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "failed");
  CHECK(runs::exit_for(state(r, Alive::GONE, Alive::GONE, Probe::FREE)) == cmdatl::Exit::PROGRAM);
  // exit.json contradicting the reply: never success.
  r.exit_present_ = true;
  r.exit_valid_ = true;
  r.spawned_ = true;
  r.exit_code_ = 0;
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "unknown");
  // exit.json without a reply: spawned is unknown, not spawned is failed.
  r.reply_.reset();
  r.reply_exit_.reset();
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "unknown");
  r.spawned_ = false;
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "failed");
  // A damaged exit.json.
  r.exit_valid_ = false;
  CHECK(state(r, Alive::GONE, Alive::GONE, Probe::FREE).state_ == "unknown");
  CHECK(runs::terminal(runs::State{"lost", std::nullopt, ""}));
  CHECK(!runs::terminal(runs::State{"running", std::nullopt, ""}));
  CHECK(!runs::terminal(runs::State{"starting", std::nullopt, ""}));
}

TEST_CASE("records are trusted only whole: unterminated lines, damaged files and other runs' records are not") {
  const ScratchDir dir;
  const std::string run = "r000000000001";
  spit(dir.path() / "reply.json", "{\"run\":\"r000000000001\",\"exit\":0");
  spit(dir.path() / "events.jsonl", "{\"event\":\"start\",\"run\":\"r000000000001\",\"at\":\"x\"}");
  spit(dir.path() / "pid", "12x\n");
  auto r = runs::read_records(dir.path(), run);
  CHECK(!r.reply_);
  CHECK(!r.start_);
  CHECK(!r.pid_);
  spit(dir.path() / "reply.json", "{\"run\":\"r000000000002\",\"exit\":0}\n");
  spit(dir.path() / "events.jsonl",
       "{\"event\":\"start\",\"run\":\"r000000000001\",\"at\":\"a\"}\n{\"event\":\"clock\",\"at\":\"b\"}\n{\"event\":\"placed\",\"at\":\"c\"}\n");
  spit(dir.path() / "pid", "12\n");
  spit(dir.path() / "exit.json", "{\"format\":1,\"pid\":12,\"exit\":300,\"signal\":null,\"spawned\":true}");
  r = runs::read_records(dir.path(), run);
  CHECK(!r.reply_);
  CHECK(r.start_);
  CHECK(*r.pid_ == 12);
  CHECK(r.clock_ == std::string("b"));
  CHECK(r.counted_);
  CHECK(r.counts_.at("placed") == 1);
  CHECK(r.exit_present_);
  CHECK(!r.exit_valid_);
  // A record that is not a regular file is never read, and never blocks.
  std::filesystem::remove(dir.path() / "exit.json");
  REQUIRE(::mkfifo((dir.path() / "exit.json").c_str(), 0600) == 0);
  r = runs::read_records(dir.path(), run);
  CHECK(r.exit_present_);
  CHECK(!r.exit_valid_);
}

TEST_CASE("exit.json and reply.json are admitted only whole, every field with its type, for this run") {
  const auto exit_of = [](const std::string& text) { return runs::exit_record_of(json::parse(text).as_object()); };
  CHECK(exit_of("{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}"));
  CHECK(exit_of("{\"format\":1,\"pid\":7,\"exit\":0,\"signal\":null,\"spawned\":true,\"ended\":\"t\"}"));
  CHECK(exit_of("{\"format\":1,\"pid\":7,\"exit\":null,\"signal\":9,\"spawned\":true,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":7,\"signal\":null,\"spawned\":true,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":7,\"exit\":\"bad\",\"signal\":null,\"spawned\":true,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":7,\"exit\":0,\"signal\":9,\"spawned\":true,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":0,\"exit\":0,\"signal\":null,\"spawned\":true,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":5,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":0,\"exit\":0,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":\"no\",\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":2,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}"));
  CHECK(!exit_of("{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false}"));
  const std::string run = "r000000000001";
  const auto reply_ok = [&run](const std::string& text) { return runs::valid_reply(json::parse(text).as_object(), run); };
  const std::string success =
    "{\"run\":\"r000000000001\",\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[]}";
  CHECK(reply_ok(success));
  CHECK(!reply_ok("{\"exit\":0}"));
  CHECK(!reply_ok("{\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[]}"));
  CHECK(!reply_ok("{\"run\":\"r000000000002\",\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[]}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":0,\"result\":\"ended\",\"executions\":3,\"owed_at_end\":[]}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":0,\"result\":\"failed\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[]}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[{}]}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[],"
                  "\"error\":{\"code\":\"x\"}}"));
  CHECK(reply_ok("{\"run\":\"r000000000001\",\"exit\":4,\"result\":\"unknown\",\"error\":{\"code\":\"unknown.owed_at_end\"}}"));
  CHECK(reply_ok("{\"run\":\"r000000000001\",\"exit\":3,\"result\":\"refused\",\"error\":{\"code\":\"refused.not_armed\"}}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":3,\"result\":\"refused\"}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":3,\"result\":\"failed\",\"error\":{\"code\":\"refused.not_armed\"}}"));
  CHECK(!reply_ok("{\"run\":\"r000000000001\",\"exit\":9,\"result\":\"failed\",\"error\":{\"code\":\"x\"}}"));
}

TEST_CASE("contradictory or damaged completion records never establish success, nor that nothing was launched") {
  const ScratchDir dir;
  const std::string run = "r000000000001";
  const std::string success =
    "{\"run\":\"r000000000001\",\"exit\":0,\"result\":\"ended\",\"outcome\":\"clean\",\"executions\":3,\"owed_at_end\":[]}\n";
  const auto state_of = [&dir, &run]() {
    const auto records = runs::read_records(dir.path(), run);
    return std::make_pair(runs::run_state(records, runs::Alive::GONE, runs::Alive::GONE, runs::Probe::FREE), runs::early_end(records));
  };
  // A success reply beside a record saying no driver was spawned.
  spit(dir.path() / "reply.json", success);
  spit(dir.path() / "exit.json", "{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}");
  auto [state, early] = state_of();
  CHECK(state.state_ == "unknown");
  CHECK(early == runs::EarlyEnd::RELAY);
  // A success reply beside a damaged exit record.
  spit(dir.path() / "exit.json", "{\"format\":1,\"pid\":7,\"exit\":\"bad\",\"signal\":null,\"spawned\":true,\"ended\":\"t\"}");
  std::tie(state, early) = state_of();
  CHECK(state.state_ == "unknown");
  // Only a bare reply.
  std::filesystem::remove(dir.path() / "exit.json");
  spit(dir.path() / "reply.json", "{\"exit\":0}\n");
  std::tie(state, early) = state_of();
  CHECK(state.state_ == "unknown");
  CHECK(early == runs::EarlyEnd::UNKNOWN);
  // A damaged exit record alone: unknown, never "nothing was launched".
  std::filesystem::remove(dir.path() / "reply.json");
  spit(dir.path() / "exit.json", "{\"format\":1,\"spawned\":false}");
  std::tie(state, early) = state_of();
  CHECK(state.state_ == "unknown");
  CHECK(early == runs::EarlyEnd::UNKNOWN);
  // A spawned driver that left no reply: unknown.
  spit(dir.path() / "exit.json", "{\"format\":1,\"pid\":7,\"exit\":0,\"signal\":null,\"spawned\":true,\"ended\":\"t\"}");
  std::tie(state, early) = state_of();
  CHECK(state.state_ == "unknown");
  CHECK(early == runs::EarlyEnd::UNKNOWN);
  // Only a validated record not spawned says nothing was launched.
  spit(dir.path() / "exit.json", "{\"format\":1,\"pid\":0,\"exit\":null,\"signal\":null,\"spawned\":false,\"ended\":\"t\"}");
  std::tie(state, early) = state_of();
  CHECK(state.state_ == "failed");
  CHECK(early == runs::EarlyEnd::NOT_LAUNCHED);
  // The writer's own success with its matching record.
  spit(dir.path() / "reply.json", success);
  spit(dir.path() / "exit.json", "{\"format\":1,\"pid\":7,\"exit\":0,\"signal\":null,\"spawned\":true,\"ended\":\"t\"}");
  std::tie(state, early) = state_of();
  CHECK(state.state_ == "ended");
  CHECK(early == runs::EarlyEnd::RELAY);
  // A reply still being written is not evidence either way.
  std::filesystem::remove(dir.path() / "exit.json");
  spit(dir.path() / "reply.json", "{\"run\":\"r000000000001\",\"exit\":0");
  std::tie(state, early) = state_of();
  CHECK(early == runs::EarlyEnd::NONE);
}

TEST_CASE("the table's run commands: options, codes, kinds of code and the schema version") {
  const cmdatl::Table table(cmdatl::command_list());
  const auto* start = table.resolve({"run", "start"}).command_;
  const auto* status = table.resolve({"run", "status"}).command_;
  const auto* stop = table.resolve({"run", "stop"}).command_;
  REQUIRE(start != nullptr);
  REQUIRE(status != nullptr);
  REQUIRE(stop != nullptr);
  std::vector<std::string> names;
  for (const auto& option : start->options_) {
    names.push_back(option.name_);
  }
  CHECK(names == std::vector<std::string>{"param", "wait"});
  CHECK(start->may_send_);
  CHECK(static_cast<bool>(start->run_config_));
  CHECK(static_cast<bool>(status->run_config_));
  CHECK(stop->available_);
  CHECK(stop->may_send_);
  for (const auto& option : stop->options_) {
    CHECK(option.name_ != "flatten");
  }
  std::set<std::string> catalog;
  for (const auto& entry : cmdatl::error_catalog()) {
    catalog.insert(entry.code_);
  }
  for (const auto& code : cmdatl::RUN_CODES) {
    INFO(code);
    CHECK(catalog.count(code) == 1);
    CHECK(std::find(start->errors_.begin(), start->errors_.end(), code) != start->errors_.end());
  }
  CHECK(cmdatl::check_table(table).empty());
  CHECK(cmdatl::SCHEMA_VERSION == 6);
  // Two kinds of code on one command is a problem the table check reports.
  auto list = cmdatl::command_list();
  for (auto& command : list) {
    if (command.path_ == std::vector<std::string>{"run", "status"}) {
      command.runs([](const cmdatl::Invocation&, const cmdatl::Table&, cmdatl::Output&) { return cmdatl::Reply{}; });
    }
  }
  const cmdatl::Table doubled(list);
  CHECK(!cmdatl::check_table(doubled).empty());
}

TEST_CASE("run status answers for a missing run, a bad id, and an account with no run") {
  const PaperFixture fixture;
  const auto bad = fixture.json({"run", "status", "nope"});
  CHECK(bad.exit_ == 2);
  CHECK(code_of(bad.out_) == "usage.bad_value");
  const auto none = fixture.json({"run", "status"});
  CHECK(none.exit_ == 1);
  CHECK(code_of(none.out_) == "run.unknown");
  const auto missing = fixture.json({"run", "status", "r000000000001"});
  CHECK(missing.exit_ == 1);
  CHECK(code_of(missing.out_) == "run.unknown");
  CHECK(reply_of(missing.out_).at("env").as_string() == "paper");
}

// --- Processes, against the real atlc and algotradelib ---

namespace {

/** @brief The real atlc and the library prefix, or nothing. */
std::optional<std::pair<std::string, std::string>> run_equipment() {
  const auto atlc = cmdatl::variable("CMDATL_TEST_ATLC");
  const auto prefix = cmdatl::variable("CMDATL_TEST_PREFIX");
  if (!atlc || !prefix) {
    return std::nullopt;
  }
  return std::make_pair(*atlc, *prefix);
}

/**
 * @brief Daily bars of 005930, date-only rows (atlc stamps them at 00:00 UTC): a walk with a sharp dip every 23 days,
 *   so the mean-reversion starter trades.
 * @param days How many weekdays from 2024-01-02.
 * @param extra_symbols Other symbols in the file, each with the same bars, to make the file larger.
 * @return The CSV.
 */
std::string dip_bars(int days, int extra_symbols = 0) {
  std::string out = "symbol,date,open,high,low,close,volume\n";
  long day = 19724;  // 2024-01-02
  long close = 78000;
  std::vector<std::string> rows;
  for (int k = 0, made = 0; made < days; ++day) {
    const long weekday = (day + 3) % 7;
    if (weekday >= 5) {
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
    long y = 0;
    long m = 0;
    long d = 0;
    {
      long z = day + 719468;
      const long era = z / 146097;
      const long doe = z - era * 146097;
      const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const long mp = (5 * doy + 2) / 153;
      d = doy - (153 * mp + 2) / 5 + 1;
      m = mp < 10 ? mp + 3 : mp - 9;
      y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    }
    char date[64];
    std::snprintf(date, sizeof date, "%04ld-%02ld-%02ld", y, m, d);
    char row[160];
    std::snprintf(row, sizeof row, ",%s,%ld,%ld,%ld,%ld,100000\n", date, open, std::max(open, close) + 300, std::min(open, close) - 300, close);
    out += std::string("005930") + row;
    for (int s = 0; s < extra_symbols; ++s) {
      char symbol[16];
      std::snprintf(symbol, sizeof symbol, "9%05d", s);
      out += symbol + std::string(row);
    }
    ++k;
    ++made;
  }
  return out;
}

/** @brief The program, the specs and the build cache every process case shares, so the driver is built once. */
struct Shared {
  /** @brief Where they live. */
  ScratchDir dir_;

  Shared() {
    spit(dir_.path() / "dip.atls", slurp(std::filesystem::path(CMDATL_SOURCE_DIR) / "templates" / "mean-reversion.atls"));
    spit(dir_.path() / "bars.csv", dip_bars(160));
    spit(dir_.path() / "long.csv", dip_bars(500));
    spit(dir_.path() / "wide.csv", dip_bars(400, 600));
    for (const auto& [name, data] : std::vector<std::pair<std::string, std::string>>{{"dip", "bars.csv"}, {"long", "long.csv"}, {"wide", "wide.csv"}}) {
      spit(dir_.path() / (name + ".toml"),
           "strategy = \"dip.atls\"\n\n[data]\nsource = \"csv\"\nfile = \"" + data +
             "\"\nsymbol_column = \"symbol\"\ntime = \"date\"\ntime_format = \"iso\"\n\n[instruments]\nstock = \"005930\"\n\n"
             "[replay]\ncash = 100000000\ncommission = 0.0005\nmarket = \"kr\"\n\n[orders]\ntype = \"limit\"\noffset = 0.01\n");
    }
    std::filesystem::create_directories(dir_.path() / "cache");
  }

  /**
   * @brief A spec's path.
   * @param name `dip`, `long` or `wide`.
   * @return The path.
   */
  std::string spec(const std::string& name) const { return (dir_.path() / (name + ".toml")).string(); }
};

/**
 * @brief The shared fixture, made once per test process.
 * @return It.
 */
Shared& shared() {
  static Shared s;
  return s;
}

/** @brief The operator's file for the process cases: 005930 allowed with room for every order of the dip data. */
ConfigParts run_operator() {
  ConfigParts parts;
  parts.limits_ =
    "symbols = [\"005930\"]\n"
    "position = { \"005930\" = 100000 }\n"
    "order_value = 100000000000\n"
    "daily_orders = 1000\n";
  return parts;
}

/** @brief Points cmdatl at the real atlc, the build at the library prefix and the shared cache, for a case's length. */
class RunEquipped {
public:
  /**
   * @brief Sets the variables.
   * @param atlc atlc's path.
   * @param prefix The library prefix.
   */
  RunEquipped(const std::string& atlc, const std::string& prefix)
  : atlc_("CMDATL_ATLC", atlc),
    prefix_("CMAKE_PREFIX_PATH", prefix),
    cache_("XDG_CACHE_HOME", (shared().dir_.path() / "cache").string()) {}

private:
  ScopedVariable atlc_;
  ScopedVariable prefix_;
  ScopedVariable cache_;
};

/**
 * @brief A run's directory under a fixture.
 * @param fixture The fixture.
 * @param run The run's id.
 * @return The directory.
 */
std::filesystem::path run_path(const PaperFixture& fixture, const std::string& run) {
  return fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001" / run;
}

/**
 * @brief `run status RUN` until its state is final, at most `seconds`.
 * @param fixture The fixture.
 * @param run The run's id.
 * @param seconds The bound.
 * @return The last reply.
 */
json::object status_until_final(const PaperFixture& fixture, const std::string& run, int seconds = 120) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (true) {
    const auto ran = fixture.json({"run", "status", run});
    REQUIRE(ran.exit_ == 0);
    auto reply = reply_of(ran.out_);
    const std::string state(reply.at("state").as_string());
    if (state != "starting" && state != "running") {
      return reply;
    }
    REQUIRE(std::chrono::steady_clock::now() < until);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

/**
 * @brief A run's fills from its event log, as `date,side,quantity,price`.
 * @param dir The run's directory.
 * @return The fills.
 */
std::vector<std::string> run_fills(const std::filesystem::path& dir) {
  std::vector<std::string> out;
  for (const auto& line : lines_of(slurp(dir / "events.jsonl"))) {
    const auto event = json::parse(line).as_object();
    if (event.at("event").as_string() == "filled") {
      out.push_back(std::string(event.at("at").as_string()).substr(0, 10) + "," + std::string(event.at("side").as_string()) + "," +
                    std::string(event.at("quantity").as_string()) + "," + std::string(event.at("price").as_string()));
    }
  }
  return out;
}

/**
 * @brief A backtest's fills from atlc's fills file, as `date,side,quantity,price`.
 * @param path The fills file.
 * @return The fills.
 */
std::vector<std::string> backtest_fills(const std::filesystem::path& path) {
  std::vector<std::string> out;
  const auto lines = lines_of(slurp(path));
  for (std::size_t k = 1; k < lines.size(); ++k) {
    std::vector<std::string> cells;
    std::size_t start = 0;
    while (true) {
      const auto comma = lines[k].find(',', start);
      cells.push_back(lines[k].substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    REQUIRE(cells.size() >= 5);
    out.push_back(cells[0].substr(0, 10) + "," + cells[2] + "," + cells[3] + "," + cells[4]);
  }
  return out;
}

}  // namespace

TEST_CASE("run start --wait trades as strategy backtest does on the same spec, fill for fill", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  const auto fills = shared().dir_.path() / "fills.csv";
  const auto backtest = fixture.json({"strategy", "backtest", shared().spec("dip"), "--fills", fills.string()});
  REQUIRE(backtest.exit_ == 0);
  const auto expected = backtest_fills(fills);
  // The conditions that make equality expected, asserted before comparing: bars that trade, every limit inside the
  // collar and the operator's caps (the spec's offset is 1 %, the collar 5 %, the cap 100,000 shares), enough cash.
  REQUIRE(expected.size() >= 6);
  const auto ran = fixture.json({"run", "start", shared().spec("dip"), "--wait"});
  INFO(ran.out_);
  INFO(ran.err_);
  REQUIRE(ran.exit_ == 0);
  const auto reply = reply_of(ran.out_);
  CHECK(reply.at("state").as_string() == "ended");
  CHECK(reply.at("run_exit").as_int64() == 0);
  const auto& summary = reply.at("summary").as_object();
  CHECK(summary.at("executions").as_int64() == static_cast<std::int64_t>(expected.size()));
  const auto run = std::string(reply.at("run").as_string());
  CHECK(run_fills(run_path(fixture, run)) == expected);
  CHECK(reply_of(backtest.out_).at("report").as_object().at("executions").as_int64() == static_cast<std::int64_t>(expected.size()));
  // The cache: a second start of the same program builds nothing.
  const auto driver = shared().dir_.path() / "cache" / "cmdatl" / "drivers";
  std::optional<std::filesystem::file_time_type> before;
  std::filesystem::path shape;
  for (const auto& entry : std::filesystem::directory_iterator(driver)) {
    if (entry.path().filename().string().rfind("Dip-", 0) == 0) {
      shape = entry.path();
      before = std::filesystem::last_write_time(shape / "build" / "driver");
    }
  }
  REQUIRE(before);
  const auto again = fixture.json({"run", "start", shared().spec("dip"), "--wait", "-P", "lookback=21"});
  CHECK(again.exit_ == 0);
  CHECK((std::filesystem::last_write_time(shape / "build" / "driver") == *before));
  // The driver's include closure, from the compiler's own dependency files: no CLI11, nothing of cmdatl's cli/ or
  // strategy/ (the layout check's driver row, seen in the real build).
  std::size_t read = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(shape / "build")) {
    if (entry.is_regular_file() && entry.path().extension() == ".d") {
      const auto deps = slurp(entry.path());
      ++read;
      CHECK(deps.find("/CLI/") == std::string::npos);
      CHECK(deps.find("cmdatl/cli/") == std::string::npos);
      CHECK(deps.find("cmdatl/strategy/") == std::string::npos);
      CHECK(deps.find("cmdatl/driver/run.hpp") != std::string::npos);
    }
  }
  CHECK(read >= 1);
}

TEST_CASE("two runs started at once on one account build the program once and trade apart", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  // A cache of this case's own, so both starts meet an unbuilt shape and one waits on the other's lock.
  const ScratchDir cache;
  const ScopedVariable own_cache("XDG_CACHE_HOME", cache.path().string());
  fixture.arm();
  Spawned first({"run", "start", shared().spec("dip"), "--wait", "--json"});
  Spawned second({"run", "start", shared().spec("dip"), "--wait", "--json"});
  const int a = first.wait(600);
  const int b = second.wait(600);
  INFO(first.out() << first.err());
  INFO(second.out() << second.err());
  REQUIRE(WIFEXITED(a));
  REQUIRE(WIFEXITED(b));
  CHECK(WEXITSTATUS(a) == 0);
  CHECK(WEXITSTATUS(b) == 0);
  const auto one = reply_of(first.out());
  const auto two = reply_of(second.out());
  CHECK(one.at("run") != two.at("run"));
  CHECK(one.at("summary").as_object().at("executions") == two.at("summary").as_object().at("executions"));
  CHECK(one.at("summary").as_object().at("closing_equity") == two.at("summary").as_object().at("closing_equity"));
  std::size_t shapes = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cache.path() / "cmdatl" / "drivers")) {
    shapes += entry.is_directory() ? 1 : 0;
  }
  CHECK(shapes == 1);
}

TEST_CASE("run start returns while the run trades, its output closed, and run status follows the run to its end", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  // Built once before, so this start answers within its wait.
  REQUIRE(fixture.json({"run", "start", shared().spec("dip"), "--wait"}).exit_ == 0);
  Spawned started({"run", "start", shared().spec("long"), "--json"});
  const int status = started.wait(120);
  INFO(started.out());
  INFO(started.err());
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  // Nothing the supervisor or the driver holds keeps cmdatl's output open.
  CHECK(started.stdout_ended());
  const auto reply = reply_of(started.out());
  CHECK(reply.at("state").as_string() == "running");
  const auto run_id = std::string(reply.at("run").as_string());
  const auto final = status_until_final(fixture, run_id);
  CHECK(final.at("state").as_string() == "ended");
  CHECK(final.at("run_exit").as_int64() == 0);
  CHECK(final.at("summary").as_object().at("executions").as_int64() > 0);
  // In text, --wait ends with the run's state.
  const auto text = support::run({"run", "start", shared().spec("dip"), "--wait"}, true);
  CHECK(text.exit_ == 0);
  CHECK(text.out_.find("ended") != std::string::npos);
}

TEST_CASE("run start relays refusals: its own before anything is built, and the driver's", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  {
    // The default limits allow 005930; the armed file is not there.
    const PaperFixture fixture;
    const RunEquipped equipped(equipment->first, equipment->second);
    const auto unarmed = fixture.json({"run", "start", shared().spec("dip")});
    CHECK(unarmed.exit_ == 3);
    CHECK(code_of(unarmed.out_) == "refused.not_armed");
    const auto runs_dir = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001";
    CHECK((!std::filesystem::exists(runs_dir) || std::filesystem::is_empty(runs_dir)));
  }
  {
    auto parts = run_operator();
    parts.limits_ = "symbols = [\"000660\"]\nposition = { \"000660\" = 10 }\norder_value = 1000000\ndaily_orders = 3\n";
    const PaperFixture fixture(parts);
    const RunEquipped equipped(equipment->first, equipment->second);
    fixture.arm();
    const auto other = fixture.json({"run", "start", shared().spec("dip")});
    CHECK(other.exit_ == 3);
    CHECK(code_of(other.out_) == "refused.symbol_not_allowed");
  }
  if (const auto examples = cmdatl::variable("CMDATL_TEST_ATLC_EXAMPLES")) {
    // A program with on_fill: the build succeeds and the driver refuses it, exit 5, relayed.
    const PaperFixture fixture(run_operator());
    const RunEquipped equipped(equipment->first, equipment->second);
    fixture.arm();
    spit(fixture.dir() / "trail.atls", slurp(std::filesystem::path(*examples) / "trailing-stop.atls"));
    spit(fixture.dir() / "bars.csv", dip_bars(60));
    spit(fixture.dir() / "trail.toml",
         "strategy = \"trail.atls\"\n\n[data]\nsource = \"csv\"\nfile = \"bars.csv\"\nsymbol_column = \"symbol\"\ntime = \"date\"\n"
         "time_format = \"iso\"\n\n[instruments]\nstock = \"005930\"\n\n[replay]\nmarket = \"kr\"\n\n[orders]\ntype = \"limit\"\noffset = 0.01\n");
    const auto ran = fixture.json({"run", "start", (fixture.dir() / "trail.toml").string()});
    INFO(ran.out_);
    INFO(ran.err_);
    CHECK(ran.exit_ == 5);
    CHECK(code_of(ran.out_) == "program.live_unsupported");
    CHECK(reply_of(ran.out_).at("state").as_string() == "failed");
  }
}

TEST_CASE("a driver stopped before its start line is unknown, never lost, and a killed one is lost", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  REQUIRE(fixture.json({"run", "start", shared().spec("wide"), "--wait"}).exit_ == 0);
  // The wide data takes the driver a while to load: it is stopped as soon as its pid appears, before its start line.
  bool stopped_early = false;
  for (int attempt = 0; attempt < 3 && !stopped_early; ++attempt) {
    Spawned started({"run", "start", shared().spec("wide"), "--json"});
    const auto runs_dir = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001";
    std::optional<long> pid;
    std::filesystem::path dir;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!pid && std::chrono::steady_clock::now() < until) {
      for (const auto& entry : std::filesystem::directory_iterator(runs_dir)) {
        if (std::filesystem::exists(entry.path() / "pid") && !std::filesystem::exists(entry.path() / "exit.json")) {
          const auto text = slurp(entry.path() / "pid");
          if (!text.empty() && text.back() == '\n') {
            pid = std::stol(text);
            dir = entry.path();
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(pid);
    ::kill(static_cast<pid_t>(*pid), SIGSTOP);
    const auto events = slurp(dir / "events.jsonl");
    if (!events.empty()) {
      ::kill(static_cast<pid_t>(*pid), SIGKILL);
      ::kill(static_cast<pid_t>(*pid), SIGCONT);
      started.wait(60);
      continue;
    }
    stopped_early = true;
    const int status = started.wait(60);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 4);
    CHECK(code_of(started.out()) == "unknown.run_start");
    const auto run = dir.filename().string();
    const auto stopped = fixture.json({"run", "status", run});
    CHECK(reply_of(stopped.out_).at("state").as_string() == "starting");
    ::kill(static_cast<pid_t>(*pid), SIGCONT);
    const auto final = status_until_final(fixture, run);
    CHECK(final.at("state").as_string() == "ended");
  }
  CHECK(stopped_early);
  // Killed after its start line, with its supervisor: the probe finds the lock free and no reply: lost.
  Spawned started({"run", "start", shared().spec("long"), "--json"});
  const int started_status = started.wait(120);
  REQUIRE(WEXITSTATUS(started_status) == 0);
  const auto run = std::string(reply_of(started.out()).at("run").as_string());
  const auto dir = run_path(fixture, run);
  const auto supervisor = std::stol(slurp(dir / "supervisor"));
  const auto driver = std::stol(slurp(dir / "pid"));
  ::kill(static_cast<pid_t>(supervisor), SIGKILL);
  ::kill(static_cast<pid_t>(driver), SIGKILL);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto lost = fixture.json({"run", "status", run});
  CHECK(lost.exit_ == 0);
  if (!std::filesystem::exists(dir / "reply.json") || slurp(dir / "reply.json").empty()) {
    CHECK(reply_of(lost.out_).at("state").as_string() == "lost");
    CHECK(reply_of(lost.out_).at("run_error").as_string() == "unknown.run_ended");
  }
}

namespace {

/**
 * @brief A process's parent, as `ps` reports it.
 * @param pid The process.
 * @return The parent's pid, or nothing.
 */
std::optional<long> parent_of(long pid) {
  const std::string command = "ps -o ppid= -p " + std::to_string(pid);
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return std::nullopt;
  }
  char text[64] = {};
  const bool got = std::fgets(text, sizeof text, pipe) != nullptr;
  ::pclose(pipe);
  if (!got) {
    return std::nullopt;
  }
  return std::strtol(text, nullptr, 10);
}

/**
 * @brief The run directory a fixture's account gained since a set of known ones.
 * @param fixture The fixture.
 * @param known The directories already there.
 * @return The new one, or nothing yet.
 */
std::optional<std::filesystem::path> new_run_dir(const PaperFixture& fixture, const std::set<std::string>& known) {
  const auto runs_dir = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001";
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(runs_dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (known.count(it->path().filename().string()) == 0) {
      return it->path();
    }
  }
  return std::nullopt;
}

/**
 * @brief The run directories a fixture's account has.
 * @param fixture The fixture.
 * @return Their names.
 */
std::set<std::string> run_dirs(const PaperFixture& fixture) {
  std::set<std::string> out;
  const auto runs_dir = fixture.dir() / "state" / "cmdatl" / "runs" / "paper-paper-paper-0001";
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(runs_dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    out.insert(it->path().filename().string());
  }
  return out;
}

}  // namespace

TEST_CASE("a driver is built against the package it names and no other, and a missing package is refused", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const ScratchDir work;
  // Two installations of the library, each a whole prefix, and a toolchain file that makes the second reachable: a
  // build that fell back to any package CMake could find would succeed against it.
  const auto a = work.path() / "a";
  const auto b = work.path() / "b";
  std::filesystem::copy(equipment->second, a, std::filesystem::copy_options::recursive);
  std::filesystem::copy(equipment->second, b, std::filesystem::copy_options::recursive);
  const auto package_a = (a / "lib" / "cmake" / "algotradelib").string();
  const auto package_b = (b / "lib" / "cmake" / "algotradelib").string();
  spit(work.path() / "toolchain.cmake", "list(APPEND CMAKE_PREFIX_PATH \"" + b.string() + "\")\n");
  const ScopedVariable cache("XDG_CACHE_HOME", (work.path() / "cache").string());
  const ScopedVariable toolchain("CMAKE_TOOLCHAIN_FILE", (work.path() / "toolchain.cmake").string());
  const auto program = (shared().dir_.path() / "dip.atls").string();
  const auto quiet = [](const std::string&) {};
  const auto request = [&](const std::string& package, const std::string& path = "") {
    return build::Request{equipment->first, "test", path.empty() ? program : path, "Dip", "incremental", package};
  };
  const auto used_package = [](const std::filesystem::path& shape) {
    std::string text;
    REQUIRE(build::read_file(shape / "build" / "CMakeCache.txt", text));
    return build::real_path(build::cache_value(text, "algotradelib_DIR").value_or(""));
  };
  for (const auto* name : {"run-a", "run-b", "run-gone", "run-fresh"}) {
    std::filesystem::create_directories(work.path() / name);
  }
  // Two packages sharing one cache: two shapes, each built against its own.
  REQUIRE(!build::build_driver(request(package_a), work.path() / "run-a", quiet));
  REQUIRE(!build::build_driver(request(package_b), work.path() / "run-b", quiet));
  CHECK(std::filesystem::exists(work.path() / "run-a" / "driver"));
  CHECK(std::filesystem::exists(work.path() / "run-b" / "driver"));
  std::map<std::string, std::filesystem::path> shapes;
  for (const auto& entry : std::filesystem::directory_iterator(work.path() / "cache" / "cmdatl" / "drivers")) {
    shapes[used_package(entry.path())] = entry.path();
  }
  REQUIRE(shapes.size() == 2);
  CHECK(shapes.count(build::real_path(package_a)) == 1);
  CHECK(shapes.count(build::real_path(package_b)) == 1);
  // The first package removed, the second still reachable: refused before anything is built, copied or run, with an
  // old build of the first's shape in the cache, and for a program never built.
  std::filesystem::rename(a, work.path() / "a-gone");
  const auto gone = build::build_driver(request(package_a), work.path() / "run-gone", quiet);
  REQUIRE(gone);
  CHECK(gone->exit_ == cmdatl::Exit::FAILED);
  CHECK(std::string(gone->body_.at("error").as_object().at("code").as_string()) == "build.failed");
  CHECK(!std::filesystem::exists(work.path() / "run-gone" / "driver"));
  spit(work.path() / "other.atls", slurp(program));
  const auto fresh = build::build_driver(request(package_a, (work.path() / "other.atls").string()), work.path() / "run-fresh", quiet);
  REQUIRE(fresh);
  CHECK(!std::filesystem::exists(work.path() / "run-fresh" / "driver"));
  // CMake's own check, below cmdatl's: the driver's project configured for the missing package fails though the
  // toolchain file makes another one findable.
  const auto cmake = cmdatl::find_on_path("cmake");
  REQUIRE(cmake);
  const auto configured = cmdatl::run_child(
    {*cmake, "-S", (shapes.at(build::real_path(package_b)) / "src").string(), "-B", (work.path() / "probe-build").string(),
     "-DCMAKE_BUILD_TYPE=Release", "-DCMDATL_ALGOTRADELIB_DIR=" + package_a},
    quiet
  );
  CHECK(configured.kind_ == cmdatl::ChildResult::Kind::FINISHED);
  CHECK(configured.code_ != 0);
}

TEST_CASE("run start never answers running for a run whose driver and supervisor died after its start line", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  REQUIRE(fixture.json({"run", "start", shared().spec("wide"), "--wait"}).exit_ == 0);
  bool exercised = false;
  for (int attempt = 0; attempt < 3 && !exercised; ++attempt) {
    const auto known = run_dirs(fixture);
    Spawned started({"run", "start", shared().spec("wide"), "--json"});
    // The launcher is paused as soon as the driver's pid is published, before it can have read a start line (the wide
    // data takes the driver a while to load).
    std::optional<std::filesystem::path> dir;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < until) {
      dir = new_run_dir(fixture, known);
      if (dir && std::filesystem::exists(*dir / "pid") && !slurp(*dir / "pid").empty()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(dir);
    started.signal(SIGSTOP);
    if (!slurp(*dir / "events.jsonl").empty()) {
      // The start line may already have been seen: not this case; try again.
      started.signal(SIGCONT);
      started.wait(60);
      status_until_final(fixture, dir->filename().string());
      continue;
    }
    const auto driver = std::stol(slurp(*dir / "pid"));
    const auto supervisor = parent_of(driver);
    REQUIRE(supervisor);
    const auto line_until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (slurp(*dir / "events.jsonl").find('\n') == std::string::npos && std::chrono::steady_clock::now() < line_until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(slurp(*dir / "events.jsonl").find('\n') != std::string::npos);
    ::kill(static_cast<pid_t>(*supervisor), SIGKILL);
    ::kill(static_cast<pid_t>(driver), SIGKILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    started.signal(SIGCONT);
    const int status = started.wait(60);
    INFO(started.out());
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 4);
    const auto reply = reply_of(started.out());
    CHECK(reply.at("state").as_string() != "running");
    CHECK(reply.at("state").as_string() == "lost");
    exercised = true;
  }
  CHECK(exercised);
}

TEST_CASE("a descriptor inherited above a lowered limit reaches neither the supervisor nor the driver", "[acceptance][runs]") {
  const auto equipment = run_equipment();
  if (!equipment) {
    SKIP("CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX name no atlc and algotradelib prefix: the run cases did not run");
  }
  const PaperFixture fixture(run_operator());
  const RunEquipped equipped(equipment->first, equipment->second);
  fixture.arm();
  REQUIRE(fixture.json({"run", "start", shared().spec("dip"), "--wait"}).exit_ == 0);
  const auto known = run_dirs(fixture);
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // The write end at descriptor 300, inheritable, then the soft limit lowered below it.
    struct rlimit limit{};
    ::getrlimit(RLIMIT_NOFILE, &limit);
    limit.rlim_cur = limit.rlim_max == RLIM_INFINITY || limit.rlim_max >= 512 ? 512 : limit.rlim_max;
    ::setrlimit(RLIMIT_NOFILE, &limit);
    if (::dup2(fds[1], 300) != 300) {
      ::_exit(126);
    }
    ::close(fds[0]);
    ::close(fds[1]);
    limit.rlim_cur = 64;
    ::setrlimit(RLIMIT_NOFILE, &limit);
    const int null = ::open("/dev/null", O_RDWR);
    ::dup2(null, STDOUT_FILENO);
    ::dup2(null, STDERR_FILENO);
    const auto spec = shared().spec("long");
    ::execl(CMDATL_BINARY, CMDATL_BINARY, "run", "start", spec.c_str(), "--json", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(fds[1]);
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  // cmdatl answered; its caller's pipe must end now, though the run trades on.
  pollfd p{fds[0], POLLIN, 0};
  const auto ready = ::poll(&p, 1, 3000);
  char byte = 0;
  const auto got = ready > 0 ? ::read(fds[0], &byte, 1) : -1;
  ::close(fds[0]);
  CHECK(ready > 0);
  CHECK(got == 0);
  const auto dir = new_run_dir(fixture, known);
  REQUIRE(dir);
  const auto running = reply_of(fixture.json({"run", "status", dir->filename().string()}).out_);
  CHECK(running.at("state").as_string() == "running");
  CHECK(status_until_final(fixture, dir->filename().string()).at("state").as_string() == "ended");
}
#endif
