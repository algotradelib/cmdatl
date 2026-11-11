#pragma once

#include "../core/reply.hpp"
#include "../core/table.hpp"
#include "../system/files.hpp"
#include "../system/process.hpp"
#include "../system/terminal.hpp"
#include "./brokers.hpp"
#include "./keychain.hpp"
#include "./store.hpp"
#include "./user_brokers.hpp"

#include <algotradelib/src/network/file_token_store.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace cmdatl {
namespace auth {

/** @brief What `auth login` was given. */
struct LoginRequest {
  /** @brief `--broker`, if given. */
  std::optional<std::string> broker_;
  /** @brief `--env`, if given. */
  std::optional<std::string> env_;
  /** @brief `--with-secret`: the document comes on standard input. */
  bool with_secret_{false};
  /** @brief `--force`: replace a stored login without asking. */
  bool force_{false};
};

/** @brief What `auth status` and `auth logout` were given. */
struct LoginFilter {
  /** @brief `--broker`, if given. */
  std::optional<std::string> broker_;
  /** @brief `--env`, if given. */
  std::optional<std::string> env_;
};

#if !defined(__APPLE__)
/**
 * @brief Whether the session can show a dialog; no store on this platform, so never.
 * @return False.
 */
inline bool session_has_graphics() {
  return false;
}
#endif

/**
 * @brief Whether a store may show its own dialogs: standard input is a terminal and the session can show one.
 * @param fd Standard input.
 * @return Whether it may.
 */
inline bool interaction_allowed(int fd) {
  return sys::is_terminal(fd) && session_has_graphics();
}

/**
 * @brief The system's credential store: the macOS keychain; none elsewhere in this version.
 * @param interaction Whether the store may show its own dialogs.
 * @return The store, or null where there is none.
 */
inline std::unique_ptr<CredentialStore> open_store(bool interaction) {
#if defined(__APPLE__)
  return std::make_unique<KeychainStore>(interaction);
#else
  (void)interaction;
  return nullptr;
#endif
}

/**
 * @brief A login's name: `<broker>/<environment>`.
 * @param broker The broker.
 * @param demo Whether it is the demo environment.
 * @return The name.
 */
inline std::string login_name(const Broker& broker, bool demo) {
  return broker.name_ + "/" + (demo ? "demo" : "real");
}

/**
 * @brief A value masked for display: its first two characters, then `****`; only `****` for a value shorter than
 *   eight characters.
 * @param value The value.
 * @return The masked text.
 */
inline std::string masked(const std::string& value) {
  return value.size() >= 8 ? value.substr(0, 2) + "****" : std::string("****");
}

/**
 * @brief The row of a broker folder's login: broker, environment, the folder (or null when it no longer reads), the
 *   store and `checked: false`; no field of the document, whose schema may have changed since the login.
 * @param name The broker's name.
 * @param demo Whether it is the demo environment.
 * @param folder The folder, or none.
 * @param store The store's kind.
 * @return The row.
 */
inline json::object folder_login_row(
  const std::string& name, bool demo, const std::optional<std::string>& folder, const std::string& store
) {
  json::object row;
  row["broker"] = name;
  row["environment"] = demo ? "demo" : "real";
  row["adapter"] = folder ? json::value(*folder) : json::value(nullptr);
  row["store"] = store;
  row["checked"] = false;
  return row;
}

/**
 * @brief The reply's row for a stored login of a built-in broker: broker, environment, the key masked, the account
 *   masked where the broker has one, the store, and `checked: false` (the broker has not seen it).
 * @param broker The broker.
 * @param demo Whether it is the demo environment.
 * @param document The canonical document.
 * @param store The store's kind.
 * @return The row.
 */
inline json::object login_row(const Broker& broker, bool demo, const json::object& document, const std::string& store) {
  json::object row;
  row["broker"] = broker.name_;
  row["environment"] = demo ? "demo" : "real";
  const auto& key = broker.fields_.front();
  if (const auto* value = document.if_contains(key.json_); value != nullptr && value->is_string()) {
    row["key"] = masked(std::string(value->get_string()));
  }
  for (const auto& field : broker.fields_) {
    if (!field.shown_masked_) {
      continue;
    }
    const auto* value = document.if_contains(field.json_);
    row["account"] = value != nullptr && value->is_string() ?
      json::value(masked(std::string(value->get_string()))) :
      json::value(nullptr);
  }
  row["store"] = store;
  row["checked"] = false;
  return row;
}

/**
 * @brief The refusal for a store that cannot answer.
 * @param kind The store's kind.
 * @param status How it answered.
 * @param detail Why.
 * @param change Whether it failed storing or removing, so whether the change happened is not known.
 * @return The reply.
 */
inline Reply store_failure(
  const std::string& kind, StoreStatus status, const std::string& detail, bool change = false
) {
  if (change && status == StoreStatus::UNAVAILABLE) {
    return error_reply(
      Exit::FAILED, "auth.no_store", "the " + kind + " did not confirm the change (" + detail + ")",
      "cmdatl auth status shows what is stored"
    );
  }
  if (status == StoreStatus::LOCKED) {
    return error_reply(
      Exit::FAILED, "auth.store_locked",
      "the " + kind + " needs approving or unlocking, and nobody can be asked here (" + detail + "); nothing changed",
      "approve or unlock it from a local graphical login session: run cmdatl auth status there once"
    );
  }
  return error_reply(
    Exit::FAILED, "auth.no_store", "the " + kind + " cannot be used (" + detail + "); nothing changed"
  );
}

/**
 * @brief The refusal where the system has no credential store.
 * @return The reply.
 */
inline Reply no_store_reply() {
  return error_reply(
    Exit::FAILED, "auth.no_store", "this cmdatl has no credential store, so no login can be kept; nothing changed",
    "logins are kept in the macOS keychain; other systems have no store in this version"
  );
}

/**
 * @brief Removes a stored login's saved access token through the library's store, as far as it can, before the login
 *   is removed or replaced: the token's identity is read from the login. It never stops the change. A broker folder's
 *   login has no such token, nor has any login when the token folder does not exist.
 * @param broker The login's broker.
 * @param name The login's name.
 * @param store The credential store.
 * @return Why the token may remain, for the reply, or none when it is gone or there was none.
 */
inline std::optional<std::string> remove_token(const Broker& broker, const std::string& name, CredentialStore& store) {
  const auto folder = token_folder();
  struct stat info{};
  if (broker.read_ == nullptr || !folder || (::lstat(folder->c_str(), &info) != 0 && errno == ENOENT)) {
    return std::nullopt;
  }
  std::string reason = "the stored login could not be read";
  std::string identity;
  const auto found = store.get(name);
  if (found.status_ == StoreStatus::OK) {
    try {
      identity = broker.read_(found.value_, "stored login " + name);
    } catch (const std::exception&) {
      // The loader's message is not repeated: the document is the user's.
      reason = "no key could be read from the stored login";
    }
  }
  if (!identity.empty()) {
    algotradelib::network::FileTokenStore tokens(*folder);
    const auto held = tokens.lock(identity);
    const auto failed = held ? tokens.remove(identity) : std::optional<algotradelib::BrokerError>(held.error());
    if (!failed) {
      return std::nullopt;
    }
    reason = failed->message_;
  }
  auto warning = "the saved access token of " + name + " may still be usable until it expires (" + reason +
    "); saved tokens are in " + *folder;
  if (reason.find("not private") != std::string::npos) {
    warning += ", which the operator makes private (chmod 700 on it, chmod 600 on each file in it)";
  }
  return warning;
}

/**
 * @brief The reply after SIGINT, SIGTERM or SIGHUP.
 * @return The reply.
 */
inline Reply interrupted_auth() {
  return error_reply(
    Exit::INTERRUPTED, "interrupted", "interrupted by a signal; nothing changed", "run the command again", true
  );
}

/**
 * @brief The reply for an answer that did not come, by how the read ended.
 * @param status How it ended.
 * @return The reply.
 */
inline Reply no_answer(sys::ReadStatus status) {
  if (status == sys::ReadStatus::INTERRUPTED) {
    return interrupted_auth();
  }
  return error_reply(
    Exit::FAILED, "auth.no_answer",
    status == sys::ReadStatus::END ?
      "the input ended before an answer; nothing changed" :
      "the answer could not be read (echo could not be turned off, or it was too "
      "long); nothing changed"
  );
}

/**
 * @brief The refusal of a document.
 * @param problem What is wrong, naming the field, never a value.
 * @return The reply.
 */
inline Reply bad_document(const std::string& problem) {
  return error_reply(
    Exit::FAILED, "auth.bad_document", problem + "; nothing was stored",
    "cmdatl help auth login shows each broker's fields"
  );
}

/**
 * @brief Where an unknown broker's message points: the built-in brokers and the folders' directory.
 * @return The text.
 */
inline std::string brokers_hint() {
  const auto root = brokers_root();
  return "the brokers are: " + broker_names() + ", or a folder installed in " +
    (root ? root->string() : std::string("<config>/cmdatl/brokers"));
}

/**
 * @brief `auth login`: asks for what was not given (on a terminal), checks the document locally with the library's
 *   loaders (a broker folder's is stored unchecked: its adapter reads it at the run), and stores it; the saved access
 *   token of a login it replaces is removed first, as far as it can be. Sends nothing to a broker.
 * @param request What was given.
 * @param in Standard input.
 * @param prompts Where questions go (stderr).
 * @param store The credential store, or null where there is none.
 * @return The reply.
 */
inline Reply login(const LoginRequest& request, int in, std::ostream& prompts, CredentialStore* store) {
  // The flags given are checked first, so a wrong one is reported as such wherever cmdatl runs.
  std::optional<BrokerLookup> named;
  if (request.broker_) {
    named = lookup_broker(*request.broker_);
    if (named->found_ == BrokerFound::NOT_FOUND) {
      return error_reply(Exit::USAGE, "usage.bad_value", named->problem_, brokers_hint());
    }
    if (named->broker() == nullptr) {
      return broker_refusal(*named);
    }
  }
  if (request.env_ && *request.env_ != "demo" && *request.env_ != "real") {
    return error_reply(Exit::USAGE, "usage.bad_value", "--env is demo or real");
  }
  if (named && request.env_ && *request.env_ == "demo" && !named->broker()->has_demo_) {
    return error_reply(
      Exit::USAGE, "usage.bad_value", *request.broker_ + " has no demo environment; its logins are real",
      "leave out --env, or give --env real"
    );
  }
  if (store == nullptr) {
    return no_store_reply();
  }
  const bool terminal = sys::is_terminal(in);

  const Broker* broker = nullptr;
  // The folders offered on a terminal, after the built-in brokers; kept while `broker` points into them.
  std::vector<BrokerLookup> folders;
  if (named) {
    broker = named->broker();
  } else if (terminal) {
    std::vector<std::string> choices;
    for (const auto& entry : brokers()) {
      choices.push_back(entry.display_ + " (" + entry.name_ + ")");
    }
    for (const auto& name : user_broker_names()) {
      auto found = lookup_broker(name);
      if (found.found_ == BrokerFound::FOLDER) {
        choices.push_back(found.folder_->entry_.display_ + " (" + name + ", a broker folder)");
        folders.push_back(std::move(found));
      }
    }
    std::size_t chosen = 0;
    const auto read = sys::choose(in, prompts, "Which broker?", choices, std::nullopt, chosen);
    if (read.status_ == sys::ReadStatus::INVALID) {
      return error_reply(Exit::USAGE, "usage.bad_value", "not one of the choices; nothing changed");
    }
    if (read.status_ != sys::ReadStatus::OK) {
      return no_answer(read.status_);
    }
    broker = chosen < brokers().size() ? &brokers()[chosen] : folders[chosen - brokers().size()].broker();
  } else {
    return error_reply(
      Exit::USAGE, "usage.missing_argument", "--broker is needed when standard input is not a terminal", brokers_hint()
    );
  }

  bool demo = false;
  if (!broker->has_demo_) {
    if (request.env_ && *request.env_ != "real") {
      return error_reply(
        Exit::USAGE, "usage.bad_value", broker->name_ + " has no demo environment; its logins are real",
        "leave out --env, or give --env real"
      );
    }
  } else if (request.env_) {
    demo = *request.env_ == "demo";
  } else if (terminal) {
    std::size_t chosen = 0;
    const auto read =
      sys::choose(in, prompts, "Which environment?", {"demo (모의투자)", "real (실전투자)"}, std::size_t{0}, chosen);
    if (read.status_ == sys::ReadStatus::INVALID) {
      return error_reply(Exit::USAGE, "usage.bad_value", "not one of the choices; nothing changed");
    }
    if (read.status_ != sys::ReadStatus::OK) {
      return no_answer(read.status_);
    }
    demo = chosen == 0;
    if (!demo) {
      const auto confirm = sys::ask(in, prompts, "A real account trades real money. Type real to go on: ", false);
      if (confirm.status_ != sys::ReadStatus::OK) {
        return no_answer(confirm.status_);
      }
      if (confirm.text_ != "real") {
        return error_reply(Exit::FAILED, "auth.not_confirmed", "real was not typed; nothing changed");
      }
    }
  } else {
    return error_reply(
      Exit::USAGE, "usage.missing_argument", "--env demo or --env real is needed when standard input is not a terminal"
    );
  }
  const auto name = login_name(*broker, demo);

  const auto listed = store->list();
  if (listed.status_ != StoreStatus::OK) {
    return store_failure(store->kind(), listed.status_, listed.detail_);
  }
  const bool exists = std::find(listed.names_.begin(), listed.names_.end(), name) != listed.names_.end();
  if (exists && !request.force_) {
    if (!terminal || request.with_secret_) {
      return error_reply(
        Exit::FAILED, "auth.exists", "a login for " + name + " is stored; nothing changed",
        "add --force to replace it, or cmdatl auth logout --broker " + broker->name_ + " --env " +
          (demo ? "demo" : "real") + " first"
      );
    }
    const auto answer = sys::ask(in, prompts, "A login for " + name + " is stored. Replace it? [y/N] ", false);
    if (answer.status_ != sys::ReadStatus::OK) {
      return no_answer(answer.status_);
    }
    if (answer.text_ != "y" && answer.text_ != "Y" && answer.text_ != "yes") {
      return error_reply(Exit::FAILED, "auth.exists", "the stored login for " + name + " was kept; nothing changed");
    }
  }

  json::object document;
  if (request.with_secret_) {
    const auto read = sys::read_input(in, true);
    if (read.status_ != sys::ReadStatus::OK) {
      return no_answer(read.status_);
    }
    boost::system::error_code ec;
    auto parsed = json::parse(read.text_, ec);
    if (ec || !parsed.is_object()) {
      // Neither the text nor the parser's message is repeated: either could hold a secret.
      return bad_document("standard input is not a JSON object");
    }
    document = std::move(parsed.as_object());
  } else if (terminal) {
    prompts << broker->display_ << ", " << (demo ? "demo" : "real") << ":\n";
    for (const auto& field : broker->fields_) {
      const auto answer = sys::ask(in, prompts, field.prompt_, field.secret_);
      if (answer.status_ != sys::ReadStatus::OK) {
        return no_answer(answer.status_);
      }
      if (answer.text_.empty()) {
        if (field.default_) {
          continue;
        }
        return bad_document("the field " + field.json_ + " is empty");
      }
      document[field.json_] = answer.text_;
    }
  } else {
    return error_reply(
      Exit::USAGE, "usage.missing_argument",
      "--with-secret is needed when standard input is not a terminal: the credentials as JSON on standard input",
      "cmdatl help auth login shows each broker's fields"
    );
  }

  json::object canonical;
  if (const auto problem = canonical_document(*broker, demo, document, canonical)) {
    return bad_document(*problem);
  }
  const auto text = json::serialize(canonical);
  if (broker->read_ != nullptr) {
    try {
      broker->read_(text, "stored login " + name);
    } catch (const std::exception& e) {
      // The library's messages name the source and the field, never a value.
      return bad_document(e.what());
    }
  }
  if (interrupted()) {
    return interrupted_auth();
  }
  const auto token_left = exists ? remove_token(*broker, name, *store) : std::nullopt;
  const auto stored = store->put(name, text);
  if (stored.status_ != StoreStatus::OK) {
    return store_failure(store->kind(), stored.status_, stored.detail_, true);
  }
  Reply reply;
  if (broker->read_ == nullptr) {
    const auto folder = brokers_root();
    reply.body_ = folder_login_row(
      broker->name_, demo, folder ? std::optional<std::string>((*folder / broker->name_).string()) : std::nullopt,
      store->kind()
    );
  } else {
    reply.body_ = login_row(*broker, demo, canonical, store->kind());
  }
  reply.body_["replaced"] = exists;
  if (token_left) {
    reply.body_["token_warning"] = *token_left;
  }
  return reply;
}

/**
 * @brief Whether a login's name passes a filter, and its broker and environment.
 * @param name The login's name (`kis/demo`).
 * @param filter The filter.
 * @param broker Where its broker's name goes: a built-in broker's, or a name a broker folder can have (whether the
 *   folder is there or not, so a login outlives its folder in `auth status` and `auth logout`).
 * @param demo Where its environment goes.
 * @return Whether it is such a login and passes.
 */
inline bool matches(const std::string& name, const LoginFilter& filter, std::string& broker, bool& demo) {
  const auto slash = name.find('/');
  if (slash == std::string::npos) {
    return false;
  }
  broker = name.substr(0, slash);
  const auto environment = name.substr(slash + 1);
  if (
    (find_broker(broker) == nullptr && !user_broker_name(broker)) || (environment != "demo" && environment != "real")
  ) {
    return false;
  }
  demo = environment == "demo";
  return (!filter.broker_ || *filter.broker_ == broker) && (!filter.env_ || *filter.env_ == environment);
}

/**
 * @brief The refusal of a filter naming no broker cmdatl has, or an environment other than demo or real.
 * @param filter The filter.
 * @return The refusal, or none.
 */
inline std::optional<Reply> filter_problem(const LoginFilter& filter) {
  if (filter.broker_ && find_broker(*filter.broker_) == nullptr && !user_broker_name(*filter.broker_)) {
    return error_reply(Exit::USAGE, "usage.bad_value", "no broker " + *filter.broker_, brokers_hint());
  }
  if (filter.env_ && *filter.env_ != "demo" && *filter.env_ != "real") {
    return error_reply(Exit::USAGE, "usage.bad_value", "--env is demo or real");
  }
  return std::nullopt;
}

/**
 * @brief `auth status`: the stored logins, masked, and which brokers' environment variables are set. Sends nothing.
 * @param filter Which logins.
 * @param store The credential store, or null where there is none.
 * @return The reply.
 */
inline Reply status(const LoginFilter& filter, CredentialStore* store) {
  if (auto problem = filter_problem(filter)) {
    return std::move(*problem);
  }
  Reply reply;
  json::object variables;
  for (const auto& broker : brokers()) {
    json::array set;
    for (const auto& variable : broker.env_vars_) {
      if (std::getenv(variable.c_str()) != nullptr) {
        set.emplace_back(variable);
      }
    }
    variables[broker.name_] = std::move(set);
  }
  reply.body_["environment_variables"] = std::move(variables);
  json::array rows;
  if (store == nullptr) {
    reply.body_["store"] = nullptr;
    reply.body_["logins"] = std::move(rows);
    return reply;
  }
  const auto listed = store->list();
  if (listed.status_ != StoreStatus::OK) {
    return store_failure(store->kind(), listed.status_, listed.detail_);
  }
  for (const auto& name : listed.names_) {
    std::string broker_name;
    bool demo = false;
    if (!matches(name, filter, broker_name, demo)) {
      continue;
    }
    const auto* broker = find_broker(broker_name);
    if (broker == nullptr) {
      // A broker folder's login: its document is not read, only whether the folder still reads.
      const auto folder = lookup_broker(broker_name);
      rows.emplace_back(folder_login_row(
        broker_name, demo,
        folder.found_ == BrokerFound::FOLDER ?
          std::optional<std::string>(folder.folder_->folder_.string()) :
          std::nullopt,
        store->kind()
      ));
      continue;
    }
    const auto found = store->get(name);
    if (found.status_ == StoreStatus::NOT_FOUND) {
      continue;
    }
    if (found.status_ == StoreStatus::UNAVAILABLE) {
      return store_failure(store->kind(), found.status_, found.detail_);
    }
    json::object row;
    if (found.status_ == StoreStatus::OK) {
      boost::system::error_code ec;
      const auto parsed = json::parse(found.value_, ec);
      row = login_row(*broker, demo, !ec && parsed.is_object() ? parsed.as_object() : json::object{}, store->kind());
    } else {
      row["broker"] = broker->name_;
      row["environment"] = demo ? "demo" : "real";
      row["store"] = store->kind();
      row["checked"] = false;
      row["locked"] = true;
    }
    rows.emplace_back(std::move(row));
  }
  reply.body_["store"] = store->kind();
  reply.body_["logins"] = std::move(rows);
  return reply;
}

/**
 * @brief `auth logout`: removes one stored login, its saved access token first as far as it can be; on a terminal, asks
 *   which when several match.
 * @param filter Which login.
 * @param in Standard input.
 * @param prompts Where questions go (stderr).
 * @param store The credential store, or null where there is none.
 * @return The reply.
 */
inline Reply logout(const LoginFilter& filter, int in, std::ostream& prompts, CredentialStore* store) {
  if (auto problem = filter_problem(filter)) {
    return std::move(*problem);
  }
  if (store == nullptr) {
    return no_store_reply();
  }
  const auto listed = store->list();
  if (listed.status_ != StoreStatus::OK) {
    return store_failure(store->kind(), listed.status_, listed.detail_);
  }
  std::vector<std::string> names;
  for (const auto& name : listed.names_) {
    std::string broker;
    bool demo = false;
    if (matches(name, filter, broker, demo)) {
      names.push_back(name);
    }
  }
  if (names.empty()) {
    return error_reply(
      Exit::FAILED, "auth.not_found", "no stored login matches; nothing changed", "cmdatl auth status"
    );
  }
  std::string name = names.front();
  if (names.size() > 1) {
    if (!sys::is_terminal(in)) {
      return error_reply(
        Exit::USAGE, "usage.missing_argument", std::to_string(names.size()) + " stored logins match; nothing changed",
        "give --broker and --env"
      );
    }
    std::size_t chosen = 0;
    const auto read = sys::choose(in, prompts, "Which login?", names, std::nullopt, chosen);
    if (read.status_ == sys::ReadStatus::INVALID) {
      return error_reply(Exit::USAGE, "usage.bad_value", "not one of the choices; nothing changed");
    }
    if (read.status_ != sys::ReadStatus::OK) {
      return no_answer(read.status_);
    }
    name = names[chosen];
  }
  if (interrupted()) {
    return interrupted_auth();
  }
  const auto* broker = find_broker(name.substr(0, name.find('/')));
  const auto token_left = broker != nullptr ? remove_token(*broker, name, *store) : std::nullopt;
  const auto removed = store->remove(name);
  if (removed.status_ == StoreStatus::NOT_FOUND) {
    return error_reply(Exit::FAILED, "auth.not_found", "no stored login " + name + "; nothing changed");
  }
  if (removed.status_ != StoreStatus::OK) {
    return store_failure(store->kind(), removed.status_, removed.detail_, true);
  }
  Reply reply;
  reply.body_["removed"] = name;
  if (token_left) {
    reply.body_["token_warning"] = *token_left;
  }
  return reply;
}

/**
 * @brief The value of an option, if given.
 * @param invocation The command line.
 * @param name The option.
 * @return Its value, or none.
 */
inline std::optional<std::string> option_of(const Invocation& invocation, const std::string& name) {
  if (!invocation.has(name)) {
    return std::nullopt;
  }
  return invocation.get(name);
}

/** @brief Standard input's descriptor. */
inline constexpr int STANDARD_INPUT = 0;

/**
 * @brief The `auth login` command: the system's store, standard input, questions on stderr.
 * @param invocation The command line.
 * @param output Where questions and the reply go.
 * @return The reply.
 */
inline Reply login_command(const Invocation& invocation, Output& output) {
  LoginRequest request;
  request.broker_ = option_of(invocation, "broker");
  request.env_ = option_of(invocation, "env");
  request.with_secret_ = invocation.has("with-secret");
  request.force_ = invocation.has("force");
  const auto store = open_store(interaction_allowed(STANDARD_INPUT));
  return login(request, STANDARD_INPUT, output.err(), store.get());
}

/**
 * @brief The `auth status` command.
 * @param invocation The command line.
 * @return The reply.
 */
inline Reply status_command(const Invocation& invocation) {
  const LoginFilter filter{option_of(invocation, "broker"), option_of(invocation, "env")};
  const auto store = open_store(interaction_allowed(STANDARD_INPUT));
  return status(filter, store.get());
}

/**
 * @brief The `auth logout` command.
 * @param invocation The command line.
 * @param output Where questions and the reply go.
 * @return The reply.
 */
inline Reply logout_command(const Invocation& invocation, Output& output) {
  const LoginFilter filter{option_of(invocation, "broker"), option_of(invocation, "env")};
  const auto store = open_store(interaction_allowed(STANDARD_INPUT));
  return logout(filter, STANDARD_INPUT, output.err(), store.get());
}

} // namespace auth
} // namespace cmdatl
