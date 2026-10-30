#pragma once

#include "./command_list.hpp"
#include "./describe.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace cmdatl {

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
  const std::vector<std::string>& args,
  Globals& globals,
  std::vector<std::string>& rest
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
  const Command& command,
  const std::vector<std::string>& words,
  Invocation& invocation
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
      added->expected(1);
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
      // One value per occurrence: `-P a=1 -P b=2`, never `-P a=1 b=2`.
      added->expected(1, -1)->allow_extra_args(false);
    } else {
      added->expected(1);
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
        Exit::USAGE,
        "usage.unknown_command",
        "cmdatl " + join(words) + " needs a command",
        "cmdatl " + join(words) + " --help"
      );
      return prepared;
    }
    const auto suggestions = table.suggest(words);
    prepared.refusal_ = error_reply(
      Exit::USAGE,
      "usage.unknown_command",
      "no command " + join(words),
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
  for (const auto& field : asked) {
    if (!declares_field(command, field)) {
      prepared.refusal_ = error_reply(
        Exit::USAGE,
        "usage.no_such_field",
        "cmdatl " + join(command.path_) + " replies have no field " + field,
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
 * @brief Runs one command line against a table of commands and writes its reply.
 * @param args The command line, without the program's name.
 * @param out stdout: replies, help, and failures in JSON.
 * @param err stderr: failures written as text.
 * @param terminal Whether `out` is a terminal (text by default) or not (compact JSON by default).
 * @param table The commands.
 * @return The exit code.
 */
inline int run(
  const std::vector<std::string>& args,
  std::ostream& out,
  std::ostream& err,
  bool terminal,
  const Table& table
) {
  const auto prepared = prepare(args, table, terminal);
  const auto& rendering = prepared.globals_.rendering_;
  const auto write = [&](const Reply& reply, const std::string& env) {
    return static_cast<int>(write_reply(stream_for(reply, rendering, out, err), reply, rendering, env));
  };
  switch (prepared.action_) {
    case Prepared::Action::VERSION:
      out << "cmdatl " << CMDATL_VERSION << "\n";
      return 0;
    case Prepared::Action::HELP:
      write_help_for(out, table, prepared.words_);
      return 0;
    case Prepared::Action::REFUSE:
      return write(*prepared.refusal_, "offline");
    case Prepared::Action::RUN:
      break;
  }
  const auto& command = *prepared.found_.command_;
  if (!command.available_) {
    const auto not_yet = error_reply(
      Exit::FAILED,
      "unavailable.not_yet",
      "cmdatl " + join(command.path_) + " is not built in this version"
    );
    return write(not_yet, "offline");
  }
  // An account command's environment comes from the account it reads (branch 3); until then, offline.
  const auto env = command.env_ == "account" ? std::string("offline") : command.env_;
  Output output(out, err, rendering, env);
  auto reply = command.run_(prepared.invocation_, table, output);
  if (prepared.found_.alias_) {
    reply.body_["note"] = "cmdatl " + join(*prepared.found_.alias_) + " is now cmdatl " + join(command.path_);
  }
  return static_cast<int>(output.finish(reply));
}

/**
 * @brief Runs one command line against cmdatl's commands and writes its reply.
 * @param args The command line, without the program's name.
 * @param out stdout: replies, help, and failures in JSON.
 * @param err stderr: failures written as text.
 * @param terminal Whether `out` is a terminal (text by default) or not (compact JSON by default).
 * @return The exit code.
 */
inline int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, bool terminal) {
  const Table table(command_list());
  return run(args, out, err, terminal, table);
}

}  // namespace cmdatl
