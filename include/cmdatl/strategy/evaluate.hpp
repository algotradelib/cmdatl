#pragma once

// `strategy evaluate`: the settings read and checked as a backtest's, with `[evaluate]` and `[grid]`, the backtest's
// driver built (a backtest's build serves it, a fetch included), run with `evaluate.method=grid`, and each of its JSON
// lines emitted as a `replay` event before the final reply, which carries the driver's summary (and the fetch's).

#include "../system/terminal.hpp"
#include "./backtest.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cmdatl {

/** @brief The summary's keys the final reply carries; any other key of the driver's summary is dropped. */
inline const std::vector<std::string> EVALUATE_SUMMARY{
  "method", "jobs", "param_sets", "invalid", "tried", "replays", "paths", "failed", "objective", "direction", "best",
  // windows and bootstrap
  "scheme", "length", "step", "warmup", "windows", "uncovered_from", "halted", "summary", "block", "join", "seed",
  "history", "warnings", "intervals",
  // walk-forward
  "train", "test", "train_replays", "folds", "chained", "chained_null_reason"
};

/** @brief The codes a driver's stop line may give the reply; any other stop is `replay.failed`. */
inline const std::vector<std::string> EVALUATE_STOP_CODES{"replay.failed", "evaluate.refused"};

/** @brief An evaluation's driver output, read a line at a time: replay lines emitted, the summary or stop kept. */
class EvaluationLines {
public:
  /**
   * @brief Reads into an output.
   * @param output Where replay lines are emitted.
   */
  explicit EvaluationLines(Output& output) : output_(output) {}

  /**
   * @brief Takes one line: a replay (emitted with `event: replay`, its index the next), the summary or a stop line;
   *   anything else, or anything after the summary or stop, breaks the protocol.
   * @param line The line.
   * @throws std::runtime_error When the line could not be written (which ends the driver).
   */
  void take(const std::string& line) {
    boost::system::error_code error;
    auto value = json::parse(line, error);
    if (broken_ || error || !value.is_object() || summary_ || stopped_) {
      broken_ = broken_ ?
        broken_ :
        (error || !value.is_object()) ?
        "a line that is not a JSON object" :
        "a line after its end";
      return;
    }
    auto& object = value.as_object();
    if (const auto* index = object.if_contains("index")) {
      if (!index->is_int64() || index->as_int64() != static_cast<std::int64_t>(replays_)) {
        broken_ = "replay " + json::serialize(*index) + " where " + std::to_string(replays_) + " was next";
        return;
      }
      ++replays_;
      const auto* failure = object.if_contains("failure");
      if (failure == nullptr) {
        ++reported_;
      } else if (const auto* message = failure->is_object() ? failure->as_object().if_contains("message") : nullptr) {
        first_failure_ =
          first_failure_.empty() && message->is_string() ? std::string(message->as_string()) : first_failure_;
      }
      json::object event{{"event", "replay"}};
      for (const auto& field : object) {
        event[field.key()] = field.value();
      }
      output_.emit(event);
    } else if (object.contains("summary") && object.at("summary").is_object()) {
      summary_ = object.at("summary").as_object();
    } else if (object.contains("stopped") && object.at("stopped").is_object()) {
      stopped_ = object.at("stopped").as_object();
    } else {
      broken_ = "a line of no known kind";
    }
  }

  /**
   * @brief The final reply from the driver's end and what it printed.
   * @param ran The driver's end.
   * @param given The settings.
   * @param atlc The atlc found.
   * @return The reply.
   */
  Reply reply(const ChildResult& ran, const GivenSettings& given, const Atlc& atlc) const {
    const std::string next = "check the data and the parameters' bounds";
    Reply reply;
    if (ran.kind_ == ChildResult::Kind::INTERRUPTED) {
      reply = interrupted_reply("the evaluation");
    } else if (broken_) {
      reply = atlc_failure(
        Exit::FAILED, "replay.failed", "the evaluation driver broke its line protocol: " + *broken_, ran, next
      );
    } else if (stopped_ && ran.kind_ == ChildResult::Kind::FINISHED && !ran.signaled_ && ran.code_ == 1) {
      const auto code = stopped_->contains("code") && stopped_->at("code").is_string() ?
        std::string(stopped_->at("code").as_string()) :
        "";
      const bool declared =
        std::find(EVALUATE_STOP_CODES.begin(), EVALUATE_STOP_CODES.end(), code) != EVALUATE_STOP_CODES.end();
      const auto message = stopped_->contains("message") && stopped_->at("message").is_string() ?
        std::string(stopped_->at("message").as_string()) :
        last_line(ran);
      // A refusal before any replay is the settings': exit 2, with the driver's next naming the key.
      const bool refused = declared && code == "evaluate.refused";
      const auto* asked = stopped_->if_contains("next");
      reply = atlc_failure(
        refused ? Exit::USAGE : Exit::FAILED, declared ? code : "replay.failed", "the evaluation stopped: " + message,
        ran, refused && asked != nullptr && asked->is_string() ? std::string(asked->as_string()) : next
      );
    } else if (ran.kind_ != ChildResult::Kind::FINISHED || ran.signaled_ || ran.code_ != 0 || !summary_) {
      const auto ending = ran.kind_ == ChildResult::Kind::FINISHED && !ran.signaled_ && ran.code_ == 1 ?
        last_line(ran) :
        ran.kind_ == ChildResult::Kind::FINISHED && !ran.signaled_ && ran.code_ == 0 ?
        std::string("the evaluation driver exited 0 without its summary") :
        ending_of(ran, "the evaluation driver");
      reply = atlc_failure(Exit::FAILED, "replay.failed", "the evaluation failed: " + ending, ran, next);
    } else if (
      !summary_->contains("replays") || summary_->at("replays") != json::value(static_cast<std::int64_t>(replays_))
    ) {
      reply = atlc_failure(
        Exit::FAILED, "replay.failed", "the evaluation driver's summary disagrees with its lines", ran, next
      );
    } else if (reported_ == 0) {
      reply =
        atlc_failure(Exit::FAILED, "replay.failed", "every replay failed or was refused: " + first_failure_, ran, next);
    }
    if (is_failure(reply)) {
      reply.body_["replays"] = static_cast<std::int64_t>(replays_);
      return reply;
    }
    for (const auto& key : EVALUATE_SUMMARY) {
      if (const auto* value = summary_->if_contains(key)) {
        reply.body_[key] = *value;
      }
    }
    reply.body_["spec"] = given.spec_.empty() ? json::value(nullptr) : json::value(given.spec_);
    reply.body_["program"] = given.program_;
    reply.body_["settings_given"] = settings_given(given);
    reply.body_["atlc"] = atlc.version_;
    return reply;
  }

private:
  Output& output_;
  std::size_t replays_{0};
  std::size_t reported_{0};
  std::string first_failure_;
  std::optional<std::string> broken_;
  std::optional<json::object> summary_;
  std::optional<json::object> stopped_;
};

/**
 * @brief `strategy evaluate TARGET [KEY=VALUE]...`; see the file comment.
 * @param invocation The arguments.
 * @param output Where progress and each replay's line go.
 * @return The reply.
 */
inline Reply evaluate_reply(const Invocation& invocation, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)output;
  return error_reply(
    Exit::FAILED, "unavailable.platform", "strategy evaluate builds and runs a driver on macOS and Linux only"
  );
#else
  GivenSettings given;
  if (auto refused = sort_words(target_and_settings(invocation), given)) {
    return std::move(*refused);
  }
  output.progress("cmdatl: evaluating " + given.target_);
  Atlc atlc;
  CheckedSettings checked;
  if (auto refused = checked_settings(given, output, spec::Purpose::EVALUATE, atlc, checked)) {
    return std::move(*refused);
  }
  // The driver reads stdin in its own process group, where a terminal stops it (SIGTTIN): refused before the build.
  const auto& args = checked.resolved_.args_;
  const bool from_stdin =
    std::find(args.begin(), args.end(), std::make_pair(std::string("bars.source"), std::string("stdin"))) != args.end();
  if (from_stdin && sys::is_terminal(STDIN_FILENO)) {
    return error_reply(
      Exit::USAGE, "usage.bad_value", "bars.source = stdin reads a generator's CSV, and stdin is a terminal",
      "pipe the rows in: GENERATOR | cmdatl strategy evaluate ..."
    );
  }
  const auto& fetching = checked.resolved_.fetch_source_;
  Fetch fetch;
  if (auto refused = prepare_fetch(given, checked, output, fetch)) {
    return std::move(*refused);
  }
  std::vector<std::string> argv;
  std::optional<ScratchCopy> scratch;
  if (auto failed = driver_argv(given, checked, atlc, fetch, output, scratch, argv)) {
    return std::move(*failed);
  }
  const auto outcome = (scratch ? scratch->path() : std::filesystem::temp_directory_path()) / "fetch.json";
  StagedLogin staged;
  if (!fetching.empty()) {
    argv.insert(argv.end(), fetch.args_.begin(), fetch.args_.end());
    argv.push_back("fetch.outcome=" + outcome.string());
    if (const int error = staged.stage(fetch.login_)) {
      return error_reply(
        Exit::FAILED, "replay.failed",
        "the login could not be handed to the driver: " + std::string(std::strerror(error))
      );
    }
  }
  EvaluationLines lines(output);
  const auto ran = run_child(
    argv,
    [&output](const std::string& line) {
      output.progress(line);
    },
    nullptr, staged.fd(),
    [&lines](const std::string& line) {
      lines.take(line);
    },
    from_stdin ? ChildInput::INHERITED : ChildInput::NUL
  );
  if (fetching.empty()) {
    return lines.reply(ran, given, atlc);
  }
  auto fetched = read_fetch_outcome(outcome);
  if (auto failed = fetch_failure(ran, fetched, fetching)) {
    return std::move(*failed);
  }
  auto reply = lines.reply(ran, given, atlc);
  // What was fetched belongs to the evaluation's result; a failed replay's reply carries its own diagnosis only.
  if (fetched && !is_failure(reply)) {
    (*fetched)["caveat"] = fetched_caveat(fetch);
    reply.body_["fetched"] = std::move(*fetched);
  }
  return reply;
#endif
}

} // namespace cmdatl
