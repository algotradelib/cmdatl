#pragma once

// `strategy backtest`: the settings read and checked against the program (`settings.hpp`), the backtest driver built
// once per program shape (`build.hpp`, a driver whose main is atlc's `atls::run::main`), a copy of it run with the
// settings as its arguments, and its report returned unchanged.

#include "../core/reply.hpp"
#include "../core/table.hpp"
#include "../system/files.hpp"
#include "../system/process.hpp"
#include "./build.hpp"
#include "./settings.hpp"
#include "./strategy.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <stdlib.h>
#endif

namespace cmdatl {

/**
 * @brief What the backtest driver's end means: its one JSON report, or `replay.failed` (the driver is cmdatl's own, so
 *   its failures are never atlc's).
 * @param ran The driver's end.
 * @param report Set to the report.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> replay_outcome(const ChildResult& ran, json::object& report) {
  if (ran.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply("the backtest");
  }
  const std::string next = "check the data and the parameters' bounds";
  if (ran.kind_ != ChildResult::Kind::FINISHED || ran.signaled_) {
    return atlc_failure(
      Exit::FAILED, "replay.failed", "the replay failed: " + ending_of(ran, "the backtest driver"), ran, next
    );
  }
  if (ran.code_ == 1) {
    return atlc_failure(Exit::FAILED, "replay.failed", "the replay failed: " + last_line(ran), ran, next);
  }
  if (ran.code_ != 0) {
    return atlc_failure(
      Exit::FAILED, "replay.failed", "the replay failed: " + ending_of(ran, "the backtest driver"), ran, next
    );
  }
  auto parsed = one_object(ran.stdout_);
  if (!parsed) {
    return atlc_failure(
      Exit::FAILED, "replay.failed", "the backtest driver exited 0 without exactly one JSON report", ran, next
    );
  }
  report = std::move(*parsed);
  return std::nullopt;
}

#if !defined(_WIN32)
/** @brief A backtest's scratch directory, made under the cache and removed with everything in it when this ends. */
class ScratchCopy {
public:
  /**
   * @brief Makes it: `<cache>/cmdatl/backtests/XXXXXX`, private.
   * @param root The drivers' cache root (`<cache>/cmdatl/drivers`).
   */
  explicit ScratchCopy(const std::string& root) {
    const auto parent = std::filesystem::path(root).parent_path() / "backtests";
    if (make_private_dirs(parent) != 0) {
      return;
    }
    std::string pattern = (parent / "XXXXXX").string();
    if (::mkdtemp(pattern.data()) != nullptr) {
      path_ = pattern;
    }
  }

  ScratchCopy(const ScratchCopy&) = delete;
  ScratchCopy& operator=(const ScratchCopy&) = delete;

  /** @brief Removes it. */
  ~ScratchCopy() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  /**
   * @brief Where it is.
   * @return The path, or empty when it could not be made.
   */
  const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};
#endif

/**
 * @brief `strategy backtest TARGET [KEY=VALUE]... [--fills FILE]`; see the file comment.
 * @param invocation The arguments.
 * @param output Where progress goes: cmdatl's first line, then atlc's and the build's and the driver's stderr lines.
 * @return The reply.
 */
inline Reply backtest_reply(const Invocation& invocation, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)output;
  return error_reply(
    Exit::FAILED, "unavailable.platform", "strategy backtest builds and runs a driver on macOS and Linux only"
  );
#else
  GivenSettings given;
  if (auto refused = sort_words(target_and_settings(invocation), given)) {
    return std::move(*refused);
  }
  output.progress("cmdatl: replaying " + given.target_);
  if (interrupted()) {
    return interrupted_reply();
  }
  if (auto refused = read_given(given)) {
    return std::move(*refused);
  }
  // After the settings' own checks: finding atlc runs `atlc --version`, which may take a while.
  const auto located = locate_atlc();
  if (located.refusal_) {
    return *located.refusal_;
  }
  const auto& atlc = located.atlc_;
  CheckedSettings checked;
  if (auto refused = check_given(given, atlc.path_, output, false, checked)) {
    return std::move(*refused);
  }
  std::vector<std::string> argv;
  std::optional<ScratchCopy> scratch;
#if defined(CMDATL_RUN_TEST_HOOKS)
  // Test builds only: a stand-in driver instead of the build, so the reply's mapping is tested without CMake.
  if (const auto stand_in = variable("CMDATL_TEST_BACKTEST_DRIVER")) {
    argv.push_back(*stand_in);
  }
#endif
  if (argv.empty()) {
    const auto root = build::cache_root();
    if (!root) {
      return error_reply(
        Exit::FAILED, "cache.unavailable", "neither XDG_CACHE_HOME nor HOME names an absolute directory"
      );
    }
    scratch.emplace(*root);
    if (scratch->path().empty()) {
      return error_reply(
        Exit::FAILED, "cache.unavailable", "a scratch directory for the backtest cannot be made under " + *root
      );
    }
    build::Request request{
      atlc.path_,
      atlc.version_,
      given.program_,
      checked.info_.program_.struct_,
      checked.resolved_.indicators_,
    };
    request.kind_ = build::DriverKind::BACKTEST;
    request.store_ = checked.resolved_.store_;
    request.duckdb_ = checked.resolved_.duckdb_;
    const auto progress = [&output](const std::string& line) {
      output.progress(line);
    };
    if (auto failed = build::build_driver(request, scratch->path(), progress)) {
      return std::move(*failed);
    }
    if (interrupted()) {
      return interrupted_reply();
    }
    argv.push_back((scratch->path() / "driver").string());
  }
  for (const auto& [key, value] : checked.resolved_.args_) {
    argv.push_back(key + "=" + value);
  }
  std::string fills;
  if (invocation.has("fills")) {
    fills = std::filesystem::absolute(invocation.get("fills")).string();
    argv.push_back("output.fills=" + fills);
  }
  const auto ran = run_child(argv, [&output](const std::string& line) {
    output.progress(line);
  });
  json::object report;
  if (auto failed = replay_outcome(ran, report)) {
    return std::move(*failed);
  }
  Reply reply;
  reply.body_["report"] = std::move(report);
  reply.body_["spec"] = given.spec_.empty() ? json::value(nullptr) : json::value(given.spec_);
  reply.body_["program"] = given.program_;
  reply.body_["settings_given"] = settings_given(given);
  reply.body_["atlc"] = atlc.version_;
  if (!fills.empty()) {
    reply.body_["fills"] = fills;
  }
  return reply;
#endif
}

} // namespace cmdatl
