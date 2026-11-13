#pragma once

#include "../account/account.hpp"
#include "../account/operator.hpp"
#include "./command_list.hpp"
#include "./describe.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace cmdatl {

/**
 * @brief The command path on a KIS account (`kis.hpp`), handed in by `main`, so that only `main` compiles the KIS
 *   client; without it a KIS account answers `unsupported.broker`.
 */
using BrokerPath = std::function<Reply(const Command&, const Invocation&, const ConfigSnapshot&, Output&)>;

/** @brief The options every command takes, taken out of the command line before the command is parsed. */
struct Globals {
  /** @brief How to write the reply. */
  Rendering rendering_;
  /** @brief Whether `--format` or `--json` chose the format (otherwise the terminal decides). */
  bool format_given_{false};
  /** @brief `-h` or `--help`. */
  bool help_{false};
  /** @brief `--version`. */
  bool version_{false};
};

/**
 * @brief Takes the global options out of the command line.
 * @param args The command line, without the program's name.
 * @param globals Where to put what was found.
 * @param rest The remaining words, in order.
 * @return An error reply when a global option is malformed.
 */
inline std::optional<Reply> take_globals(
  const std::vector<std::string>& args, Globals& globals, std::vector<std::string>& rest
) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--") {
      // Everything after `--` is the command's, as written (`cmdatl schema -- --help`).
      rest.insert(rest.end(), args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
      break;
    }
    const auto eq = arg.find('=');
    const auto name = arg.substr(0, eq);
    // A value is the next word unless that word is an option; `--option=VALUE` takes any value.
    const auto value_of = [&](std::string& value) -> bool {
      if (eq != std::string::npos) {
        value = arg.substr(eq + 1);
        return true;
      }
      if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1].front() == '-')) {
        return false;
      }
      value = args[++i];
      return true;
    };
    std::string value;
    if (arg == "--json") {
      globals.rendering_.format_ = Format::JSON;
      globals.format_given_ = true;
    } else if (arg == "-h" || arg == "--help") {
      globals.help_ = true;
    } else if (arg == "--version") {
      globals.version_ = true;
    } else if (name == "--format") {
      if (!value_of(value) || (value != "json" && value != "jsonl" && value != "text")) {
        return error_reply(Exit::USAGE, "usage.bad_format", "--format is json, jsonl or text");
      }
      globals.rendering_.format_ = value == "text" ? Format::TEXT : (value == "jsonl" ? Format::JSONL : Format::JSON);
      globals.format_given_ = true;
    } else if (name == "--fields") {
      if (!value_of(value) || value.empty()) {
        return error_reply(Exit::USAGE, "usage.bad_option", "--fields takes a,b.c");
      }
      globals.rendering_.fields_ = split(value, ',');
    } else if (name == "--value") {
      if (!value_of(value) || value.empty()) {
        return error_reply(Exit::USAGE, "usage.bad_option", "--value takes a field path such as cash");
      }
      globals.rendering_.value_ = value;
    } else {
      rest.push_back(arg);
    }
  }
  return std::nullopt;
}

/**
 * @brief Parses a command's own arguments and options with CLI11, from its entry in the table.
 * @param command The command.
 * @param words Its arguments and options, after its path.
 * @param invocation Where to put the values.
 * @return An error reply when they do not fit the command.
 */
inline std::optional<Reply> parse_command(
  const Command& command, const std::vector<std::string>& words, Invocation& invocation
) {
  CLI::App app{command.summary_, "cmdatl " + join(command.path_)};
  app.set_help_flag();
  app.allow_extras(false);
  std::map<std::string, std::vector<std::string>> values;
  std::map<std::string, bool> flags;
  for (const auto& arg : command.args_) {
    auto* added = app.add_option(arg.name_, values[arg.name_], arg.help_);
    if (arg.variadic_) {
      added->expected(arg.required_ ? 1 : 0, -1);
    } else {
      // One word, never the next: those are a following variadic argument's (`TARGET SETTING...`).
      added->expected(1)->allow_extra_args(false);
    }
    if (arg.required_) {
      added->required();
    }
  }
  for (const auto& option : command.options_) {
    const auto names = (option.short_name_.empty() ? "" : "-" + option.short_name_ + ",") + "--" + option.name_;
    if (option.value_name_.empty()) {
      app.add_flag(names, flags[option.name_], option.help_);
      continue;
    }
    auto* added = app.add_option(names, values[option.name_], option.help_);
    added->type_name(option.value_name_);
    if (option.repeated_) {
      // One value per occurrence: `--name a --name b`, never `--name a b`.
      added->expected(1, -1)->allow_extra_args(false);
    } else {
      // One value, never the words after it: those are a variadic positional's (`TARGET --fills F KEY=VALUE`).
      added->expected(1)->allow_extra_args(false);
    }
    if (option.required_) {
      added->required();
    }
  }
  std::vector<std::string> reversed(words.rbegin(), words.rend());
  try {
    app.parse(reversed);
  } catch (const CLI::RequiredError& e) {
    return error_reply(Exit::USAGE, "usage.missing_argument", e.what(), "cmdatl " + join(command.path_) + " --help");
  } catch (const CLI::ParseError& e) {
    return error_reply(Exit::USAGE, "usage.bad_option", e.what(), "cmdatl " + join(command.path_) + " --help");
  }
  for (auto& entry : values) {
    if (!entry.second.empty()) {
      invocation.values_[entry.first] = std::move(entry.second);
    }
  }
  for (const auto& entry : flags) {
    if (entry.second) {
      invocation.values_[entry.first] = {"true"};
    }
  }
  return std::nullopt;
}

/**
 * @brief Whether a field path can name something in the command's replies: its declared fields, and the ones
 *   every reply may carry.
 * @param command The command.
 * @param path The path (`cash`, `positions.0.quantity`).
 * @return Whether its first word is a field the command replies with.
 */
inline bool declares_field(const Command& command, const std::string& path) {
  const auto head = path.substr(0, path.find('.'));
  if (head == "env" || head == "result" || head == "warning" || head == "note" || head == "error") {
    return true;
  }
  return std::any_of(command.returns_.begin(), command.returns_.end(), [&head](const auto& field) {
    return field.first == head;
  });
}

/** @brief What a command line asks for, once it is read and checked, before anything runs. */
struct Prepared {
  /** @brief What to do. */
  enum class Action {
    HELP,
    VERSION,
    RUN,
    REFUSE,
  };
  /** @brief What to do. */
  Action action_{Action::REFUSE};
  /** @brief The global options. */
  Globals globals_;
  /** @brief The command line's leading words (the command's path, and for help, a group's). */
  std::vector<std::string> words_;
  /** @brief The command they named. */
  Resolution found_;
  /** @brief The command's arguments and options. */
  Invocation invocation_;
  /** @brief Why the line is refused, when it is. */
  std::optional<Reply> refusal_;
};

/**
 * @brief Reads and checks a command line without running anything: the one place both dispatch and the
 *   documentation check validate an invocation.
 * @param args The command line, without the program's name.
 * @param table The commands.
 * @param terminal Whether output goes to a terminal (text by default) or not (compact JSON by default).
 * @return What it asks for.
 */
inline Prepared prepare(const std::vector<std::string>& args, const Table& table, bool terminal) {
  Prepared prepared;
  auto& globals = prepared.globals_;
  globals.rendering_.format_ = terminal ? Format::TEXT : Format::JSON;
  std::vector<std::string> rest;
  if (auto bad = take_globals(args, globals, rest)) {
    // clig.dev: -h works at the end of anything, even beside a malformed option (before `--`).
    const auto end = std::find(args.begin(), args.end(), "--");
    if (std::find(args.begin(), end, "--help") != end || std::find(args.begin(), end, "-h") != end) {
      for (const auto& word : args) {
        if (word.empty() || word.front() == '-') {
          break;
        }
        prepared.words_.push_back(word);
      }
      prepared.found_ = table.resolve(prepared.words_);
      prepared.action_ = Prepared::Action::HELP;
      return prepared;
    }
    prepared.refusal_ = std::move(bad);
    return prepared;
  }
  if (globals.version_) {
    prepared.action_ = Prepared::Action::VERSION;
    return prepared;
  }
  for (const auto& word : rest) {
    if (!word.empty() && word.front() == '-') {
      break;
    }
    prepared.words_.push_back(word);
  }
  const auto& words = prepared.words_;
  prepared.found_ = table.resolve(words);
  if (globals.help_ || rest.empty()) {
    prepared.action_ = Prepared::Action::HELP;
    return prepared;
  }
  if (prepared.found_.command_ == nullptr) {
    if (table.is_group(words)) {
      prepared.refusal_ = error_reply(
        Exit::USAGE, "usage.unknown_command", "cmdatl " + join(words) + " needs a command",
        "cmdatl " + join(words) + " --help"
      );
      return prepared;
    }
    const auto suggestions = table.suggest(words);
    prepared.refusal_ = error_reply(
      Exit::USAGE, "usage.unknown_command", "no command " + join(words),
      suggestions.empty() ? "cmdatl --help lists the commands" : "did you mean: " + join(suggestions, ", ")
    );
    return prepared;
  }
  const auto& command = *prepared.found_.command_;
  const auto command_usage = usage(command);
  const auto usage_line = "cmdatl " + join(command.path_) + (command_usage.empty() ? "" : " " + command_usage);
  const std::vector<std::string> own(rest.begin() + static_cast<std::ptrdiff_t>(prepared.found_.used_), rest.end());
  if (auto bad = parse_command(command, own, prepared.invocation_)) {
    // clig.dev: on a usage error, show the command's usage, not only the error.
    bad->body_["usage"] = usage_line;
    prepared.refusal_ = std::move(bad);
    return prepared;
  }
  // A field no reply of this command can have is a usage error before the command runs, so a typo never stands
  // between a caller and an outcome (a field missing only after it ran is reported beside the whole reply).
  std::vector<std::string> asked = globals.rendering_.fields_;
  if (globals.rendering_.value_) {
    asked.push_back(*globals.rendering_.value_);
  }
  // `run` becomes its driver, which prints every event and its whole reply: there is nothing to select from.
  if (!asked.empty() && command.run_config_) {
    prepared.refusal_ = error_reply(
      Exit::USAGE, "usage.bad_value",
      "cmdatl " + join(command.path_) + " prints every event and its whole reply; --fields and --value do not apply",
      "run it without --fields and --value, and read the last line"
    );
    prepared.refusal_->body_["usage"] = usage_line;
    return prepared;
  }
  for (const auto& field : asked) {
    if (!declares_field(command, field)) {
      prepared.refusal_ = error_reply(
        Exit::USAGE, "usage.no_such_field", "cmdatl " + join(command.path_) + " replies have no field " + field,
        "cmdatl schema " + join(command.path_, ".") + " lists its fields"
      );
      prepared.refusal_->body_["usage"] = usage_line;
      return prepared;
    }
  }
  prepared.action_ = Prepared::Action::RUN;
  return prepared;
}

/**
 * @brief The last resort after a command ran and its reply could not be written: one fixed line on stderr, and the
 *   exit code that does not overstate what is known.
 * @param err stderr.
 * @param may_send Whether the command may have sent.
 * @param hint What to read first: empty for a sending command (`cmdatl order reconcile`); for a command that changes
 *   the account without sending, its hint; empty otherwise.
 * @return 4 (unknown) for a command that may have sent or changed the account, 1 otherwise.
 */
inline int report_lost_outcome(std::ostream& err, bool may_send, const std::string& hint = "") noexcept {
  try {
    if (may_send) {
      err << "cmdatl: the command ran but its reply could not be written; its outcome is unknown: ";
      err << "run " << (hint.empty() ? std::string("cmdatl order reconcile") : hint) << " before anything else\n";
    } else if (!hint.empty()) {
      err << "cmdatl: the command changed the account but its reply could not be written; its outcome is unknown: ";
      err << hint << "\n";
    } else {
      err << "cmdatl: the command ran but its reply could not be written\n";
    }
    err.flush();
  } catch (...) {
    // stderr itself failed: there is nowhere left to report to, and the exit code below still says what is known.
  }
  return may_send || !hint.empty() ? static_cast<int>(Exit::UNKNOWN) : static_cast<int>(Exit::FAILED);
}

/**
 * @brief The environment an account's replies carry, from its broker's session, never from the file: the one place
 *   a broker's name is mapped.
 * @param config The operator's file.
 * @return The environment, such as `mock`.
 */
inline std::string account_env(const OperatorConfig& config) {
  if (config.broker_ == AccountDesk::broker_name) {
    return AccountDesk::env_name;
  }
  // A broker's account, as its run names it: `kis-demo` or `kis-real`.
  return config.broker_ + "-" + config.env_;
}

/**
 * @brief The command and its positional arguments, for the owner record.
 * @param command The command.
 * @param invocation Its arguments.
 * @return The text, at most 200 bytes.
 */
inline std::string owner_what(const Command& command, const Invocation& invocation) {
  auto what = join(command.path_);
  for (const auto& arg : command.args_) {
    const auto found = invocation.values_.find(arg.name_);
    if (found != invocation.values_.end()) {
      for (const auto& value : found->second) {
        what += " " + value;
      }
    }
  }
  return what.substr(0, 200);
}

/**
 * @brief An account command on its open account: the account opened (its lock held for the whole command), the
 *   owner record written once it is open beneath an intact journal (the lock checked first), the handler, and the
 *   lock checked once more before the reply.
 * @param command The command.
 * @param invocation Its arguments.
 * @param snapshot The operator's file, read and `OK`.
 * @param output The output, carrying the account's environment.
 * @param kis The command path on a KIS account, or empty.
 * @return The reply.
 */
inline Reply account_command_reply(
  const Command& command, const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output,
  const BrokerPath& kis = {}
) {
#if defined(_WIN32)
  (void)command;
  (void)invocation;
  (void)snapshot;
  (void)output;
  (void)kis;
  return error_reply(
    Exit::FAILED, "unavailable.platform", "account commands run on macOS and Linux only in this version"
  );
#else
  if (snapshot.config_->broker_ == "kis" && kis) {
    return kis(command, invocation, snapshot, output);
  }
  auto opened = Account::open(snapshot, output, [] {
    return interrupted();
  });
  if (opened.refusal_) {
    return std::move(*opened.refusal_);
  }
  auto& account = *opened.account_;
  // Only beneath this account's intact journal: a journal not established leaves every file as it was.
  if (account.journal_intact() && !write_owner_record(account.lock(), owner_what(command, invocation))) {
    return lock_lost_reply();
  }
  Reply reply = command.run_account_(invocation, account, output);
  if (!account.lock().still_at_path()) {
    reply = lock_lost_reply();
  }
  return reply;
#endif
}

/**
 * @brief Runs an account command: its arguments checked, the operator's file read once, the reply's environment
 *   taken from the broker, then the account opened and the command run (`account_command_reply`).
 * @param command The command.
 * @param prepared The command line, read and checked.
 * @param out stdout.
 * @param err stderr.
 * @param kis The command path on a KIS account, or empty.
 * @return The exit code.
 */
inline int run_on_account(
  const Command& command, const Prepared& prepared, std::ostream& out, std::ostream& err, const BrokerPath& kis
) {
  const auto& rendering = prepared.globals_.rendering_;
  const bool unknown_if_lost = command.may_send_ || command.mutates_;
  const auto hint = command.may_send_ ? std::string() : command.mutates_hint_;
  // Before the account is opened nothing can have changed: a reply that cannot be written is a plain failure.
  const auto offline = [&](const Reply& reply) {
    try {
      Output output(out, err, rendering, "offline");
      return static_cast<int>(output.finish(reply));
    } catch (...) {
      return report_lost_outcome(err, false);
    }
  };
#if defined(_WIN32)
  return offline(
    error_reply(Exit::FAILED, "unavailable.platform", "account commands run on macOS and Linux only in this version")
  );
#else
  std::optional<ConfigSnapshot> snapshot;
  try {
    if (command.check_account_) {
      if (auto refused = command.check_account_(prepared.invocation_)) {
        refused->body_["usage"] = "cmdatl " + join(command.path_) + " " + usage(command);
        return offline(*refused);
      }
    }
    snapshot = load_operator_config();
  } catch (const std::exception& e) {
    return offline(error_reply(Exit::FAILED, "internal.error", e.what(), "report the defect with the command line"));
  }
  if (snapshot->state_ != ConfigSnapshot::State::OK) {
    return offline(config_reply(*snapshot));
  }
  std::string env;
  try {
    env = account_env(*snapshot->config_);
  } catch (const std::exception& e) {
    return offline(error_reply(Exit::FAILED, "internal.error", e.what(), "report the defect with the command line"));
  }
  try {
    Output output(out, err, rendering, env);
    Reply reply;
    try {
      reply = account_command_reply(command, prepared.invocation_, *snapshot, output, kis);
    } catch (const std::exception& e) {
      reply = unknown_if_lost ?
        error_reply(Exit::UNKNOWN, "unknown.internal", e.what(), hint.empty() ? "cmdatl order reconcile" : hint) :
        error_reply(Exit::FAILED, "internal.error", e.what(), "report the defect with the command line");
    } catch (...) {
      reply = unknown_if_lost ?
        error_reply(
          Exit::UNKNOWN, "unknown.internal", "an unknown exception", hint.empty() ? "cmdatl order reconcile" : hint
        ) :
        error_reply(Exit::FAILED, "internal.error", "an unknown exception", "report the defect with the command line");
    }
    if (prepared.found_.alias_) {
      reply.body_["note"] = "cmdatl " + join(*prepared.found_.alias_) + " is now cmdatl " + join(command.path_);
    }
    return static_cast<int>(output.finish(reply));
  } catch (...) {
    return report_lost_outcome(err, command.may_send_, unknown_if_lost ? hint : std::string());
  }
#endif
}

/**
 * @brief Runs a command that names the account but opens none: the operator's file read once, the reply's environment
 *   taken from the broker, then the handler with the file read; the handler reports its own outcomes, and an exception
 *   is unknown (exit 4) for a command that may send.
 * @param command The command.
 * @param prepared The command line, read and checked.
 * @param out stdout.
 * @param err stderr.
 * @return The exit code.
 */
inline int run_on_config(const Command& command, const Prepared& prepared, std::ostream& out, std::ostream& err) {
  const auto& rendering = prepared.globals_.rendering_;
  const auto offline = [&](const Reply& reply) {
    try {
      Output output(out, err, rendering, "offline");
      return static_cast<int>(output.finish(reply));
    } catch (...) {
      return report_lost_outcome(err, false);
    }
  };
#if defined(_WIN32)
  return offline(error_reply(Exit::FAILED, "unavailable.platform", "runs run on macOS and Linux only in this version"));
#else
  std::optional<ConfigSnapshot> snapshot;
  std::string env;
  try {
    snapshot = load_operator_config();
    if (snapshot->state_ != ConfigSnapshot::State::OK) {
      return offline(config_reply(*snapshot));
    }
    env = account_env(*snapshot->config_);
  } catch (const std::exception& e) {
    return offline(error_reply(Exit::FAILED, "internal.error", e.what(), "report the defect with the command line"));
  }
  try {
    Output output(out, err, rendering, env);
    Reply reply;
    // `run` returns only when its driver did not start: whatever happens here, nothing was sent.
    try {
      reply = command.run_config_(prepared.invocation_, *snapshot, output);
    } catch (const std::exception& e) {
      reply = error_reply(Exit::FAILED, "internal.error", e.what(), "report the defect with the command line");
    } catch (...) {
      reply =
        error_reply(Exit::FAILED, "internal.error", "an unknown exception", "report the defect with the command line");
    }
    if (prepared.found_.alias_) {
      reply.body_["note"] = "cmdatl " + join(*prepared.found_.alias_) + " is now cmdatl " + join(command.path_);
    }
    return static_cast<int>(output.finish(reply));
  } catch (...) {
    return report_lost_outcome(err, false);
  }
#endif
}

/**
 * @brief Runs one command line against a table of commands and writes its reply.
 * @param args The command line, without the program's name.
 * @param out stdout: replies, help, and failures in JSON.
 * @param err stderr: failures written as text.
 * @param terminal Whether `out` is a terminal (text by default) or not (compact JSON by default).
 * @param table The commands.
 * @param kis The command path on a KIS account, or empty.
 * @return The exit code.
 */
inline int run(
  const std::vector<std::string>& args, std::ostream& out, std::ostream& err, bool terminal, const Table& table,
  const BrokerPath& kis = {}
) {
  const auto prepared = prepare(args, table, terminal);
  const auto& rendering = prepared.globals_.rendering_;
  // Before any command starts nothing can have been sent, so a reply that could not be written is a plain failure.
  const auto written = [](std::ostream& stream, int exit) {
    stream.flush();
    return stream ? exit : static_cast<int>(Exit::FAILED);
  };
  const auto write = [&](const Reply& reply, const std::string& env) {
    auto& stream = stream_for(reply, rendering, out, err);
    return written(stream, static_cast<int>(write_reply(stream, reply, rendering, env)));
  };
  switch (prepared.action_) {
    case Prepared::Action::VERSION:
      out << "cmdatl " << CMDATL_VERSION << "\n";
      return written(out, 0);
    case Prepared::Action::HELP:
      write_help_for(out, table, prepared.words_);
      return written(out, 0);
    case Prepared::Action::REFUSE:
      if (!prepared.refusal_) {
        return write(error_reply(Exit::FAILED, "internal.error", "a refusal without a reason"), "offline");
      }
      return write(*prepared.refusal_, "offline");
    case Prepared::Action::RUN:
      break;
  }
  const auto& command = *prepared.found_.command_;
  if (!command.available_) {
    const auto not_yet = error_reply(
      Exit::FAILED, "unavailable.not_yet", "cmdatl " + join(command.path_) + " is not built in this version"
    );
    return write(not_yet, "offline");
  }
  if (command.run_account_) {
    return run_on_account(command, prepared, out, err, kis);
  }
  if (command.run_config_) {
    return run_on_config(command, prepared, out, err);
  }
  // A command not built that touches an account has no account to take an environment from: offline.
  const auto env = command.env_ == "account" ? std::string("offline") : command.env_;
  Output output(out, err, rendering, env);
  // From here the command may act: nothing below may let an exception reach main, whose fallback says nothing was
  // sent. A failure to report the outcome is reported as a minimal line and, for a command that may send, exit 4.
  try {
    Reply reply;
    try {
      reply = command.run_(prepared.invocation_, table, output);
    } catch (const std::exception& e) {
      // A command that may have sent cannot say nothing was sent: unknown, and reconcile first.
      reply = command.may_send_ ?
        error_reply(Exit::UNKNOWN, "unknown.internal", e.what(), "cmdatl order reconcile, then report the defect") :
        error_reply(Exit::FAILED, "internal.error", e.what(), "report the defect with the command line");
    } catch (...) {
      reply = command.may_send_ ?
        error_reply(Exit::UNKNOWN, "unknown.internal", "an unknown exception", "cmdatl order reconcile") :
        error_reply(Exit::FAILED, "internal.error", "an unknown exception", "report the defect with the command line");
    }
    if (prepared.found_.alias_) {
      reply.body_["note"] = "cmdatl " + join(*prepared.found_.alias_) + " is now cmdatl " + join(command.path_);
    }
    return static_cast<int>(output.finish(reply));
  } catch (...) {
    return report_lost_outcome(err, command.may_send_, command.mutates_ ? command.mutates_hint_ : std::string());
  }
}

/**
 * @brief Runs one command line against cmdatl's commands and writes its reply.
 * @param args The command line, without the program's name.
 * @param out stdout: replies, help, and failures in JSON.
 * @param err stderr: failures written as text.
 * @param terminal Whether `out` is a terminal (text by default) or not (compact JSON by default).
 * @param kis The command path on a KIS account, or empty.
 * @return The exit code.
 */
inline int run(
  const std::vector<std::string>& args, std::ostream& out, std::ostream& err, bool terminal, const BrokerPath& kis = {}
) {
  const Table table(command_list());
  return run(args, out, err, terminal, table, kis);
}

} // namespace cmdatl
