#pragma once

// The settings `strategy backtest` and `run start` read, in the order both read them: the words sorted into the
// program, the run spec and the settings, the run spec and the command line's settings checked key by key, the
// program's facts from `atlc check FILE --info`, and the settings checked against them.

#include "../core/reply.hpp"
#include "../core/runs.hpp"
#include "../core/table.hpp"
#include "../system/process.hpp"
#include "./spec.hpp"
#include "./strategy.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace cmdatl {

/**
 * @brief A command's words, `TARGET` and `SETTING` together, in order: the parser fills `TARGET` with the first word,
 *   which may be a setting, so the words are sorted again by `sort_words`.
 * @param invocation The arguments.
 * @return The words.
 */
inline std::vector<std::string> target_and_settings(const Invocation& invocation) {
  std::vector<std::string> words;
  for (const auto* name : {"TARGET", "SETTING"}) {
    const auto found = invocation.values_.find(name);
    if (found != invocation.values_.end()) {
      words.insert(words.end(), found->second.begin(), found->second.end());
    }
  }
  return words;
}

/** @brief What a command was given: its program and run spec, and the run spec's and the command line's settings. */
struct GivenSettings {
  /** @brief What is run, as given, for the first progress line: the program, the run spec, or `PROGRAM with SPEC`. */
  std::string target_;
  /** @brief The program (`.atls`) as given, or empty. */
  std::string program_given_;
  /** @brief The run spec as given, or empty. */
  std::string spec_given_;
  /** @brief The run spec, absolute, or empty when none is given. */
  std::string spec_;
  /** @brief The program, absolute and lexically normal: the one given, else the run spec's `strategy`. */
  std::string program_;
  /** @brief The run spec's settings, checked key by key. */
  std::vector<spec::Entry> file_;
  /** @brief The command line's settings, checked key by key. */
  std::vector<spec::Entry> line_;
  /** @brief The command line's settings as given. */
  std::vector<std::string> given_;
  /** @brief The command line's words that are settings, unread. */
  std::vector<std::string> words_;
};

/**
 * @brief Sorts a command's words into a program, a run spec and the settings: a word is a setting when the text before
 *   its first `=` is a dotted key (`spec::is_setting`); of the other words, one ending in `.atls` is the program and any
 *   other the run spec, at most one of each and at least one of the two.
 * @param words The words, in order.
 * @param given Set to the program, the run spec, the target's text and the settings' words.
 * @return A usage refusal naming the words when there is neither, two programs or two run specs, or nothing.
 */
inline std::optional<Reply> sort_words(const std::vector<std::string>& words, GivenSettings& given) {
  std::vector<std::string> programs;
  std::vector<std::string> specs;
  for (const auto& word : words) {
    if (spec::is_setting(word)) {
      given.words_.push_back(word);
    } else if (word.size() > 5 && word.compare(word.size() - 5, 5, ".atls") == 0) {
      programs.push_back(word);
    } else {
      specs.push_back(word);
    }
  }
  const std::string next = "cmdatl COMMAND --help shows its arguments";
  if (programs.empty() && specs.empty()) {
    return error_reply(
      Exit::USAGE, "usage.missing_argument",
      "no program or run spec among " + join(words) +
        ": give a program (.atls), a run spec, or both; KEY=VALUE words are settings",
      next
    );
  }
  for (const auto* list : {&programs, &specs}) {
    if (list->size() > 1) {
      return error_reply(
        Exit::USAGE, "usage.bad_value",
        std::string("at most one ") + (list == &programs ? "program (.atls)" : "run spec") + ", and " +
          join(*list, ", ") + " are " + std::to_string(list->size()) + "; a setting is KEY=VALUE with a dotted key",
        next
      );
    }
  }
  given.program_given_ = programs.empty() ? std::string() : programs.front();
  given.spec_given_ = specs.empty() ? std::string() : specs.front();
  given.target_ = given.program_given_.empty() ? given.spec_given_ : given.program_given_;
  if (!given.program_given_.empty() && !given.spec_given_.empty()) {
    given.target_ += " with " + given.spec_given_;
  }
  return std::nullopt;
}

/**
 * @brief The refusal of settings errors: `spec.invalid` (exit 2), the first error's message, every error in
 *   `errors`, and a `next` naming the run spec or the settings given.
 * @param errors The errors.
 * @param spec The run spec, or empty.
 * @return The reply.
 */
inline Reply settings_refusal(const std::vector<spec::Error>& errors, const std::string& spec) {
  json::array list;
  for (const auto& e : errors) {
    list.push_back(e.to_json());
  }
  auto reply = error_reply(
    Exit::USAGE, "spec.invalid", first_error(list, "the settings have errors"),
    spec.empty() ?
      std::string("fix the settings given on the command line") :
      "fix the run spec " + spec + " or the settings given on the command line"
  );
  reply.body_["errors"] = std::move(list);
  return reply;
}

/**
 * @brief Reads the settings a command was given, key by key: the run spec (when one is given) and the command line's,
 *   a key given twice on the command line refused; the program given, else the one the run spec's `strategy` names.
 *   A run spec given with a program may name it too, and then only the same file (`spec::same_path`).
 * @param given The program, the run spec and the settings' words, from `sort_words`; filled with the rest.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> read_given(GivenSettings& given) {
  std::vector<spec::Error> errors;
  std::error_code ec;
  const auto absolute = [&ec](const std::string& path) {
    auto made = std::filesystem::absolute(path, ec);
    return (ec ? std::filesystem::path(path) : made).lexically_normal().string();
  };
  if (!given.program_given_.empty()) {
    given.program_ = absolute(given.program_given_);
  }
  if (!given.spec_given_.empty()) {
    given.spec_ = absolute(given.spec_given_);
    std::string named;
    auto read = spec::read_file(given.spec_, named);
    errors = std::move(read.errors_);
    given.file_ = std::move(read.entries_);
    if (!errors.empty() && errors.front().code_ == "unreadable" && given.spec_given_.find('=') != std::string::npos) {
      // A mistyped setting (`lookback=30`) is read as the run spec: say what a setting is.
      errors.front().message_ += "; a setting is KEY=VALUE with a dotted key";
    }
    if (errors.empty() && given.program_.empty()) {
      if (named.empty()) {
        errors.push_back(spec::Error::file(
          given.spec_, "no `strategy = \"FILE.atls\"`: a spec given without a program names the program it runs"
        ));
      } else {
        given.program_ = std::filesystem::path(named).lexically_normal().string();
      }
    } else if (errors.empty() && !named.empty() && !spec::same_path(named, given.program_)) {
      const auto strategy = std::find_if(given.file_.begin(), given.file_.end(), [](const spec::Entry& e) {
        return e.key_ == "strategy";
      });
      const auto message =
        "`strategy` names " + named + ", another program than " + given.program_ + ": remove it, or give that one";
      errors.push_back(
        strategy == given.file_.end() ? spec::Error::file(given.spec_, message) :
                                        spec::Error::at(strategy->origin_, message)
      );
    }
  }
  std::vector<std::string> keys;
  for (const auto& word : given.words_) {
    given.given_.push_back(word);
    spec::Entry entry;
    if (auto error = spec::from_command_line(word, entry)) {
      errors.push_back(std::move(*error));
      continue;
    }
    if (std::find(keys.begin(), keys.end(), entry.key_) != keys.end()) {
      errors.push_back(spec::Error::at(entry.origin_, "`" + entry.key_ + "` is given twice on the command line"));
      continue;
    }
    keys.push_back(entry.key_);
    if (auto error = spec::check(entry)) {
      errors.push_back(std::move(*error));
      continue;
    }
    given.line_.push_back(std::move(entry));
  }
  if (!errors.empty()) {
    return settings_refusal(errors, given.spec_);
  }
  return std::nullopt;
}

/**
 * @brief The program's facts: `atlc check PROGRAM --info`, its one JSON object checked (`spec::read_info`); its
 *   warnings go on as progress lines.
 * @param atlc atlc's path.
 * @param program The program, absolute.
 * @param output Where progress goes.
 * @param info Set to the facts.
 * @return The refusal (the program's errors, an atlc without `--info`, or an answer cmdatl cannot use), or nothing.
 */
inline std::optional<Reply> program_info(
  const std::string& atlc, const std::string& program, Output& output, spec::Info& info
) {
  const auto forward = [&output](const std::string& line) {
    output.progress(line);
  };
  const auto result = run_child({atlc, "check", program, "--info"}, forward);
  if (result.kind_ == ChildResult::Kind::INTERRUPTED) {
    return interrupted_reply();
  }
  const std::string check = "check atlc with cmdatl env";
  if (result.kind_ != ChildResult::Kind::FINISHED) {
    return atlc_failure(Exit::FAILED, "atlc.failed", "atlc check: " + ending_of(result), result, check);
  }
  const auto read = result.signaled_ ? std::nullopt : spec::read_info(result.stdout_, program);
  if (!read) {
    // An atlc without `--info` answers it as any unknown option: exit 2 and its usage on stderr.
    const bool usage = std::any_of(result.stderr_tail_.begin(), result.stderr_tail_.end(), [](const std::string& line) {
      return line.rfind("usage: atlc", 0) == 0;
    });
    if (!result.signaled_ && result.code_ == 2 && result.stdout_.find('{') == std::string::npos && usage) {
      return atlc_failure(
        Exit::FAILED, "atlc.unsupported", "this atlc has no atlc check --info: it is older than the atlc cmdatl reads",
        result, "install a newer atlc"
      );
    }
    return atlc_failure(
      Exit::FAILED, "atlc.failed", "atlc check: " + ending_of(result) + " without a valid --info reply", result, check
    );
  }
  json::array errors;
  bool usage = false;
  bool file = false;
  for (const auto& d : read->diagnostics_) {
    const auto& object = d.as_object();
    if (object.at("severity").as_string() != "error") {
      const auto place = object.at("file").is_string() ?
        std::string(object.at("file").as_string()) +
          (object.at("line").is_int64() ? ":" + std::to_string(object.at("line").as_int64()) : std::string()) + ": " :
        std::string();
      output.progress(
        place + "warning[" + std::string(object.at("code").as_string()) +
        "]: " + std::string(object.at("message").as_string())
      );
      continue;
    }
    const auto& code = object.at("code").as_string();
    usage = usage || code == "usage";
    file = file || code == "unreadable" || code == "program-name";
    errors.push_back(d);
  }
  if (result.code_ == 0 && read->ok_) {
    info = *read;
    return std::nullopt;
  }
  const bool program_errors = result.code_ == 1 && !read->ok_ && !usage && !file;
  const bool file_errors = result.code_ == 2 && !read->ok_ && file && !usage;
  if (program_errors || file_errors) {
    auto reply = error_reply(
      Exit::PROGRAM, "program.invalid", first_error(errors, "the program has errors"),
      file_errors ?
        std::string("name a program file that exists, named so it can be a C++ struct (a letter first)") :
        std::string("fix the program; atlc check FILE.atls checks it alone")
    );
    reply.body_["errors"] = std::move(errors);
    return reply;
  }
  if (result.code_ == 2 && usage) {
    return atlc_failure(
      Exit::FAILED, "atlc.failed", "atlc check refused cmdatl's arguments: " + first_error(errors, "a usage error"),
      result, check
    );
  }
  return atlc_failure(
    Exit::FAILED, "atlc.failed", "atlc check: " + ending_of(result) + " with a contradictory --info reply", result,
    check
  );
}

/** @brief The settings checked against their program. */
struct CheckedSettings {
  /** @brief The program's facts. */
  spec::Info info_;
  /** @brief The build's settings and the driver's arguments. */
  spec::Resolved resolved_;
};

/**
 * @brief Checks the settings given against the program's facts: the program asked of atlc, then (for a run) a
 *   program with session legs refused as the driver refuses it, then every rule that reads the program.
 * @param given The settings, read by `read_given`.
 * @param atlc atlc's path.
 * @param output Where progress goes.
 * @param run Whether the settings are a run's (`run start`).
 * @param checked Set to the result.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> check_given(
  const GivenSettings& given, const std::string& atlc, Output& output, bool run, CheckedSettings& checked
) {
  if (auto refused = program_info(atlc, given.program_, output, checked.info_)) {
    return refused;
  }
  if (interrupted()) {
    return interrupted_reply();
  }
  // No live source delivers session legs: the driver's own refusal, before the settings' rules ask for legs.
  if (run && checked.info_.program_.legs_) {
    auto reply = error_reply(Exit::PROGRAM, "program.live_unsupported", LEGS_UNSUPPORTED, LEGS_UNSUPPORTED_NEXT);
    reply.body_["unsupported"] = "legs";
    return reply;
  }
  const auto errors = spec::resolve({given.file_, given.line_}, checked.info_.program_, checked.resolved_);
  if (!errors.empty()) {
    return settings_refusal(errors, given.spec_);
  }
  return std::nullopt;
}

/**
 * @brief The command line's settings as given, for a reply.
 * @param given The settings.
 * @return The array.
 */
inline json::array settings_given(const GivenSettings& given) {
  json::array out;
  for (const auto& word : given.given_) {
    out.push_back(json::value(word));
  }
  return out;
}

} // namespace cmdatl
