#include <cmdatl/auth/login.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

using namespace support;

namespace {

namespace auth = cmdatl::auth;
namespace json = boost::json;
using cmdatl::Exit;
using cmdatl::Reply;

/** @brief A store in memory, whose answers a test can turn into locked or unavailable ones. */
class MemoryStore : public auth::CredentialStore {
public:
  /** @brief The logins. */
  std::map<std::string, std::string> items_;
  /** @brief An answer every call gives instead of doing its work, unless `OK`. */
  auth::StoreStatus fail_{auth::StoreStatus::OK};
  /** @brief An answer only `put` gives, unless `OK`. */
  auth::StoreStatus fail_put_{auth::StoreStatus::OK};
  /** @brief An answer only `get` gives, unless `OK`. */
  auth::StoreStatus fail_get_{auth::StoreStatus::OK};
  /** @brief How many times `put` was called. */
  int puts_{0};

  std::string kind() const override {
    return "memory";
  }

  auth::StoreList list() override {
    auth::StoreList result;
    result.status_ = fail_;
    if (fail_ != auth::StoreStatus::OK) {
      result.detail_ = "test";
      return result;
    }
    for (const auto& item : items_) {
      result.names_.push_back(item.first);
    }
    return result;
  }

  auth::StoreAnswer get(const std::string& name) override {
    if (fail_ != auth::StoreStatus::OK || fail_get_ != auth::StoreStatus::OK) {
      return {fail_ != auth::StoreStatus::OK ? fail_ : fail_get_, {}, "test"};
    }
    const auto found = items_.find(name);
    if (found == items_.end()) {
      return {auth::StoreStatus::NOT_FOUND, {}, {}};
    }
    return {auth::StoreStatus::OK, found->second, {}};
  }

  auth::StoreAnswer put(const std::string& name, const std::string& value) override {
    ++puts_;
    if (fail_ != auth::StoreStatus::OK || fail_put_ != auth::StoreStatus::OK) {
      return {fail_ != auth::StoreStatus::OK ? fail_ : fail_put_, {}, "test"};
    }
    items_[name] = value;
    return {auth::StoreStatus::OK, {}, {}};
  }

  auth::StoreAnswer remove(const std::string& name) override {
    if (fail_ != auth::StoreStatus::OK) {
      return {fail_, {}, "test"};
    }
    return {items_.erase(name) > 0 ? auth::StoreStatus::OK : auth::StoreStatus::NOT_FOUND, {}, {}};
  }
};

/**
 * @brief A store in files, one per login, so a forked child's login can be read back by the parent.
 */
class FileStore : public auth::CredentialStore {
public:
  /**
   * @brief A store in a directory.
   * @param dir The directory.
   */
  explicit FileStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

  std::string kind() const override {
    return "files";
  }

  auth::StoreList list() override {
    auth::StoreList result;
    result.status_ = auth::StoreStatus::OK;
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
      auto name = entry.path().filename().string();
      std::replace(name.begin(), name.end(), '_', '/');
      result.names_.push_back(name);
    }
    std::sort(result.names_.begin(), result.names_.end());
    return result;
  }

  auth::StoreAnswer get(const std::string& name) override {
    const auto path = file(name);
    if (!std::filesystem::exists(path)) {
      return {auth::StoreStatus::NOT_FOUND, {}, {}};
    }
    return {auth::StoreStatus::OK, slurp(path), {}};
  }

  auth::StoreAnswer put(const std::string& name, const std::string& value) override {
    spit(file(name), value);
    return {auth::StoreStatus::OK, {}, {}};
  }

  auth::StoreAnswer remove(const std::string& name) override {
    return {std::filesystem::remove(file(name)) ? auth::StoreStatus::OK : auth::StoreStatus::NOT_FOUND, {}, {}};
  }

private:
  std::filesystem::path dir_;

  std::filesystem::path file(std::string name) const {
    std::replace(name.begin(), name.end(), '/', '_');
    return dir_ / name;
  }
};

/** @brief A pipe whose read end holds a text and then ends, as standard input that is not a terminal. */
class Input {
public:
  /**
   * @brief Writes the text into a new pipe and closes its write end.
   * @param text The text.
   */
  explicit Input(const std::string& text = "") {
    int ends[2] = {-1, -1};
    REQUIRE(::pipe(ends) == 0);
    REQUIRE(::write(ends[1], text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    ::close(ends[1]);
    fd_ = ends[0];
  }

  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;

  /** @brief Closes the read end. */
  ~Input() {
    ::close(fd_);
  }

  /**
   * @brief The read end.
   * @return The descriptor.
   */
  int fd() const noexcept {
    return fd_;
  }

private:
  int fd_{-1};
};

const std::string KEY = "KEY-0123456789";
const std::string SECRET = "SECRET-9876543210";

/** @brief Every reply and prompt text a test saw, for the secret sweep. */
std::vector<std::string>& seen() {
  static std::vector<std::string> texts;
  return texts;
}

/**
 * @brief Runs `auth login` with flags and a document on standard input (not a terminal).
 * @param store The store.
 * @param broker `--broker`.
 * @param env `--env`, or empty for none.
 * @param document The document.
 * @param force `--force`.
 * @return The reply.
 */
Reply login_with(
  MemoryStore& store, const std::string& broker, const std::string& env, const std::string& document, bool force = false
) {
  auth::LoginRequest request;
  request.broker_ = broker;
  if (!env.empty()) {
    request.env_ = env;
  }
  request.with_secret_ = true;
  request.force_ = force;
  const Input in(document);
  std::ostringstream prompts;
  auto reply = auth::login(request, in.fd(), prompts, &store);
  seen().push_back(json::serialize(reply.body_));
  seen().push_back(prompts.str());
  return reply;
}

std::string code(const Reply& reply) {
  const auto* error = reply.body_.if_contains("error");
  return error == nullptr ? "" : std::string(error->as_object().at("code").as_string());
}

std::string kis_document(const std::string& extra = "") {
  return R"({"app_key":")" + KEY + R"(","app_secret":")" + SECRET + R"(","account_number":"12345678")" + extra + "}";
}

} // namespace

TEST_CASE("auth login stores each broker's canonical document, masked in the reply") {
  MemoryStore store;
  auto reply = login_with(store, "kis", "demo", kis_document());
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(
    store.items_.at("kis/demo") ==
    R"({"app_key":")" + KEY + R"(","app_secret":")" + SECRET + R"(","account_number":"12345678","demo":true})"
  );
  CHECK(reply.body_.at("broker").as_string() == "kis");
  CHECK(reply.body_.at("environment").as_string() == "demo");
  CHECK(reply.body_.at("key").as_string() == "KE****");
  CHECK(reply.body_.at("account").as_string() == "12****");
  CHECK(reply.body_.at("checked").as_bool() == false);
  CHECK(reply.body_.at("replaced").as_bool() == false);

  reply = login_with(store, "kis", "real", kis_document(R"(,"account_product_code":"22","demo":false)"));
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(store.items_.at("kis/real").find(R"("account_product_code":"22","demo":false)") != std::string::npos);

  reply = login_with(store, "kiwoom", "demo", R"({"appkey":")" + KEY + R"(","secretkey":")" + SECRET + R"("})");
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(store.items_.at("kiwoom/demo") == R"({"appkey":")" + KEY + R"(","secretkey":")" + SECRET + R"(","demo":true})");
  CHECK_FALSE(reply.body_.contains("account"));

  // Toss has no demo: no --env needed, real only, and no demo field stored.
  reply = login_with(store, "toss", "", R"({"client_id":")" + KEY + R"(","client_secret":")" + SECRET + R"("})");
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(reply.body_.at("environment").as_string() == "real");
  CHECK(store.items_.at("toss/real") == R"({"client_id":")" + KEY + R"(","client_secret":")" + SECRET + R"("})");
}

TEST_CASE("auth login refuses a document that is not the broker's, and stores nothing") {
  const std::vector<std::pair<std::string, std::string>> refused{
    {"kis", kis_document(R"(,"access_token":"t")")},
    {"kis", kis_document(R"(,"demo":"true")")},
    {"kis", kis_document(R"(,"demo":false)")},
    {"kis", R"({"app_key":")" + KEY + R"(","app_secret":")" + SECRET + R"("})"},
    {"kis", R"({"app_key":")" + KEY + R"(","app_secret":")" + SECRET + R"(","account_number":"1234"})"},
    {"kis", kis_document(R"(,"account_product_code":"7")")},
    {"kis", R"({"app_key":")" + KEY + R"(","app_secret":7,"account_number":"12345678"})"},
    {"kis", R"({"app_key":"","app_secret":")" + SECRET + R"(","account_number":"12345678"})"},
    {"kiwoom", R"({"appkey":")" + KEY + R"(","secretkey":")" + SECRET + R"(","account_number":"12345678"})"},
    {"toss", R"({"client_id":")" + KEY + R"(","client_secret":")" + SECRET + R"(","demo":false})"},
    {"toss", R"({")" + SECRET + R"(":1,"client_id":")" + KEY + R"(","client_secret":"x"})"},
    {"kis", "not json " + SECRET},
    {"kis", R"([")" + SECRET + R"("])"},
  };
  for (const auto& [broker, document] : refused) {
    INFO(broker << " " << document);
    MemoryStore store;
    const auto reply = login_with(store, broker, broker == "toss" ? "" : "demo", document);
    CHECK(reply.exit_ == Exit::FAILED);
    CHECK(code(reply) == "auth.bad_document");
    CHECK(store.puts_ == 0);
  }
}

TEST_CASE("auth login without a terminal never asks: each missing value is a usage error") {
  MemoryStore store;
  std::ostringstream prompts;
  const Input in;
  auth::LoginRequest request;
  CHECK(code(auth::login(request, in.fd(), prompts, &store)) == "usage.missing_argument");
  request.broker_ = "kis";
  CHECK(code(auth::login(request, in.fd(), prompts, &store)) == "usage.missing_argument");
  request.env_ = "demo";
  CHECK(code(auth::login(request, in.fd(), prompts, &store)) == "usage.missing_argument");
  request.env_ = "paper";
  CHECK(code(auth::login(request, in.fd(), prompts, &store)) == "usage.bad_value");
  request.broker_ = "toss";
  request.env_ = "demo";
  CHECK(code(auth::login(request, in.fd(), prompts, &store)) == "usage.bad_value");
  request.broker_ = "nobroker";
  CHECK(code(auth::login(request, in.fd(), prompts, &store)) == "usage.bad_value");
  CHECK(prompts.str().empty());
  CHECK(store.puts_ == 0);
}

TEST_CASE("auth login replaces a stored login only with --force") {
  MemoryStore store;
  REQUIRE(login_with(store, "kis", "demo", kis_document()).exit_ == Exit::OK);
  const auto before = store.items_.at("kis/demo");
  const auto other = kis_document(R"(,"account_product_code":"22")");
  auto reply = login_with(store, "kis", "demo", other);
  CHECK(code(reply) == "auth.exists");
  CHECK(store.items_.at("kis/demo") == before);
  reply = login_with(store, "kis", "demo", other, true);
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(reply.body_.at("replaced").as_bool());
  CHECK(store.items_.at("kis/demo") != before);
  // Another environment is another login.
  CHECK(login_with(store, "kis", "real", kis_document()).exit_ == Exit::OK);
  CHECK(store.items_.size() == 2);
}

TEST_CASE("auth login answers a locked or failing store, and no store at all") {
  MemoryStore locked;
  locked.fail_ = auth::StoreStatus::LOCKED;
  CHECK(code(login_with(locked, "kis", "demo", kis_document())) == "auth.store_locked");
  MemoryStore failing;
  failing.fail_ = auth::StoreStatus::UNAVAILABLE;
  CHECK(code(login_with(failing, "kis", "demo", kis_document())) == "auth.no_store");
  MemoryStore put_locked;
  put_locked.fail_put_ = auth::StoreStatus::LOCKED;
  CHECK(code(login_with(put_locked, "kis", "demo", kis_document())) == "auth.store_locked");
  CHECK(put_locked.items_.empty());
  // A store that fails while storing cannot say whether it stored: the reply does not claim nothing changed.
  MemoryStore put_failing;
  put_failing.fail_put_ = auth::StoreStatus::UNAVAILABLE;
  const auto uncertain = login_with(put_failing, "kis", "demo", kis_document());
  CHECK(code(uncertain) == "auth.no_store");
  const auto message = std::string(uncertain.body_.at("error").at("message").as_string());
  CHECK(message.find("did not confirm") != std::string::npos);
  CHECK(message.find("nothing changed") == std::string::npos);

  std::ostringstream prompts;
  const Input in(kis_document());
  auth::LoginRequest request;
  request.broker_ = "kis";
  request.env_ = "demo";
  request.with_secret_ = true;
  CHECK(code(auth::login(request, in.fd(), prompts, nullptr)) == "auth.no_store");
}

TEST_CASE("auth login stops on SIGINT, SIGTERM or SIGHUP before storing") {
  MemoryStore store;
  cmdatl::interruption = 1;
  const auto reply = login_with(store, "kis", "demo", kis_document());
  cmdatl::interruption = 0;
  CHECK(reply.exit_ == Exit::INTERRUPTED);
  CHECK(code(reply) == "interrupted");
  CHECK(store.puts_ == 0);
}

TEST_CASE("auth status shows the stored logins masked, and which variables are set") {
  MemoryStore store;
  REQUIRE(login_with(store, "kis", "demo", kis_document()).exit_ == Exit::OK);
  REQUIRE(
    login_with(store, "toss", "", R"({"client_id":")" + KEY + R"(","client_secret":")" + SECRET + R"("})").exit_ ==
    Exit::OK
  );
  store.items_["junk"] = "x";
  auto reply = auth::status({}, &store);
  seen().push_back(json::serialize(reply.body_));
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(reply.body_.at("store").as_string() == "memory");
  const auto& logins = reply.body_.at("logins").as_array();
  REQUIRE(logins.size() == 2);
  CHECK(logins[0].at("broker").as_string() == "kis");
  CHECK(logins[0].at("key").as_string() == "KE****");
  CHECK(logins[0].at("account").as_string() == "12****");
  CHECK(logins[0].at("checked").as_bool() == false);
  CHECK(logins[1].at("broker").as_string() == "toss");
  CHECK(logins[1].at("environment").as_string() == "real");
  CHECK_FALSE(logins[1].as_object().contains("account"));
  CHECK(reply.body_.at("environment_variables").as_object().contains("kiwoom"));

  const auth::LoginFilter only_toss{std::string("toss"), std::nullopt};
  CHECK(auth::status(only_toss, &store).body_.at("logins").as_array().size() == 1);

  CHECK(code(auth::status({std::string("nobroker"), std::nullopt}, &store)) == "usage.bad_value");
  CHECK(code(auth::status({std::nullopt, std::string("paper")}, &store)) == "usage.bad_value");

  store.fail_get_ = auth::StoreStatus::LOCKED;
  reply = auth::status({}, &store);
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(reply.body_.at("logins").as_array()[0].at("locked").as_bool());
  CHECK_FALSE(reply.body_.at("logins").as_array()[0].as_object().contains("key"));
  store.fail_get_ = auth::StoreStatus::UNAVAILABLE;
  CHECK(code(auth::status({}, &store)) == "auth.no_store");
  store.fail_get_ = auth::StoreStatus::OK;

  store.fail_ = auth::StoreStatus::LOCKED;
  CHECK(code(auth::status({}, &store)) == "auth.store_locked");
  reply = auth::status({}, nullptr);
  CHECK(reply.exit_ == Exit::OK);
  CHECK(reply.body_.at("store").is_null());
}

TEST_CASE("auth logout removes one login, and asks which only on a terminal") {
  MemoryStore store;
  REQUIRE(login_with(store, "kis", "demo", kis_document()).exit_ == Exit::OK);
  REQUIRE(login_with(store, "kis", "real", kis_document()).exit_ == Exit::OK);
  std::ostringstream prompts;
  const Input in;
  CHECK(code(auth::logout({std::string("kis"), std::nullopt}, in.fd(), prompts, &store)) == "usage.missing_argument");
  const auto reply = auth::logout({std::string("kis"), std::string("demo")}, in.fd(), prompts, &store);
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(reply.body_.at("removed").as_string() == "kis/demo");
  CHECK(store.items_.count("kis/demo") == 0);
  CHECK(code(auth::logout({std::string("kiwoom"), std::nullopt}, in.fd(), prompts, &store)) == "auth.not_found");
  CHECK(code(auth::logout({}, in.fd(), prompts, nullptr)) == "auth.no_store");
  CHECK(code(auth::logout({std::string("kis"), std::string("x")}, in.fd(), prompts, &store)) == "usage.bad_value");
  CHECK(prompts.str().empty());
}

TEST_CASE("the auth commands are in the table, and their replies parse") {
  // The system's store: a CI runner's keychain may be locked, and a Linux build may have none.
  const auto status = run({"auth", "status", "--json"});
  if (status.exit_ == 0) {
    CHECK(reply_of(status.out_).contains("logins"));
  } else {
    CHECK((code_of(status.out_) == "auth.store_locked" || code_of(status.out_) == "auth.no_store"));
  }
  const auto login = run({"auth", "login", "--broker", "toss", "--env", "demo", "--json"});
  CHECK(login.exit_ == 2);
  CHECK(code_of(login.out_) == "usage.bad_value");
}

#if !defined(_WIN32)
namespace {

/** @brief A child running `auth login` or `auth logout` on a pseudo-terminal, with a file store. */
class TerminalLogin {
public:
  /**
   * @brief Starts the child on a new pseudo-terminal, as its controlling terminal.
   * @param dir The file store's directory.
   * @param request The login's flags.
   * @param logout Whether to run `auth logout` (with `request`'s broker and environment) instead.
   */
  TerminalLogin(const std::filesystem::path& dir, const auth::LoginRequest& request, bool logout = false) {
    pid_ = ::forkpty(&master_, nullptr, nullptr, nullptr);
    REQUIRE(pid_ >= 0);
    if (pid_ == 0) {
      cmdatl::handle_signals();
      termios before{};
      ::tcgetattr(0, &before);
      FileStore store(dir);
      Reply reply;
      if (logout) {
        reply = auth::logout({request.broker_, request.env_}, 0, std::cerr, &store);
      } else {
        reply = auth::login(request, 0, std::cerr, &store);
      }
      termios now{};
      const bool echo = ::tcgetattr(0, &now) == 0 && (now.c_lflag & ECHO) != 0;
      const bool same = now.c_iflag == before.c_iflag && now.c_oflag == before.c_oflag &&
        now.c_cflag == before.c_cflag && now.c_lflag == before.c_lflag &&
        std::equal(std::begin(now.c_cc), std::end(now.c_cc), std::begin(before.c_cc));
      // What a later reader of the terminal (the shell) would get: typed bytes the login left unread.
      termios raw = now;
      raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      ::tcsetattr(0, TCSANOW, &raw);
      std::size_t left = 0;
      pollfd ready{0, POLLIN, 0};
      while (::poll(&ready, 1, 200) > 0) {
        char byte = 0;
        if (::read(0, &byte, 1) != 1) {
          break;
        }
        ++left;
      }
      std::cerr
        << "\n[echo " << (echo ? "on" : "off") << "] [termios " << (same ? "same" : "changed") << "] [left " << left
        << "] [code " << code(reply) << "]\n"
        << std::flush;
      std::_Exit(static_cast<int>(reply.exit_));
    }
  }

  TerminalLogin(const TerminalLogin&) = delete;
  TerminalLogin& operator=(const TerminalLogin&) = delete;

  /** @brief Kills the child if it is still there, and closes the terminal. */
  ~TerminalLogin() {
    if (pid_ > 0 && !reaped_) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    ::close(master_);
  }

  /**
   * @brief Reads the terminal until a text appears (10 s at most).
   * @param text The text.
   * @return Whether it appeared.
   */
  bool wait_for(const std::string& text) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (transcript_.find(text, cursor_) == std::string::npos) {
      if (std::chrono::steady_clock::now() > until || !read_some(100)) {
        if (std::chrono::steady_clock::now() > until) {
          return false;
        }
      }
    }
    cursor_ = transcript_.find(text, cursor_) + text.size();
    return true;
  }

  /**
   * @brief Types a line.
   * @param line The line, without its line break.
   */
  void type(const std::string& line) {
    const auto text = line + "\n";
    REQUIRE(::write(master_, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
  }

  /**
   * @brief Types raw bytes (a control character).
   * @param bytes The bytes.
   */
  void press(const std::string& bytes) {
    REQUIRE(::write(master_, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  }

  /**
   * @brief Sends the child a signal.
   * @param sig The signal.
   */
  void signal(int sig) {
    ::kill(pid_, sig);
  }

  /**
   * @brief Waits for the child to end, reading the rest of its output.
   * @return Its exit code, or -1 if it did not exit normally.
   */
  int finish() {
    int status = 0;
    for (;;) {
      read_some(50);
      const auto done = ::waitpid(pid_, &status, WNOHANG);
      if (done == pid_) {
        break;
      }
    }
    reaped_ = true;
    while (read_some(50)) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  /**
   * @brief Everything the terminal showed.
   * @return The text.
   */
  const std::string& transcript() const noexcept {
    return transcript_;
  }

private:
  int master_{-1};
  pid_t pid_{-1};
  bool reaped_{false};
  std::string transcript_;
  std::size_t cursor_{0};

  bool read_some(int wait_ms) {
    pollfd ready{master_, POLLIN, 0};
    if (::poll(&ready, 1, wait_ms) <= 0) {
      return false;
    }
    char buffer[512];
    const auto got = ::read(master_, buffer, sizeof buffer);
    if (got <= 0) {
      return false;
    }
    transcript_.append(buffer, static_cast<std::size_t>(got));
    return true;
  }
};

/**
 * @brief The prompts a transcript shows, in order: each line ending in `: ` or `? `, and each numbered choice's
 *   question, as written in the test, not taken from the broker entries.
 * @param transcript The transcript.
 * @param expected The prompts expected, in order.
 * @return Whether they appear in that order and no other prompt appears.
 */
bool asks_exactly(const std::string& transcript, const std::vector<std::string>& expected) {
  const std::vector<std::string> every{
    "Which broker?", "Which environment?", "Type real to go on: ", "Replace it? [y/N] ",
    "App key: ", "App secret: ", "Account number (8 digits): ", "Account product code (2 digits, Enter for 01): ",
    "Secret key: ", "Client id: ", "Client secret: "
  };
  std::vector<std::pair<std::size_t, std::string>> found;
  for (const auto& prompt : every) {
    for (auto at = transcript.find(prompt); at != std::string::npos; at = transcript.find(prompt, at + 1)) {
      found.emplace_back(at, prompt);
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> asked;
  for (const auto& entry : found) {
    asked.push_back(entry.second);
  }
  INFO(transcript);
  return asked == expected;
}

} // namespace

TEST_CASE("auth login on a terminal asks only what the chosen broker needs") {
  const Watchdog watchdog("terminal login", 60);
  SECTION("KIS: the environment, the keys and the account, the product code defaulting on Enter") {
    const ScratchDir dir;
    TerminalLogin child(dir.path(), {});
    REQUIRE(child.wait_for("Choose 1-3: "));
    child.type("1");
    REQUIRE(child.wait_for("Choose 1-2: "));
    child.type("");
    REQUIRE(child.wait_for("App key: "));
    child.type(KEY);
    REQUIRE(child.wait_for("App secret: "));
    child.type(SECRET);
    REQUIRE(child.wait_for("Account number (8 digits): "));
    child.type("12345678");
    REQUIRE(child.wait_for("Enter for 01): "));
    child.type("");
    CHECK(child.finish() == 0);
    CHECK(asks_exactly(
      child.transcript(),
      {"Which broker?", "Which environment?",
        "App key: ", "App secret: ", "Account number (8 digits): ", "Account product code (2 digits, Enter for 01): "}
    ));
    // The secret was typed with echo off: the terminal never showed it.
    CHECK(child.transcript().find(SECRET) == std::string::npos);
    CHECK(child.transcript().find(KEY) != std::string::npos);
    CHECK(child.transcript().find("[echo on]") != std::string::npos);
    CHECK(
      slurp(dir.path() / "kis_demo") ==
      R"({"app_key":")" + KEY + R"(","app_secret":")" + SECRET + R"(","account_number":"12345678","demo":true})"
    );
  }
  SECTION("Kiwoom: the environment and the keys, no account") {
    const ScratchDir dir;
    TerminalLogin child(dir.path(), {});
    REQUIRE(child.wait_for("Choose 1-3: "));
    child.type("2");
    REQUIRE(child.wait_for("Choose 1-2: "));
    child.type("2");
    REQUIRE(child.wait_for("Type real to go on: "));
    child.type("real");
    REQUIRE(child.wait_for("App key: "));
    child.type(KEY);
    REQUIRE(child.wait_for("Secret key: "));
    child.type(SECRET);
    CHECK(child.finish() == 0);
    CHECK(asks_exactly(
      child.transcript(), {"Which broker?", "Which environment?", "Type real to go on: ", "App key: ", "Secret key: "}
    ));
    CHECK(
      slurp(dir.path() / "kiwoom_real") == R"({"appkey":")" + KEY + R"(","secretkey":")" + SECRET + R"(","demo":false})"
    );
  }
  SECTION("Toss: no environment and no account") {
    const ScratchDir dir;
    TerminalLogin child(dir.path(), {});
    REQUIRE(child.wait_for("Choose 1-3: "));
    child.type("3");
    REQUIRE(child.wait_for("Client id: "));
    child.type(KEY);
    REQUIRE(child.wait_for("Client secret: "));
    child.type(SECRET);
    CHECK(child.finish() == 0);
    CHECK(asks_exactly(child.transcript(), {"Which broker?", "Client id: ", "Client secret: "}));
    CHECK(slurp(dir.path() / "toss_real") == R"({"client_id":")" + KEY + R"(","client_secret":")" + SECRET + R"("})");
  }
  SECTION("flags skip their questions") {
    const ScratchDir dir;
    auth::LoginRequest request;
    request.broker_ = "kiwoom";
    request.env_ = "demo";
    TerminalLogin child(dir.path(), request);
    REQUIRE(child.wait_for("App key: "));
    child.type(KEY);
    REQUIRE(child.wait_for("Secret key: "));
    child.type(SECRET);
    CHECK(child.finish() == 0);
    CHECK(asks_exactly(child.transcript(), {"App key: ", "Secret key: "}));
  }
  SECTION("a stored login is replaced only when the answer is yes") {
    const ScratchDir dir;
    spit(dir.path() / "toss_real", "old");
    auth::LoginRequest request;
    request.broker_ = "toss";
    TerminalLogin child(dir.path(), request);
    REQUIRE(child.wait_for("Replace it? [y/N] "));
    child.type("");
    CHECK(child.finish() == 1);
    CHECK(child.transcript().find("[code auth.exists]") != std::string::npos);
    CHECK(slurp(dir.path() / "toss_real") == "old");
  }
  SECTION("real not typed stores nothing") {
    const ScratchDir dir;
    auth::LoginRequest request;
    request.broker_ = "kis";
    TerminalLogin child(dir.path(), request);
    REQUIRE(child.wait_for("Choose 1-2: "));
    child.type("2");
    REQUIRE(child.wait_for("Type real to go on: "));
    child.type("yes");
    CHECK(child.finish() == 1);
    CHECK(child.transcript().find("[code auth.not_confirmed]") != std::string::npos);
    CHECK(std::filesystem::is_empty(dir.path()));
  }
}

TEST_CASE("auth login on a terminal stops on a signal or the end of input, echo restored and nothing stored") {
  const Watchdog watchdog("terminal signals", 60);
  auth::LoginRequest request;
  request.broker_ = "kiwoom";
  request.env_ = "demo";
  for (const std::string how : {"ctrl-c", "sigint", "sigterm", "sighup"}) {
    INFO(how);
    const ScratchDir dir;
    TerminalLogin child(dir.path(), request);
    REQUIRE(child.wait_for("App key: "));
    child.type(KEY);
    REQUIRE(child.wait_for("Secret key: "));
    if (how == "ctrl-c") {
      child.press("\x03");
    } else {
      child.signal(how == "sigint" ? SIGINT : (how == "sigterm" ? SIGTERM : SIGHUP));
    }
    CHECK(child.finish() == 130);
    CHECK(child.transcript().find("[echo on] [termios same] [left 0]") != std::string::npos);
    CHECK(child.transcript().find("[code interrupted]") != std::string::npos);
    CHECK(std::filesystem::is_empty(dir.path()));
  }
  SECTION("a secret half typed when a signal arrives is discarded, not left for the shell") {
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) {
      INFO(sig);
      const ScratchDir dir;
      TerminalLogin child(dir.path(), request);
      REQUIRE(child.wait_for("App key: "));
      child.type(KEY);
      REQUIRE(child.wait_for("Secret key: "));
      child.press(SECRET);
      // The bytes reach the terminal's input queue before the signal; the login has not read them (no line break).
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      child.signal(sig);
      CHECK(child.finish() == 130);
      CHECK(child.transcript().find("[echo on] [termios same] [left 0]") != std::string::npos);
      CHECK(child.transcript().find(SECRET) == std::string::npos);
      CHECK(std::filesystem::is_empty(dir.path()));
    }
  }
  SECTION("the end of input at a hidden prompt") {
    const ScratchDir dir;
    TerminalLogin child(dir.path(), request);
    REQUIRE(child.wait_for("App key: "));
    child.type(KEY);
    REQUIRE(child.wait_for("Secret key: "));
    child.press("\x04");
    CHECK(child.finish() == 1);
    CHECK(child.transcript().find("[echo on] [termios same]") != std::string::npos);
    CHECK(child.transcript().find("[code auth.no_answer]") != std::string::npos);
    CHECK(std::filesystem::is_empty(dir.path()));
  }
  SECTION("a signal at a visible prompt and at a choice") {
    for (const bool choice : {false, true}) {
      INFO(choice);
      const ScratchDir dir;
      TerminalLogin child(dir.path(), choice ? auth::LoginRequest{} : request);
      REQUIRE(child.wait_for(choice ? "Choose 1-3: " : "App key: "));
      child.press("half");
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      child.signal(SIGTERM);
      CHECK(child.finish() == 130);
      CHECK(child.transcript().find("[echo on] [termios same] [left 0]") != std::string::npos);
      CHECK(std::filesystem::is_empty(dir.path()));
    }
  }
  SECTION("the end of input at a prompt") {
    const ScratchDir dir;
    TerminalLogin child(dir.path(), request);
    REQUIRE(child.wait_for("App key: "));
    child.press("\x04");
    CHECK(child.finish() == 1);
    CHECK(child.transcript().find("[code auth.no_answer]") != std::string::npos);
    CHECK(std::filesystem::is_empty(dir.path()));
  }
  SECTION("a choice that is not one") {
    const ScratchDir dir;
    TerminalLogin child(dir.path(), {});
    REQUIRE(child.wait_for("Choose 1-3: "));
    child.type("9");
    CHECK(child.finish() == 2);
    CHECK(child.transcript().find("[code usage.bad_value]") != std::string::npos);
  }
  SECTION("logout asks which login when several match") {
    const ScratchDir dir;
    spit(dir.path() / "kis_demo", "{}");
    spit(dir.path() / "kis_real", "{}");
    auth::LoginRequest which;
    which.broker_ = "kis";
    TerminalLogin child(dir.path(), which, true);
    REQUIRE(child.wait_for("Choose 1-2: "));
    child.type("2");
    CHECK(child.finish() == 0);
    CHECK(std::filesystem::exists(dir.path() / "kis_demo"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "kis_real"));
  }
}
#endif

TEST_CASE("no auth reply or prompt repeats a key or a secret") {
  // Runs after the cases above in declaration order; it also produces its own outcomes, so it holds alone.
  MemoryStore store;
  login_with(store, "kis", "demo", kis_document());
  login_with(store, "kis", "demo", kis_document());
  login_with(store, "kis", "demo", "garbage " + SECRET);
  login_with(store, "toss", "", R"({")" + SECRET + R"(":1})");
  seen().push_back(json::serialize(auth::status({}, &store).body_));
  REQUIRE_FALSE(seen().empty());
  for (const auto& text : seen()) {
    INFO(text);
    CHECK(text.find(SECRET) == std::string::npos);
    CHECK(text.find(KEY) == std::string::npos);
  }
}
