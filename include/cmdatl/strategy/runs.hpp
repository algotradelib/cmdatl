#pragma once

// `run`: a strategy on its own mock market, in the caller's foreground.
//
// `run` reads and checks its settings (`settings.hpp`), makes the run's directory (`<state root>/runs/<account>/<run
// id>`, new), builds the program's driver into it (`build.hpp`), and replaces itself with the driver (`execve`), so the
// driver is the process the caller started: its events and its reply go to the caller's stdout, a signal reaches it
// directly, and its exit is the command's. cmdatl holds no lock across the exec; the driver takes the run's own.

#include "../account/account.hpp"
#include "../account/operator.hpp"
#include "../auth/login.hpp"
#include "../core/errors.hpp"
#include "../core/reply.hpp"
#include "../core/runs.hpp"
#include "../core/table.hpp"
#include "../system/credential_pipe.hpp"
#include "../system/files.hpp"
#include "../system/process.hpp"
#include "./atlc.hpp"
#include "./build.hpp"
#include "./settings.hpp"
#include "./spec.hpp"
#include "./strategy.hpp"

#include <boost/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace runs {

namespace json = boost::json;

/**
 * @brief The catalog's `next` for a code.
 * @param code The code.
 * @return The text, or empty.
 */
inline std::string next_for(const std::string& code) {
  for (const auto& entry : error_catalog()) {
    if (entry.code_ == code) {
      return entry.next_;
    }
  }
  return {};
}

/**
 * @brief Whether a text is an offset as a run takes it: digits, a point, digits, below 1 (such as `0.01`).
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_offset(const std::string& text) {
  static const std::regex grammar("0(\\.[0-9]{1,8})?|\\.[0-9]{1,8}");
  return text.size() <= 10 && std::regex_match(text, grammar);
}

/**
 * @brief A fresh run id: `r` and 12 lowercase hex digits from 6 random bytes; no clock in it.
 * @return The id, or nothing when no random bytes could be read.
 */
inline std::optional<std::string> new_run_id() {
#if defined(_WIN32)
  return std::nullopt;
#else
  ScopedFd fd(open_lifted("/dev/urandom", O_RDONLY | O_CLOEXEC));
  unsigned char bytes[6];
  if (fd.get() < 0 || ::read(fd.get(), bytes, sizeof bytes) != static_cast<ssize_t>(sizeof bytes)) {
    return std::nullopt;
  }
  char text[16];
  std::snprintf(
    text, sizeof text, "r%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]
  );
  return std::string(text);
#endif
}

#if !defined(_WIN32)
/** @brief Removes a run's directory when `run` fails before the driver could start. */
class DirGuard {
public:
  /**
   * @brief Guards a directory.
   * @param dir It.
   */
  explicit DirGuard(std::filesystem::path dir) : dir_(std::move(dir)) {}

  DirGuard(const DirGuard&) = delete;
  DirGuard& operator=(const DirGuard&) = delete;

  /** @brief Removes the directory. */
  ~DirGuard() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

private:
  std::filesystem::path dir_;
};
#endif

/**
 * @brief Reads the logins a run on a broker's account needs, in this process (the store's access list names `cmdatl`):
 *   the account's (`<broker>/<env>`), then on a KIS demo account the real login, read only for market data (a KIS demo
 *   host refuses reference data and the calendar); an empty document for none.
 * @param config The operator's file, for a broker's account.
 * @param store The credential store, or null where the system has none.
 * @param documents Set to the documents, in that order.
 * @return The refusal, or nothing.
 */
inline std::optional<Reply> read_run_logins(
  const OperatorConfig& config, auth::CredentialStore* store, std::vector<std::string>& documents
) {
  const auto* broker = auth::find_broker(config.broker_);
  if (broker == nullptr) {
    return error_reply(Exit::FAILED, "unsupported.broker", "cmdatl run does not trade " + config.broker_);
  }
  if (store == nullptr) {
    return auth::no_store_reply();
  }
  const bool demo = config.env_ == "demo";
  const auto read =
    [&store](const std::string& name, const std::string& message, const std::string& login, std::string& out) {
      const auto answer = store->get(name);
      if (answer.status_ == auth::StoreStatus::OK) {
        out = answer.value_;
        return std::optional<Reply>();
      }
      if (answer.status_ == auth::StoreStatus::NOT_FOUND) {
        return std::optional<Reply>(error_reply(Exit::FAILED, "auth.not_found", message, "the operator runs " + login));
      }
      return std::optional<Reply>(auth::store_failure(store->kind(), answer.status_, answer.detail_));
    };
  std::string account;
  const auto own = auth::login_name(*broker, demo);
  if (
    auto refused = read(
      own, "no login " + own + " in the credential store",
      "cmdatl auth login --broker " + config.broker_ + " --env " + config.env_, account
    )
  ) {
    return refused;
  }
  std::string market;
  if (config.broker_ == "kis" && demo) {
    if (
      auto refused = read(
        auth::login_name(*broker, false),
        "a run on a KIS demo account reads market data with the real login (a demo host refuses reference data and the "
        "calendar), and there is none; it is only read, never traded",
        "cmdatl auth login --broker kis --env real", market
      )
    ) {
      return refused;
    }
  }
  documents = {std::move(account), std::move(market)};
  return std::nullopt;
}

/**
 * @brief `run TARGET [KEY=VALUE]...`; see the file comment. Returns only when the driver could not be started.
 * @param invocation The arguments.
 * @param snapshot The operator's file, read and `OK`.
 * @param output Where progress goes.
 * @return The reply, when nothing was started.
 */
inline Reply run_reply(const Invocation& invocation, const ConfigSnapshot& snapshot, Output& output) {
#if defined(_WIN32)
  (void)invocation;
  (void)snapshot;
  (void)output;
  return error_reply(Exit::FAILED, "unavailable.platform", "runs run on macOS and Linux only in this version");
#else
  const auto& config = *snapshot.config_;
  const bool broker = config.broker_ != "paper";
  GivenSettings given;
  if (auto refused = sort_words(target_and_settings(invocation), given)) {
    return std::move(*refused);
  }
  output.progress("cmdatl: starting a run of " + given.target_);
  if (interrupted()) {
    return interrupted_reply();
  }
  // Until the exec, a failure is the command's own (exit 1, nothing sent) and the run's directory is removed.
  try {
    if (auto refused = read_given(given)) {
      return std::move(*refused);
    }
    const auto atlc = find_atlc(&snapshot);
    const std::string install = "install atlc, or name it with CMDATL_ATLC; cmdatl env shows what was found";
    if (atlc.path_.empty() || atlc.version_.empty()) {
      return error_reply(
        Exit::FAILED, "atlc.missing",
        atlc.path_.empty() ? "atlc was not found" : atlc.path_ + " did not answer --version", install
      );
    }
    if (!at_least(atlc.version_, MINIMUM_ATLC)) {
      return error_reply(
        Exit::FAILED, "atlc.unsupported", "atlc " + atlc.version_ + " is not a version cmdatl reads",
        "install a newer atlc"
      );
    }
    CheckedSettings checked;
    if (auto refused = check_given(given, atlc.path_, output, true, checked, broker)) {
      return std::move(*refused);
    }
    const auto& resolved = checked.resolved_;
    if (resolved.store_) {
      return error_reply(
        Exit::USAGE, "spec.invalid", "a run reads files, not a store: give [bars] a file", "fix the run spec"
      );
    }
    // A run keeps the offset grammar its collar pre-check compares (at most 8 decimals).
    if (!resolved.offset_.empty() && !valid_offset(resolved.offset_)) {
      return error_reply(
        Exit::USAGE, "spec.invalid",
        "orders.offset = " + resolved.offset_ + ": a run takes an offset of at most 8 decimals below 1",
        "fix the run spec or the settings given on the command line"
      );
    }
    std::vector<std::string> args;
    for (const auto& [key, value] : resolved.args_) {
      args.push_back(key + "=" + value);
    }
    // A market-price rule prices a replay's fills; a run's fills are the market's, so a rule would only mislead.
    if (!resolved.fill_header_.empty()) {
      auto reply = error_reply(
        Exit::USAGE, "spec.invalid",
        "a market-price rule prices a backtest's fills; a run's fills are the market's: remove [replay] fill_header "
        "and fill_function",
        "fix the run spec"
      );
      reply.body_["key"] = std::string("replay.fill_header");
      return reply;
    }
    // The early refusals, with nothing built; the driver checks each again, and its checks are what count.
    if (const auto refusal = spec_refusal(args)) {
      auto reply = error_reply(Exit::USAGE, "spec.invalid", refusal->second, "fix the run spec");
      reply.body_["key"] = refusal->first;
      return reply;
    }
    for (const auto& arg : args) {
      if (arg.rfind("instrument.", 0) == 0) {
        const auto symbol = arg.substr(arg.find('=') + 1);
        if (!config.allows(symbol)) {
          auto reply = error_reply(
            Exit::REFUSED, "refused.symbol_not_allowed", symbol + " is not in the operator's limits.symbols",
            next_for("refused.symbol_not_allowed")
          );
          reply.body_["symbol"] = symbol;
          return reply;
        }
      }
    }
    if (config.limits_.order_type_ == "limit" && resolved.order_type_ != "limit") {
      return error_reply(
        Exit::REFUSED, "refused.order_type",
        "the run spec's [orders] type is " + resolved.order_type_ + "; the operator allows limit orders only",
        next_for("refused.order_type")
      );
    }
    if (!resolved.offset_.empty() && !(std::stod(resolved.offset_) <= config.limits_.collar_)) {
      return error_reply(
        Exit::REFUSED, "refused.collar",
        "the run spec's [orders] offset " + resolved.offset_ + " is above the operator's collar " +
          decimal_text(config.limits_.collar_),
        next_for("refused.collar")
      );
    }
    std::error_code ec;
    if (!std::filesystem::exists(config.armed_path(), ec)) {
      return error_reply(
        Exit::REFUSED, "refused.not_armed", "the operator's armed file does not exist", next_for("refused.not_armed")
      );
    }
    std::vector<std::string> logins;
    if (broker) {
      const auto store = auth::open_store(auth::interaction_allowed(0));
      if (auto refused = read_run_logins(config, store.get(), logins)) {
        return std::move(*refused);
      }
    }
    const auto id = new_run_id();
    if (!id) {
      return error_reply(Exit::FAILED, "run.failed", "no random bytes for the run's id", next_for("run.failed"), true);
    }
    const auto where = run_dir(config, *id);
    if (!where) {
      return error_reply(
        Exit::FAILED, "state.unavailable", "neither XDG_STATE_HOME nor HOME names an absolute directory"
      );
    }
    const std::filesystem::path dir = *where;
    if (make_private_dirs(dir.parent_path()) != 0 || ::mkdir(dir.c_str(), 0700) != 0) {
      return error_reply(Exit::FAILED, "file.unwritable", "the run's directory " + dir.string() + " cannot be made");
    }
    DirGuard guard(dir);
    build::Request request{
      atlc.path_, atlc.version_, given.program_, checked.info_.program_.struct_, resolved.indicators_
    };
    request.fill_header_ = resolved.fill_header_;
    request.fill_function_ = resolved.fill_function_;
    if (broker) {
      request.broker_ = config.broker_;
    }
    const auto progress = [&output](const std::string& line) {
      output.progress(line);
    };
    if (auto failed = build::build_driver(request, dir, progress)) {
      return std::move(*failed);
    }
    const auto driver = (dir / "driver").string();
    std::vector<std::string> argv_owned{driver, "run.id=" + *id};
    argv_owned.insert(argv_owned.end(), args.begin(), args.end());
    std::vector<std::string> envp_owned;
    for (auto& entry : build::own_environment()) {
      if (entry.rfind("CMDATL_CONFIG=", 0) != 0) {
        envp_owned.push_back(std::move(entry));
      }
    }
    envp_owned.push_back("CMDATL_CONFIG=" + config.path_);
    std::vector<char*> argv;
    for (auto& arg : argv_owned) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& entry : envp_owned) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    output.progress("cmdatl: run " + *id + " in " + dir.string());
    std::cout.flush();
    std::cerr.flush();
    // The stops are blocked from the last check through the exec: one arriving in between stays pending, and the
    // driver receives it once it has noted its own handlers (`handle_signals` unblocks them).
    const sigset_t stops = stop_signals();
    sigset_t before;
    ::sigprocmask(SIG_BLOCK, &stops, &before);
    if (interrupted()) {
      ::sigprocmask(SIG_SETMASK, &before, nullptr);
      return interrupted_reply();
    }
    // A broker's logins on descriptor 3, staged last so no build child inherited them.
    if (broker) {
      const auto payload = sys::frame_credentials(logins);
      const int staged = payload ? sys::stage_credentials(*payload) : EMSGSIZE;
      if (staged != 0) {
        ::sigprocmask(SIG_SETMASK, &before, nullptr);
        return error_reply(
          Exit::FAILED, "run.failed",
          "the logins could not be handed to the driver: " + std::string(std::strerror(staged)), next_for("run.failed"),
          true
        );
      }
    }
    ::execve(driver.c_str(), argv.data(), envp.data());
    const int error = errno;
    if (broker) {
      ::close(sys::CREDENTIAL_FD);
    }
    ::sigprocmask(SIG_SETMASK, &before, nullptr);
    return error_reply(
      Exit::FAILED, "run.failed", "the driver could not be started: " + std::string(std::strerror(error)),
      next_for("run.failed"), true
    );
  } catch (const std::exception& error) {
    return error_reply(
      Exit::FAILED, "internal.error", error.what(), "nothing was started; report the defect with the command line"
    );
  }
#endif
}

} // namespace runs
} // namespace cmdatl
