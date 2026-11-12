// Broker folders: finding one by name, the trust checks, `broker.toml`, and the auth commands on a folder's login.
#include <cmdatl/auth/login.hpp>

#include <catch2/catch_test_macros.hpp>

#include "broker-folders.hpp"
#include "support.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>

using namespace support;

namespace {

namespace auth = cmdatl::auth;
namespace json = boost::json;
using cmdatl::Exit;
using cmdatl::Reply;

/** @brief A store in memory. */
class Store : public auth::CredentialStore {
public:
  /** @brief The logins. */
  std::map<std::string, std::string> items_;
  /** @brief How many times `get` was called. */
  int gets_{0};

  std::string kind() const override {
    return "memory";
  }

  auth::StoreList list() override {
    auth::StoreList result;
    result.status_ = auth::StoreStatus::OK;
    for (const auto& item : items_) {
      result.names_.push_back(item.first);
    }
    return result;
  }

  auth::StoreAnswer get(const std::string& name) override {
    ++gets_;
    const auto found = items_.find(name);
    if (found == items_.end()) {
      return {auth::StoreStatus::NOT_FOUND, {}, {}};
    }
    return {auth::StoreStatus::OK, found->second, {}};
  }

  auth::StoreAnswer put(const std::string& name, const std::string& value) override {
    items_[name] = value;
    return {auth::StoreStatus::OK, {}, {}};
  }

  auth::StoreAnswer remove(const std::string& name) override {
    return {items_.erase(name) > 0 ? auth::StoreStatus::OK : auth::StoreStatus::NOT_FOUND, {}, {}};
  }
};

/** @brief A pipe holding a text, then its end: standard input that is not a terminal. */
class Input {
public:
  /**
   * @brief Writes the text and closes the write end.
   * @param text The text.
   */
  explicit Input(const std::string& text) {
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

std::string code(const Reply& reply) {
  const auto* error = reply.body_.if_contains("error");
  return error == nullptr ? "" : std::string(error->as_object().at("code").as_string());
}

/**
 * @brief `auth login --broker NAME [--env ENV] --with-secret` with a document on standard input.
 * @param store The store.
 * @param broker The broker.
 * @param env The environment, or empty.
 * @param document The document.
 * @return The reply.
 */
Reply login_with(Store& store, const std::string& broker, const std::string& env, const std::string& document) {
  auth::LoginRequest request;
  request.broker_ = broker;
  if (!env.empty()) {
    request.env_ = env;
  }
  request.with_secret_ = true;
  const Input in(document);
  std::ostringstream prompts;
  return auth::login(request, in.fd(), prompts, &store);
}

} // namespace

TEST_CASE("a broker folder is found by name, after the built-in brokers and never as one of cmdatl's own names") {
  const BrokerHome home;
  const auto folder = home.folder("vendor", VENDOR_TOML);
  home.folder("kis", VENDOR_TOML);

  const auto found = auth::lookup_broker("vendor");
  REQUIRE(found.found_ == auth::BrokerFound::FOLDER);
  CHECK(found.folder_->folder_ == folder);
  CHECK(found.folder_->data_only_);
  CHECK(found.broker()->read_ == nullptr);
  CHECK(found.broker()->fields_.size() == 1);
  CHECK(found.broker()->fields_[0].secret_);
  CHECK_FALSE(found.broker()->has_demo_);

  // A folder named as a built-in broker is never read: the built-in one answers.
  const auto kis = auth::lookup_broker("kis");
  CHECK(kis.found_ == auth::BrokerFound::BUILT_IN);
  CHECK(kis.broker()->read_ != nullptr);
  for (const auto* name : {"mock", "paper", "csv", "wide", "store", "Vendor", "a_b", "-x", "1x", ""}) {
    INFO(name);
    CHECK_FALSE(cmdatl::user_broker_name(name));
  }
  CHECK(cmdatl::user_broker_name("eodhd"));
  CHECK(cmdatl::user_broker_name("twelve-data"));
  CHECK(auth::lookup_broker("missing").found_ == auth::BrokerFound::NOT_FOUND);
  CHECK(code(auth::broker_refusal(auth::lookup_broker("missing"))) == "adapter.not_found");
  CHECK(auth::user_broker_names() == std::vector<std::string>{"vendor"});
}

TEST_CASE("a broker folder another user could change is refused, at any depth and in its parents") {
  const BrokerHome home;
  const auto folder = home.folder("vendor", VENDOR_TOML);
  const auto unsafe = [] {
    return auth::lookup_broker("vendor").found_ == auth::BrokerFound::UNSAFE;
  };
  REQUIRE_FALSE(unsafe());

  SECTION("the folder writable by its group") {
    ::chmod(folder.c_str(), 0770);
    CHECK(unsafe());
  }
  SECTION("a file writable by others") {
    ::chmod((folder / "adapter.hpp").c_str(), 0602);
    CHECK(unsafe());
  }
  SECTION("a nested directory writable by its group") {
    std::filesystem::create_directories(folder / "include" / "x");
    ::chmod((folder / "include").c_str(), 0700);
    ::chmod((folder / "include" / "x").c_str(), 0770);
    CHECK(unsafe());
  }
  SECTION("brokers/ writable by others") {
    ::chmod(home.brokers().c_str(), 0702);
    CHECK(unsafe());
  }
  SECTION("cmdatl/ writable by its group") {
    ::chmod(home.brokers().parent_path().c_str(), 0770);
    CHECK(unsafe());
  }
  SECTION("a link to a file") {
    BrokerHome::file(folder / "real.hpp", "#pragma once\n");
    std::filesystem::create_symlink(folder / "real.hpp", folder / "link.hpp");
    CHECK(unsafe());
  }
  SECTION("the folder a link") {
    std::filesystem::rename(folder, home.brokers() / "elsewhere");
    std::filesystem::create_directory_symlink(home.brokers() / "elsewhere", folder);
    CHECK(unsafe());
  }
  SECTION("a FIFO") {
    REQUIRE(::mkfifo((folder / "pipe").c_str(), 0600) == 0);
    CHECK(unsafe());
  }
#if defined(__APPLE__)
  SECTION("an access control list granting write, the mode bits unchanged") {
    const auto file = folder / "adapter.hpp";
    REQUIRE(std::system(("/bin/chmod +a 'everyone allow write' '" + file.string() + "'").c_str()) == 0);
    struct stat info{};
    REQUIRE(::lstat(file.c_str(), &info) == 0);
    CHECK((info.st_mode & 0022) == 0);
    CHECK(unsafe());
  }
  SECTION("an access control list on brokers/ granting a file's addition") {
    REQUIRE(std::system(("/bin/chmod +a 'everyone allow add_file' '" + home.brokers().string() + "'").c_str()) == 0);
    CHECK(unsafe());
  }
#endif
  const auto reply = auth::broker_refusal(auth::lookup_broker("vendor"));
  CHECK(code(reply) == "adapter.unsafe");
  CHECK(reply.exit_ == Exit::FAILED);
}

TEST_CASE("a broker folder is refused beyond its bounds, or without its two files") {
  const BrokerHome home;
  const auto folder = home.folder("vendor", VENDOR_TOML);
  SECTION("too many entries") {
    for (std::size_t k = 0; k < auth::MAX_ADAPTER_ENTRIES; ++k) {
      BrokerHome::file(folder / ("f" + std::to_string(k)), "");
    }
    CHECK(auth::lookup_broker("vendor").found_ == auth::BrokerFound::INVALID);
  }
  SECTION("too deep") {
    auto path = folder;
    for (int k = 0; k <= auth::MAX_ADAPTER_DEPTH; ++k) {
      path /= "d";
      std::filesystem::create_directory(path);
      ::chmod(path.c_str(), 0700);
    }
    CHECK(auth::lookup_broker("vendor").found_ == auth::BrokerFound::INVALID);
  }
  SECTION("no adapter.hpp") {
    std::filesystem::remove(folder / "adapter.hpp");
    CHECK(auth::lookup_broker("vendor").found_ == auth::BrokerFound::INVALID);
  }
  SECTION("broker.toml too large") {
    BrokerHome::file(folder / "broker.toml", VENDOR_TOML + std::string(auth::MAX_BROKER_TOML_BYTES, '#'));
    CHECK(auth::lookup_broker("vendor").found_ == auth::BrokerFound::INVALID);
  }
}

TEST_CASE("broker.toml is read strictly, each refusal at its line") {
  struct Case {
    const char* what_;
    std::string toml_;
    std::string line_;
  };
  const std::string field = "[[fields]]\nname = \"k\"\nprompt = \"K: \"\n";
  const std::vector<Case> cases{
    {"no display", "demo = false\n" + field, "1:"},
    {"an unknown key", "display = \"x\"\nicon = \"y\"\n" + field, "2:"},
    {"demo not a boolean", "display = \"x\"\ndemo = \"no\"\n" + field, "2:"},
    {"no fields", "display = \"x\"\n", "1:"},
    {"fields not tables", "display = \"x\"\nfields = [1]\n", "2:"},
    {"a field's unknown key", "display = \"x\"\n" + field + "hint = \"h\"\n", "5:"},
    {"a field name with a dot", "display = \"x\"\n[[fields]]\nname = \"a.b\"\nprompt = \"P\"\n", "3:"},
    {"a field named demo", "display = \"x\"\n[[fields]]\nname = \"demo\"\nprompt = \"P\"\n", "3:"},
    {"a field twice", "display = \"x\"\n" + field + field, "6:"},
    {"no prompt", "display = \"x\"\n[[fields]]\nname = \"k\"\n", "2:"},
    {"a control character", "display = \"x\\u001b[2J\"\n" + field, "1:"},
    {"a secret default", "display = \"x\"\n" + field + "secret = true\ndefault = \"d\"\n", "2:"},
    {"a masked key (a folder's status shows no field)", "display = \"x\"\n" + field + "masked = true\n", "5:"},
    {"an empty default", "display = \"x\"\n" + field + "default = \"\"\n", "5:"},
    {"not TOML", "display = \n", "1:"},
  };
  for (const auto& c : cases) {
    INFO(c.what_);
    auth::UserBroker broker;
    const auto problem = auth::read_broker_toml("x", c.toml_, broker);
    REQUIRE(problem);
    INFO(*problem);
    CHECK(problem->rfind(c.line_, 0) == 0);
  }
  std::string many = "display = \"x\"\n";
  for (std::size_t k = 0; k <= auth::MAX_BROKER_FIELDS; ++k) {
    many += "[[fields]]\nname = \"k" + std::to_string(k) + "\"\nprompt = \"P\"\n";
  }
  auth::UserBroker broker;
  CHECK(auth::read_broker_toml("x", many, broker));
  CHECK_FALSE(auth::read_broker_toml("x", BROKER_TOML, broker));
  CHECK(broker.entry_.fields_.size() == 3);
  CHECK(broker.entry_.fields_[2].default_ == std::optional<std::string>("kr"));
}

TEST_CASE("auth login stores a broker folder's login unchecked, its defaults written, and shows none of its fields") {
  const BrokerHome home;
  const auto folder = home.folder("acme", BROKER_TOML);
  Store store;

  auto reply = login_with(store, "acme", "demo", R"({"appKey":")" + KEY + R"(","appSecret":")" + SECRET + R"("})");
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(
    store.items_.at("acme/demo") ==
    R"({"appKey":")" + KEY + R"(","appSecret":")" + SECRET + R"(","region":"kr","demo":true})"
  );
  CHECK(reply.body_.at("adapter").as_string() == folder.string());
  CHECK_FALSE(reply.body_.contains("key"));
  CHECK(reply.body_.at("checked").as_bool() == false);

  // A field the folder does not declare, a missing one, and demo for a vendor without one are refused.
  CHECK(code(login_with(store, "acme", "real", R"({"appKey":"a","appSecret":"b","x":"c"})")) == "auth.bad_document");
  CHECK(code(login_with(store, "acme", "real", R"({"appKey":"a"})")) == "auth.bad_document");
  home.folder("vendor", VENDOR_TOML);
  CHECK(code(login_with(store, "vendor", "demo", R"({"api_token":"t"})")) == "usage.bad_value");
  REQUIRE(login_with(store, "vendor", "", R"({"api_token":")" + SECRET + R"("})").exit_ == Exit::OK);
  CHECK(store.items_.count("vendor/real") == 1);

  // An unknown name, and a folder another user could change.
  CHECK(code(login_with(store, "nobroker", "", "{}")) == "usage.bad_value");
  ::chmod(folder.c_str(), 0777);
  CHECK(code(login_with(store, "acme", "real", R"({"appKey":"a","appSecret":"b"})")) == "adapter.unsafe");
  ::chmod(folder.c_str(), 0700);

  // Status reads no folder login's document: only whether its folder still reads.
  store.gets_ = 0;
  const auto status = auth::status({}, &store);
  REQUIRE(status.exit_ == Exit::OK);
  CHECK(store.gets_ == 0);
  const auto text = json::serialize(status.body_);
  CHECK(text.find(KEY) == std::string::npos);
  CHECK(text.find(SECRET) == std::string::npos);
  CHECK(text.find("SE****") == std::string::npos);
  const auto& rows = status.body_.at("logins").as_array();
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].at("broker").as_string() == "acme");
  CHECK(rows[0].at("adapter").as_string() == folder.string());
  CHECK_FALSE(rows[0].as_object().contains("key"));
}

TEST_CASE("a broker folder's login outlives its folder: status shows it, logout removes it") {
  Store store;
  {
    const BrokerHome home;
    home.folder("vendor", VENDOR_TOML);
    REQUIRE(login_with(store, "vendor", "", R"({"api_token":"t"})").exit_ == Exit::OK);
  }
  const BrokerHome empty;
  const auto status = auth::status({}, &store);
  REQUIRE(status.body_.at("logins").as_array().size() == 1);
  CHECK(status.body_.at("logins").as_array()[0].at("adapter").is_null());
  const Input none("");
  std::ostringstream prompts;
  const auto removed = auth::logout({std::string("vendor"), std::nullopt}, none.fd(), prompts, &store);
  REQUIRE(removed.exit_ == Exit::OK);
  CHECK(removed.body_.at("removed").as_string() == "vendor/real");
  CHECK(store.items_.empty());
}
#endif
