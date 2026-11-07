#pragma once

#include "../core/errors.hpp"
#include "../core/table.hpp"

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cmdatl {

/** @brief The version of `cmdatl schema`'s format; raised when a command is renamed or a field changes meaning. */
inline constexpr int SCHEMA_VERSION = 11;

/** @brief Where the documentation and the issue tracker are. */
inline constexpr const char* HOME_PAGE = "https://github.com/algotradelib/cmdatl";

/** @brief Exit 3's words, the same in the help, the README and the agent guide (a test checks them). */
inline constexpr const char* EXIT_REFUSED_TEXT = "3 refused (nothing was sent)";
/** @brief Exit 4's words, the same in the help, the README and the agent guide (a test checks them). */
inline constexpr const char* EXIT_UNKNOWN_TEXT = "4 unknown (follow error.next before anything else)";

/** @brief The examples `cmdatl --help` opens with: the most common uses (each parsed by the tests). */
inline const std::vector<std::string> TOP_EXAMPLES{
  "cmdatl status",
  "cmdatl strategy backtest mean-reversion.toml params.lookback=30",
  "cmdatl run mean-reversion.toml",
  "cmdatl order plan 005930 --target 10 --limit 78200",
};

/** @brief The error codes any command can answer with, before or after it runs. */
inline const std::vector<std::string> COMMON_ERRORS{
  "usage.missing_argument",
  "usage.bad_option",
  "usage.bad_format",
  "usage.no_such_field",
  "internal.error",
};

/** @brief The error codes every available account command can answer with, from opening the account. */
inline const std::vector<std::string> ACCOUNT_ERRORS{
  "unavailable.platform",
  "config.missing",
  "config.invalid",
  "unsupported.broker",
  "state.unavailable",
  "busy.locked",
  "interrupted",
  "unknown.lock_lost",
  "unknown.journal_missing",
  "unknown.journal_damaged",
  "journal.unopened",
  "paper.data",
  "unknown.paper_state",
  "file.unwritable",
};

/** @brief An option every command takes. */
struct GlobalSpec {
  /** @brief How it is written. */
  std::string name_;
  /** @brief Its value's name, or empty for a flag. */
  std::string value_name_;
  /** @brief What it does. */
  std::string help_;
};

/**
 * @brief The options every command takes (`cli.hpp` parses exactly these).
 * @return The options.
 */
inline const std::vector<GlobalSpec>& global_options() {
  static const std::vector<GlobalSpec> globals{
    {"--json", "", "compact JSON, whatever the terminal"},
    {"--format", "json|jsonl|text", "the reply's format"},
    {"--fields", "a,b.c", "only these fields (dotted fields select inside objects and every array element)"},
    {"--value", "PATH", "one value, bare (positions.0.quantity)"},
    {"-h, --help", "", "help for cmdatl, a group or a command"},
    {"--version", "", "cmdatl's version"},
  };
  return globals;
}

/**
 * @brief A command's error codes: the common ones, `unavailable.not_yet` while it is not built, `unknown.internal`
 *   for a command that may send or changes the account, the account errors for an account command, and its own.
 * @param command The command.
 * @return The codes.
 */
inline std::vector<std::string> errors_of(const Command& command) {
  auto errors = COMMON_ERRORS;
  if (!command.available_) {
    errors.push_back("unavailable.not_yet");
  }
  if (command.may_send_ || command.mutates_) {
    errors.push_back("unknown.internal");
  }
  if (command.run_account_) {
    errors.insert(errors.end(), ACCOUNT_ERRORS.begin(), ACCOUNT_ERRORS.end());
  }
  for (const auto& error : command.errors_) {
    if (std::find(errors.begin(), errors.end(), error) == errors.end()) {
      errors.push_back(error);
    }
  }
  return errors;
}

/**
 * @brief The words of a command's usage: arguments, then options.
 * @param command The command.
 * @return The usage, such as `SYMBOL --target N --limit P`.
 */
inline std::string usage(const Command& command) {
  std::vector<std::string> parts;
  for (const auto& arg : command.args_) {
    const auto part = arg.name_ + (arg.variadic_ ? "..." : "");
    parts.push_back(arg.required_ ? part : "[" + part + "]");
  }
  for (const auto& option : command.options_) {
    const auto flag = option.short_name_.empty() ? "--" + option.name_ : "-" + option.short_name_;
    auto part = flag + (option.value_name_.empty() ? "" : " " + option.value_name_);
    if (!option.required_) {
      part = "[" + part + "]";
    }
    parts.push_back(part + (option.repeated_ ? "..." : ""));
  }
  return join(parts);
}

/**
 * @brief The text after a command's summary when it is not built yet.
 * @param command The command.
 * @return The marker, or empty.
 */
inline std::string availability(const Command& command) {
  return command.available_ ? "" : " (not yet)";
}

/**
 * @brief Text padded with spaces to a width (at least one space).
 * @param text The text.
 * @param width The width.
 * @return The padding.
 */
inline std::string pad_to(const std::string& text, std::size_t width) {
  return std::string(text.size() < width ? width - text.size() : 1, ' ');
}

/**
 * @brief Writes `cmdatl --help`: every command by group, from the table.
 * @param out The stream.
 * @param table The commands.
 */
inline void write_help(std::ostream& out, const Table& table) {
  out << "cmdatl: algotradelib from the command line - strategies in atls, backtests and trading loops\n\n";
  out << "Usage: cmdatl [--json | --format json|jsonl|text] [--fields a,b.c] [--value PATH] COMMAND ...\n";
  out << "\nExamples\n";
  for (const auto& example : TOP_EXAMPLES) {
    out << "  " << example << "\n";
  }
  std::vector<std::string> groups;
  for (const auto& command : table.commands()) {
    if (std::find(groups.begin(), groups.end(), command.group_) == groups.end()) {
      groups.push_back(command.group_);
    }
  }
  const std::size_t width = 44;
  for (const auto& group : groups) {
    out << "\n" << group << "\n";
    for (const auto& command : table.commands()) {
      if (command.group_ != group) {
        continue;
      }
      const auto args = usage(command);
      const auto left = join(command.path_) + (args.empty() ? "" : " " + args);
      out << "  " << left;
      if (left.size() >= width) {
        out << "\n  " << std::string(width, ' ');
      } else {
        out << pad_to(left, width);
      }
      out << "  " << command.summary_ << availability(command) << "\n";
    }
  }
  out << "\nExit: 0 ok, 1 failed and nothing sent, 2 usage, " << EXIT_REFUSED_TEXT << ",\n";
  out << EXIT_UNKNOWN_TEXT << ", 5 strategy errors, 130 interrupted.\n";
  out << "Output: text on a terminal, compact JSON otherwise. cmdatl help COMMAND for one command; cmdatl schema for\n";
  out << "the machine contract; cmdatl guide agent or strategy to learn it. Docs and issues: " << HOME_PAGE << "\n";
}

/**
 * @brief Writes the help of a group, such as `cmdatl order --help`: the commands its path or an alias continues.
 * @param out The stream.
 * @param table The commands.
 * @param words The group's words.
 */
inline void write_group_help(std::ostream& out, const Table& table, const std::vector<std::string>& words) {
  out << "cmdatl " << join(words) << " COMMAND\n\n";
  for (const auto* command : table.select(words)) {
    if (command->path_ == words) {
      continue;
    }
    const auto args = usage(*command);
    out << "  " << join(command->path_) << (args.empty() ? "" : " " + args) << "\n";
    out << "      " << command->summary_ << availability(*command) << "\n";
  }
}

/**
 * @brief Writes one command's help: usage, description, arguments, options, reply fields and exit codes.
 * @param out The stream.
 * @param command The command.
 */
inline void write_command_help(std::ostream& out, const Command& command) {
  const auto args = usage(command);
  out << "cmdatl " << join(command.path_) << (args.empty() ? "" : " " + args) << "\n\n";
  out << (command.description_.empty() ? command.summary_ : command.description_) << "\n";
  if (!command.available_) {
    out << "Not built in this version yet.\n";
  }
  if (!command.examples_.empty()) {
    out << "\nExamples:\n";
    for (const auto& example : command.examples_) {
      out << "  " << example << "\n";
    }
  }
  if (!command.args_.empty() || !command.options_.empty()) {
    out << "\n";
  }
  for (const auto& arg : command.args_) {
    out << "  " << arg.name_ << pad_to(arg.name_, 14) << arg.help_ << "\n";
  }
  for (const auto& option : command.options_) {
    auto name = option.short_name_.empty() ? "--" + option.name_ : "-" + option.short_name_ + ", --" + option.name_;
    name += option.value_name_.empty() ? "" : " " + option.value_name_;
    out << "  " << name << pad_to(name, 14) << option.help_ << "\n";
  }
  if (!command.returns_.empty()) {
    out << "Replies:";
    for (const auto& field : command.returns_) {
      out << " " << field.first;
    }
    out << "\n";
  }
  out << "Exit:";
  for (const auto exit : command.exits_) {
    out << " " << static_cast<int>(exit);
  }
  out << (command.may_send_ ? "  (may send to a broker)" : "") << (command.mutates_ ? "  (changes the account)" : "");
  out << "\n";
  for (const auto& alias : command.aliases_) {
    out << "Also called as: cmdatl " << join(alias) << "\n";
  }
}

/**
 * @brief Writes the help some words ask for: a command's (and, for a command that is also a group such as
 *   `clock`, its subcommands), a group's, or every command's for no words. Used by `--help` and `cmdatl help`.
 * @param out The stream.
 * @param table The commands.
 * @param words The words, such as `order plan`; extra words after a command (its arguments) are ignored.
 * @return Whether the words named a command or a group (always, for no words).
 */
inline bool write_help_for(std::ostream& out, const Table& table, const std::vector<std::string>& words) {
  const auto found = table.resolve(words);
  if (found.command_ != nullptr) {
    write_command_help(out, *found.command_);
    if (found.used_ == words.size() && table.is_group(words)) {
      out << "\n";
      write_group_help(out, table, words);
    }
    return true;
  }
  if (!words.empty() && table.is_group(words)) {
    write_group_help(out, table, words);
    return true;
  }
  write_help(out, table);
  return words.empty();
}

/**
 * @brief One command as `cmdatl schema` describes it.
 * @param command The command.
 * @return Its entry.
 */
inline json::object schema_entry(const Command& command) {
  json::object entry;
  entry["command"] = join(command.path_, ".");
  json::array path;
  for (const auto& word : command.path_) {
    path.push_back(json::value(word));
  }
  entry["path"] = std::move(path);
  json::array aliases;
  for (const auto& alias : command.aliases_) {
    aliases.push_back(json::value(join(alias)));
  }
  entry["aliases"] = std::move(aliases);
  entry["summary"] = command.summary_;
  json::array examples;
  for (const auto& example : command.examples_) {
    examples.push_back(json::value(example));
  }
  entry["examples"] = std::move(examples);
  json::array args;
  for (const auto& arg : command.args_) {
    args.push_back(json::object{{"name", arg.name_}, {"required", arg.required_}, {"variadic", arg.variadic_}});
  }
  entry["args"] = std::move(args);
  json::array options;
  for (const auto& option : command.options_) {
    options.push_back(
      json::object{
        {"name", "--" + option.name_},
        {"short", option.short_name_.empty() ? json::value(nullptr) : json::value("-" + option.short_name_)},
        {"value", option.value_name_},
        {"required", option.required_},
        {"repeated", option.repeated_},
      }
    );
  }
  entry["options"] = std::move(options);
  json::object returns;
  for (const auto& field : command.returns_) {
    returns[field.first] = field.second;
  }
  entry["returns"] = std::move(returns);
  json::array exits;
  for (const auto exit : command.exits_) {
    exits.push_back(static_cast<int>(exit));
  }
  entry["exits"] = std::move(exits);
  json::array errors;
  for (const auto& error : errors_of(command)) {
    errors.push_back(json::value(error));
  }
  entry["errors"] = std::move(errors);
  entry["may_send"] = command.may_send_;
  entry["mutates"] = command.mutates_;
  entry["available"] = command.available_;
  return entry;
}

/**
 * @brief `cmdatl schema [COMMAND|errors]`: every command, those a selector names (by path or alias), or the errors.
 * @param table The commands.
 * @param words The selector's words (`order`, `order plan`, `order.plan`, `errors`), or none for everything.
 * @return The reply.
 */
inline Reply schema_reply(const Table& table, const std::vector<std::string>& words) {
  Reply reply;
  reply.body_["schema_version"] = SCHEMA_VERSION;
  if (words.size() == 1 && words.front() == "errors") {
    json::array errors;
    for (const auto& error : error_catalog()) {
      errors.push_back(
        json::object{
          {"code", error.code_},
          {"exit", static_cast<int>(error.exit_)},
          {"retry", error.retry_},
          {"next", error.next_},
        }
      );
    }
    reply.body_["errors"] = std::move(errors);
    return reply;
  }
  std::vector<std::string> selector;
  for (const auto& word : words) {
    for (const auto& part : split(word, '.')) {
      selector.push_back(part);
    }
  }
  const auto selected = table.select(selector);
  if (selected.empty()) {
    const auto suggestions = table.suggest(selector);
    return error_reply(
      Exit::USAGE, "usage.unknown_command", "no command " + join(selector),
      suggestions.empty() ? "cmdatl schema lists every command" : "did you mean: " + join(suggestions, ", ")
    );
  }
  json::array commands;
  for (const auto* command : selected) {
    commands.push_back(schema_entry(*command));
  }
  reply.body_["commands"] = std::move(commands);
  if (selector.empty()) {
    json::array globals;
    for (const auto& global : global_options()) {
      globals.push_back(json::object{{"name", global.name_}, {"value", global.value_name_}, {"help", global.help_}});
    }
    reply.body_["global_options"] = std::move(globals);
  }
  return reply;
}

/**
 * @brief Checks the table's invariants: unique paths and aliases, positional layouts the parser can read, unique
 *   argument and option names and short names, no option named like a global one, errors in the catalog, exit codes
 *   3 and 4 on every command that may send, exit 4 on every command that changes the account, and code exactly when
 *   available, of one kind.
 * @param table The commands.
 * @return One line per problem; empty when the table is sound.
 */
inline std::vector<std::string> check_table(const Table& table) {
  std::vector<std::string> problems;
  std::set<std::string> paths;
  std::set<std::string> catalog;
  for (const auto& error : error_catalog()) {
    catalog.insert(error.code_);
  }
  const std::set<std::string> reserved{"json", "format", "fields", "value", "help", "version", "h"};
  for (const auto& command : table.commands()) {
    const auto name = join(command.path_);
    std::vector<std::vector<std::string>> all{command.path_};
    all.insert(all.end(), command.aliases_.begin(), command.aliases_.end());
    const auto empty_word = [](const std::string& w) {
      return w.empty();
    };
    for (const auto& path : all) {
      if (path.empty() || std::any_of(path.begin(), path.end(), empty_word)) {
        problems.push_back(name + ": an empty path or word");
      }
      if (!paths.insert(join(path)).second) {
        problems.push_back(name + ": the path " + join(path) + " is used twice");
      }
    }
    if (command.summary_.empty() || command.group_.empty() || command.returns_.empty() || command.exits_.empty()) {
      problems.push_back(name + ": a summary, group, reply field or exit code is missing");
    }
    if (command.examples_.empty()) {
      problems.push_back(name + ": no example");
    }
    // An example calls its own command: its leading words resolve to this entry, not to a longer path (an
    // example of `clock` must not be `clock advance`). Its arguments and options are checked by the tests.
    for (const auto& example : command.examples_) {
      std::vector<std::string> words;
      std::istringstream in(example);
      std::string program;
      in >> program;
      std::string word;
      while (in >> word && word.front() != '-' && word.front() != '"' && word.front() != '\'') {
        words.push_back(word);
      }
      if (program != "cmdatl" || words.empty() || table.resolve(words).command_ != &command) {
        problems.push_back(name + ": the example '" + example + "' does not call it");
      }
    }
    const bool runs = static_cast<bool>(command.run_);
    const bool runs_on_account = static_cast<bool>(command.run_account_);
    const bool runs_on_config = static_cast<bool>(command.run_config_);
    const int kinds = (runs ? 1 : 0) + (runs_on_account ? 1 : 0) + (runs_on_config ? 1 : 0);
    if (command.available_ != (kinds > 0) || kinds > 1) {
      problems.push_back(name + ": available without code, code without available, or two kinds of code");
    }
    if ((runs_on_account || runs_on_config) && command.env_ != "account") {
      problems.push_back(name + ": runs on the account but its replies do not carry the account's environment");
    }
    if (
      command.mutates_ && std::find(command.exits_.begin(), command.exits_.end(), Exit::UNKNOWN) == command.exits_.end()
    ) {
      problems.push_back(name + ": changes the account but cannot answer unknown (4)");
    }
    std::set<std::string> keys;
    std::set<std::string> shorts;
    for (const auto& arg : command.args_) {
      if (!keys.insert(arg.name_).second) {
        problems.push_back(name + ": the argument " + arg.name_ + " is declared twice");
      }
    }
    for (std::size_t i = 0; i < command.args_.size(); ++i) {
      if (command.args_[i].variadic_ && i + 1 != command.args_.size()) {
        problems.push_back(name + ": a variadic argument that is not the last");
      }
      if (!command.args_[i].required_ && i + 1 < command.args_.size() && command.args_[i + 1].required_) {
        problems.push_back(name + ": an optional argument before a required one");
      }
    }
    for (const auto& option : command.options_) {
      if (!keys.insert(option.name_).second) {
        problems.push_back(name + ": the option " + option.name_ + " is declared twice");
      }
      if (reserved.count(option.name_) != 0 || reserved.count(option.short_name_) != 0) {
        problems.push_back(name + ": the option " + option.name_ + " collides with a global option");
      }
      if (!option.short_name_.empty() && !shorts.insert(option.short_name_).second) {
        problems.push_back(name + ": the short name -" + option.short_name_ + " is used twice");
      }
    }
    for (const auto& error : errors_of(command)) {
      if (catalog.count(error) == 0) {
        problems.push_back(name + ": the error " + error + " is not in the catalog");
      }
    }
    if (command.may_send_) {
      const auto& exits = command.exits_;
      if (
        std::find(exits.begin(), exits.end(), Exit::REFUSED) == exits.end() ||
        std::find(exits.begin(), exits.end(), Exit::UNKNOWN) == exits.end()
      ) {
        problems.push_back(name + ": may send but cannot answer refused (3) or unknown (4)");
      }
    }
  }
  return problems;
}

} // namespace cmdatl
