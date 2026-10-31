#pragma once

#include "./reply.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {

/** @brief A positional argument of a command. */
struct ArgSpec {
  /** @brief The name shown in usage, such as `SYMBOL`. */
  std::string name_;
  /** @brief What it is. */
  std::string help_;
  /** @brief Whether it must be given. */
  bool required_{true};
  /** @brief Whether it takes one or more values (`SYMBOL...`). */
  bool variadic_{false};
};

/** @brief An option of a command. */
struct OptionSpec {
  /** @brief The long name without dashes, such as `target`. */
  std::string name_;
  /** @brief The value's name in usage, such as `N`; empty for a flag. */
  std::string value_name_;
  /** @brief What it is. */
  std::string help_;
  /** @brief Whether it must be given. */
  bool required_{false};
  /** @brief Whether it may be given more than once (`-P k=v`). */
  bool repeated_{false};
  /** @brief A one-letter short name, or empty. */
  std::string short_name_;
};

/** @brief What a command was called with, after parsing. */
struct Invocation {
  /** @brief Each argument's or option's values by name; a flag that was given has one value, `true`. */
  std::map<std::string, std::vector<std::string>> values_;

  /**
   * @brief Whether a value was given.
   * @param name The argument's or option's name.
   * @return Whether it was given.
   */
  bool has(const std::string& name) const { return values_.count(name) != 0; }

  /**
   * @brief The first value given, or a default.
   * @param name The argument's or option's name.
   * @param fallback The value when none was given.
   * @return The value.
   */
  std::string get(const std::string& name, const std::string& fallback = "") const {
    const auto found = values_.find(name);
    return found == values_.end() || found->second.empty() ? fallback : found->second.front();
  }
};

class Table;
class Account;

/** @brief The exit codes of an account command: 4 when the account's state is unknown, 130 from the lock's wait. */
inline const std::vector<Exit> ACCOUNT_EXITS{Exit::OK, Exit::FAILED, Exit::USAGE, Exit::UNKNOWN, Exit::INTERRUPTED};

/** @brief One command: everything the parser, the help and the schema know about it, in one place. */
struct Command {
  /** @brief The words that call it, such as `{"order", "plan"}`. */
  std::vector<std::string> path_;
  /** @brief Earlier or alternative paths that still call it (kept while the interface is experimental). */
  std::vector<std::vector<std::string>> aliases_;
  /** @brief The heading the help lists it under, such as `Trade`. */
  std::string group_;
  /** @brief One line on what it does. */
  std::string summary_;
  /** @brief A longer description for `cmdatl COMMAND --help`, or empty. */
  std::string description_;
  /** @brief Example invocations, each a whole `cmdatl ...` line; the help leads with them, and a test parses them. */
  std::vector<std::string> examples_;
  /** @brief Its positional arguments. */
  std::vector<ArgSpec> args_;
  /** @brief Its options. */
  std::vector<OptionSpec> options_;
  /** @brief The reply's fields and what each holds. */
  std::vector<std::pair<std::string, std::string>> returns_;
  /** @brief The exit codes it can end with. */
  std::vector<Exit> exits_;
  /** @brief The error codes it can answer with. */
  std::vector<std::string> errors_;
  /** @brief Whether it can send anything to a broker. */
  bool may_send_{false};
  /**
   * @brief Whether it changes the account's state without sending (`clock advance`): an outcome it cannot report is
   *   unknown (exit 4), with `mutates_hint_` as what to do.
   */
  bool mutates_{false};
  /** @brief What to do when a change's outcome is unknown. */
  std::string mutates_hint_;
  /** @brief The reply's environment when it touches no account (`offline`), or `account` for the account's. */
  std::string env_{"offline"};
  /** @brief Whether it works yet; a command not built answers `unavailable.not_yet`. */
  bool available_{false};
  /**
   * @brief What it does: it reads the invocation, may stream events through the output, and returns the final
   *   reply. The code never sees the command's name, so renaming it touches only the path.
   */
  std::function<Reply(const Invocation&, const Table&, Output&)> run_;
  /**
   * @brief What it does on an open account, for an account command: `run_on_account` (cli.hpp) opens the account,
   *   holding its lock, and calls it.
   *
   * Set instead of `run_`, never with it.
   */
  std::function<Reply(const Invocation&, Account&, Output&)> run_account_;
  /** @brief An account command's checks of its arguments, before the account is opened; empty for none. */
  std::function<std::optional<Reply>(const Invocation&)> check_account_;

  /**
   * @brief Adds an earlier or alternative path that still calls the command.
   * @param path The path.
   * @return The command.
   */
  Command& alias(std::vector<std::string> path) {
    aliases_.push_back(std::move(path));
    return *this;
  }

  /**
   * @brief Sets the description `cmdatl COMMAND --help` shows.
   * @param text The description.
   * @return The command.
   */
  Command& describe(std::string text) {
    description_ = std::move(text);
    return *this;
  }

  /**
   * @brief Adds an example invocation.
   * @param line A whole command line, starting with `cmdatl `.
   * @return The command.
   */
  Command& example(std::string line) {
    examples_.push_back(std::move(line));
    return *this;
  }

  /**
   * @brief Adds a positional argument.
   * @param name Its name in usage.
   * @param help What it is.
   * @param required Whether it must be given.
   * @param variadic Whether it takes one or more values.
   * @return The command.
   */
  Command& arg(std::string name, std::string help, bool required = true, bool variadic = false) {
    args_.push_back({std::move(name), std::move(help), required, variadic});
    return *this;
  }

  /**
   * @brief Adds an option that takes a value.
   * @param name The long name without dashes.
   * @param value_name The value's name in usage.
   * @param help What it is.
   * @param required Whether it must be given.
   * @return The command.
   */
  Command& option(std::string name, std::string value_name, std::string help, bool required = false) {
    options_.push_back({std::move(name), std::move(value_name), std::move(help), required, false, ""});
    return *this;
  }

  /**
   * @brief Adds an option that may be given more than once.
   * @param spec The option.
   * @return The command.
   */
  Command& option(OptionSpec spec) {
    options_.push_back(std::move(spec));
    return *this;
  }

  /**
   * @brief Adds a flag.
   * @param name The long name without dashes.
   * @param help What it does.
   * @return The command.
   */
  Command& flag(std::string name, std::string help) {
    options_.push_back({std::move(name), "", std::move(help), false, false, ""});
    return *this;
  }

  /**
   * @brief Adds a field of the reply.
   * @param field The field's name.
   * @param help What it holds.
   * @return The command.
   */
  Command& returns(std::string field, std::string help) {
    returns_.emplace_back(std::move(field), std::move(help));
    return *this;
  }

  /**
   * @brief Sets the exit codes it can end with.
   * @param codes The exit codes.
   * @return The command.
   */
  Command& exits(std::vector<Exit> codes) {
    exits_ = std::move(codes);
    return *this;
  }

  /**
   * @brief Adds an error code it can answer with.
   * @param code The code.
   * @return The command.
   */
  Command& error(std::string code) {
    errors_.push_back(std::move(code));
    return *this;
  }

  /**
   * @brief Marks it as able to send to a broker.
   * @return The command.
   */
  Command& sends() {
    may_send_ = true;
    return *this;
  }

  /**
   * @brief Marks it as reading or changing the account (its replies carry the account's environment).
   * @return The command.
   */
  Command& account() {
    env_ = "account";
    return *this;
  }

  /**
   * @brief Marks it as changing the account's state without sending: an outcome it cannot report is unknown (exit 4).
   * @param hint What to do then, such as the command that shows the state.
   * @return The command.
   */
  Command& mutates(std::string hint) {
    mutates_ = true;
    mutates_hint_ = std::move(hint);
    return *this;
  }

  /**
   * @brief Gives an account command its code, which makes it available: it runs on the open account, its replies
   *   carry the account's environment, and its exit codes are `ACCOUNT_EXITS`.
   * @param run What it does on the account.
   * @param check Its checks of its arguments before the account is opened, or empty.
   * @return The command.
   */
  Command& on_account(
    std::function<Reply(const Invocation&, Account&, Output&)> run,
    std::function<std::optional<Reply>(const Invocation&)> check = {}
  ) {
    run_account_ = std::move(run);
    check_account_ = std::move(check);
    env_ = "account";
    available_ = true;
    exits_ = ACCOUNT_EXITS;
    return *this;
  }

  /**
   * @brief Gives it its code, which makes it available.
   * @param run What it does.
   * @return The command.
   */
  Command& runs(std::function<Reply(const Invocation&, const Table&, Output&)> run) {
    run_ = std::move(run);
    available_ = true;
    return *this;
  }
};

/**
 * @brief Starts a command's entry.
 * @param path The words that call it.
 * @param group The heading the help lists it under.
 * @param summary One line on what it does.
 * @return The command, to be completed with its builder calls.
 */
inline Command make_command(std::vector<std::string> path, std::string group, std::string summary) {
  Command command;
  command.path_ = std::move(path);
  command.group_ = std::move(group);
  command.summary_ = std::move(summary);
  return command;
}

/**
 * @brief Words joined by a separator, such as a command's path as written (`order plan`).
 * @param path The words.
 * @param separator The separator.
 * @return The joined text.
 */
inline std::string join(const std::vector<std::string>& path, const std::string& separator = " ") {
  std::string out;
  for (const auto& word : path) {
    out += (out.empty() ? "" : separator) + word;
  }
  return out;
}

/**
 * @brief The edit distance between two words (insertions, deletions and substitutions).
 * @param a One word.
 * @param b The other.
 * @return The distance.
 */
inline std::size_t edit_distance(const std::string& a, const std::string& b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) {
    row[j] = j;
  }
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const auto above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

/** @brief Where a command line's words led. */
struct Resolution {
  /** @brief The command, or null when none matched. */
  const Command* command_{nullptr};
  /** @brief How many leading words named it. */
  std::size_t used_{0};
  /** @brief The alias it was called by, when not its own path. */
  std::optional<std::vector<std::string>> alias_;
};

/** @brief Every command, and the lookups the parser, the help and the schema need. */
class Table {
public:
  /**
   * @brief Builds the table.
   * @param commands The commands.
   */
  explicit Table(std::vector<Command> commands)
  : commands_(std::move(commands)) {}

  /**
   * @brief Every command, in the order the help lists them.
   * @return The commands.
   */
  const std::vector<Command>& commands() const noexcept { return commands_; }

  /**
   * @brief The command named by the longest leading run of words, by its path or an alias.
   * @param words The command line's words, options removed.
   * @return The resolution.
   */
  Resolution resolve(const std::vector<std::string>& words) const {
    Resolution best;
    for (const auto& command : commands_) {
      consider(command, command.path_, false, words, best);
      for (const auto& alias : command.aliases_) {
        consider(command, alias, true, words, best);
      }
    }
    return best;
  }

  /**
   * @brief Whether some command's path or alias continues these words (a group such as `order`).
   * @param words The words.
   * @return Whether they name a group.
   */
  bool is_group(const std::vector<std::string>& words) const {
    return std::any_of(commands_.begin(), commands_.end(), [&words](const Command& command) {
      return starts_with(command.path_, words, true) ||
        std::any_of(command.aliases_.begin(), command.aliases_.end(), [&words](const std::vector<std::string>& alias) {
          return starts_with(alias, words, true);
        });
    });
  }

  /**
   * @brief The commands a selector names: those whose path or an alias starts with it (`order`, `order plan`).
   * @param selector The selector's words; none selects every command.
   * @return The commands, in the table's order.
   */
  std::vector<const Command*> select(const std::vector<std::string>& selector) const {
    std::vector<const Command*> out;
    for (const auto& command : commands_) {
      const bool by_alias = std::any_of(
        command.aliases_.begin(),
        command.aliases_.end(),
        [&selector](const std::vector<std::string>& alias) { return starts_with(alias, selector, false); }
      );
      if (starts_with(command.path_, selector, false) || by_alias) {
        out.push_back(&command);
      }
    }
    return out;
  }

  /**
   * @brief Whether a path starts with some words.
   * @param path The path.
   * @param words The words.
   * @param longer Whether the path must be longer than the words.
   * @return Whether it does.
   */
  static bool starts_with(const std::vector<std::string>& path, const std::vector<std::string>& words, bool longer) {
    if (path.size() < words.size() || (longer && path.size() == words.size())) {
      return false;
    }
    return std::equal(words.begin(), words.end(), path.begin());
  }

  /**
   * @brief The closest known paths to words that matched nothing.
   * @param words The words.
   * @return Up to three suggestions, closest first.
   */
  std::vector<std::string> suggest(const std::vector<std::string>& words) const {
    std::vector<std::pair<std::size_t, std::string>> scored;
    const auto typed = join(words);
    for (const auto& command : commands_) {
      std::vector<std::vector<std::string>> paths{command.path_};
      paths.insert(paths.end(), command.aliases_.begin(), command.aliases_.end());
      for (const auto& path : paths) {
        const auto length = static_cast<std::ptrdiff_t>(std::min(path.size(), words.size()));
        const auto prefix = std::vector<std::string>(path.begin(), path.begin() + length);
        const auto distance = edit_distance(typed, join(prefix));
        if (distance <= std::max<std::size_t>(2, typed.size() / 3)) {
          scored.emplace_back(distance, join(command.path_));
        }
      }
    }
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    for (const auto& entry : scored) {
      if (std::find(out.begin(), out.end(), entry.second) == out.end() && out.size() < 3) {
        out.push_back(entry.second);
      }
    }
    return out;
  }

private:
  /**
   * @brief Keeps a path when it matches the leading words and is longer than the best so far.
   * @param command The command.
   * @param path Its path or an alias.
   * @param alias Whether the path is an alias.
   * @param words The command line's words.
   * @param best The best match so far.
   */
  static void consider(
    const Command& command,
    const std::vector<std::string>& path,
    bool alias,
    const std::vector<std::string>& words,
    Resolution& best
  ) {
    if (path.size() > words.size() || path.size() <= best.used_) {
      return;
    }
    if (!std::equal(path.begin(), path.end(), words.begin())) {
      return;
    }
    best.command_ = &command;
    best.used_ = path.size();
    best.alias_ = alias ? std::optional<std::vector<std::string>>(path) : std::nullopt;
  }

  std::vector<Command> commands_;
};

}  // namespace cmdatl
