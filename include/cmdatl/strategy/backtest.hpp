#pragma once

// `strategy backtest`: the settings read and checked against the program (`settings.hpp`), the backtest driver built
// once per program shape (`build.hpp`, a driver whose main is `cmdatl::replay::main`), a copy of it run with the
// settings as its arguments, and its report returned unchanged.

#include "../auth/login.hpp"
#include "../core/reply.hpp"
#include "../core/table.hpp"
#include "../system/credential_pipe.hpp"
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
/**
 * @brief A login written into a pipe whose read end this holds, on a descriptor above 3 closed on exec; the child gets
 *   it as descriptor 3 (`run_child`), and this process's own descriptor 3 is never touched. Closed when its owner
 *   goes, on every path.
 */
class StagedLogin {
public:
  StagedLogin() = default;
  StagedLogin(const StagedLogin&) = delete;
  StagedLogin& operator=(const StagedLogin&) = delete;

  /** @brief Closes the read end. */
  ~StagedLogin() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /**
   * @brief Writes the framed login into a new pipe and keeps its read end.
   * @param document The login.
   * @return 0, or the error number.
   */
  int stage(const std::string& document) {
    const auto payload = sys::frame_credentials({document});
    if (!payload) {
      return EMSGSIZE;
    }
    int ends[2] = {-1, -1};
    if (::pipe(ends) != 0) {
      return errno;
    }
    // Above 3, so mapping it onto 3 in the child is never a no-op that leaves it closed on exec.
    fd_ = ::fcntl(ends[0], F_DUPFD_CLOEXEC, 4);
    const int lifted = errno;
    ::close(ends[0]);
    if (fd_ < 0) {
      ::close(ends[1]);
      return lifted;
    }
    std::size_t written = 0;
    const int error = write_whole(ends[1], payload->data(), payload->size(), written);
    ::close(ends[1]);
    return error;
  }

  /**
   * @brief The read end.
   * @return The descriptor, or -1 before `stage`.
   */
  int fd() const noexcept {
    return fd_;
  }

private:
  int fd_{-1};
};

/**
 * @brief Reads a backtest's outcome file written by the fetch: one JSON object.
 * @param path The file.
 * @return It, or none.
 */
inline std::optional<json::object> read_fetch_outcome(const std::filesystem::path& path) {
  std::string text;
  if (!build::read_file(path, text) || text.size() > (1u << 20)) {
    return std::nullopt;
  }
  boost::system::error_code ec;
  auto parsed = json::parse(text, ec);
  if (ec || !parsed.is_object()) {
    return std::nullopt;
  }
  return std::move(parsed.as_object());
}

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
  // A broker folder's history: the folder found and its files read once, its options checked against its
  // declarations, its login read and checked against that folder, and the files compiled as read.
  const auto& fetching = checked.resolved_.fetch_source_;
  std::optional<auth::AdapterSnapshot> adapter;
  std::string login;
  std::string options = "{}";
  if (!fetching.empty()) {
    auto found = auth::lookup_broker(fetching, checked.resolved_.fetch_adapter_);
    if (found.found_ != auth::BrokerFound::FOLDER) {
      return auth::broker_refusal(found);
    }
    const auto& folder = *found.folder_;
    const auto errors = spec::fetch_options(checked.resolved_, folder.options_, options);
    if (!errors.empty()) {
      return settings_refusal(errors, given.spec_);
    }
    const auto name = auth::login_name(folder.entry_, false);
    auth::StoreAnswer answer;
    std::string kind = "store";
    bool hooked = false;
#if defined(CMDATL_RUN_TEST_HOOKS)
    // Test builds only: the login from a file, and the system's store never opened.
    if (const auto file = variable("CMDATL_TEST_FETCH_LOGIN")) {
      hooked = true;
      answer.status_ = build::read_file(*file, answer.value_) ? auth::StoreStatus::OK : auth::StoreStatus::NOT_FOUND;
    }
#endif
    if (!hooked) {
      const auto store = auth::open_store(auth::interaction_allowed(0));
      if (store == nullptr) {
        return auth::no_store_reply();
      }
      kind = store->kind();
      answer = store->get(name);
    }
    if (answer.status_ == auth::StoreStatus::NOT_FOUND) {
      return error_reply(
        Exit::FAILED, "auth.not_found", "no login " + name + " in the credential store: the backtest fetches with it",
        "the operator runs cmdatl auth login --broker " + fetching + " --adapter " + folder.folder_.string()
      );
    }
    if (answer.status_ != auth::StoreStatus::OK) {
      return auth::store_failure(kind, answer.status_, answer.detail_);
    }
    if (auto refused = auth::folder_login(answer.value_, name, folder, false, login)) {
      return std::move(*refused);
    }
    output.progress(
      "cmdatl: fetching through the broker folder " + folder.snapshot_.folder_ + " (" +
      build::adapter_digest(folder.snapshot_) + ")"
    );
    adapter = std::move(found.folder_->snapshot_);
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
    request.fill_header_ = checked.resolved_.fill_header_;
    request.fill_function_ = checked.resolved_.fill_function_;
    request.header_ = checked.resolved_.header_;
    request.adapter_ = adapter;
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
  std::filesystem::path outcome;
  for (const auto& [key, value] : checked.resolved_.args_) {
    // A broker folder's history is read from the store it is fetched into; how many bars and of which interval are
    // the fetch's.
    if (!fetching.empty() && key == "bars.source") {
      argv.push_back("bars.source=store");
    } else if (!fetching.empty() && (key == "bars.count" || key == "bars.interval")) {
      argv.push_back("fetch." + key.substr(5) + "=" + value);
    } else {
      argv.push_back(key + "=" + value);
    }
  }
  if (!fetching.empty()) {
    outcome = (scratch ? scratch->path() : std::filesystem::temp_directory_path()) / "fetch.json";
    argv.push_back("fetch.source=" + fetching);
    argv.push_back("fetch.options=" + options);
    argv.push_back("fetch.outcome=" + outcome.string());
  }
  std::string fills;
  if (invocation.has("fills")) {
    fills = std::filesystem::absolute(invocation.get("fills")).string();
    argv.push_back("output.fills=" + fills);
  }
  // The login on descriptor 3 for the driver only: a pipe made after the build (no build child sees it), mapped onto 3
  // in the driver alone, and closed here once the driver has ended, on every path.
  StagedLogin staged;
  if (!fetching.empty()) {
    if (const int error = staged.stage(login)) {
      return error_reply(
        Exit::FAILED, "replay.failed",
        "the login could not be handed to the driver: " + std::string(std::strerror(error))
      );
    }
  }
  const auto ran = run_child(
    argv,
    [&output](const std::string& line) {
      output.progress(line);
    },
    nullptr, staged.fd()
  );
  std::optional<json::object> fetched;
  if (!fetching.empty()) {
    fetched = read_fetch_outcome(outcome);
    if (ran.kind_ == ChildResult::Kind::FINISHED && !ran.signaled_ && ran.code_ == 3) {
      std::string message = "the fetch through " + fetching + " failed";
      const json::object* error = nullptr;
      if (fetched && fetched->contains("error") && fetched->at("error").is_object()) {
        error = &fetched->at("error").as_object();
      }
      if (error != nullptr && error->contains("message") && error->at("message").is_string()) {
        message += ": " + std::string(error->at("message").as_string());
      }
      auto reply = error_reply(
        Exit::FAILED, "fetch.failed", message,
        "check the login (cmdatl auth status) and the broker folder; bars already fetched stay in the store"
      );
      if (fetched) {
        reply.body_["fetched"] = std::move(*fetched);
      }
      return reply;
    }
  }
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
  if (fetched) {
    // The history is the vendor's as of the fetch, revisions included; the store as of it can hold more than it.
    (*fetched)["caveat"] = std::string("the bars are the broker folder's history as of fetched_at, revised values ") +
      "included; bars.source = \"store\" with bars.as_of = fetched_at reads the store as it stood after this fetch, " +
      "which can hold bars this fetch did not return (an older history, or bars an earlier fetch had)";
    reply.body_["fetched"] = std::move(*fetched);
  }
  return reply;
#endif
}

} // namespace cmdatl
