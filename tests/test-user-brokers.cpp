// Broker folders: finding one by its path, the trust checks, `broker.toml`, and the auth commands on a folder's login,
// bound to the folder.
#include <cmdatl/auth/login.hpp>

#include <catch2/catch_test_macros.hpp>

#include "broker-folders.hpp"
#include "support.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
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
 * @brief `auth login --broker NAME [--env ENV] [--adapter PATH] --with-secret` with a document on standard input.
 * @param store The store.
 * @param broker The broker.
 * @param env The environment, or empty.
 * @param document The document.
 * @param adapter The folder, or empty.
 * @return The reply.
 */
Reply login_with(
  Store& store, const std::string& broker, const std::string& env, const std::string& document,
  const std::string& adapter = ""
) {
  auth::LoginRequest request;
  request.broker_ = broker;
  if (!env.empty()) {
    request.env_ = env;
  }
  if (!adapter.empty()) {
    request.adapter_ = adapter;
  }
  request.with_secret_ = true;
  const Input in(document);
  std::ostringstream prompts;
  return auth::login(request, in.fd(), prompts, &store);
}

/**
 * @brief Finds a broker folder at a path.
 * @param name The broker's name.
 * @param path The folder.
 * @return What was found.
 */
auth::BrokerFound found_at(const std::string& name, const std::filesystem::path& path) {
  return auth::lookup_broker(name, path.string()).found_;
}

} // namespace

TEST_CASE("a broker folder is named by its path, and a built-in name is never one") {
  const BrokerFolders project;
  const auto folder = project.folder("vendor", VENDOR_TOML, "#pragma once\n#include \"src/client.hpp\"\n");
  std::filesystem::create_directory(folder / "src");
  ::chmod((folder / "src").c_str(), 0700);
  BrokerFolders::file(folder / "src" / "client.hpp", "#pragma once\n");

  const auto found = auth::lookup_broker("eodhd", folder.string());
  REQUIRE(found.found_ == auth::BrokerFound::FOLDER);
  CHECK(found.folder_->folder_ == folder);
  CHECK(found.folder_->data_only_);
  CHECK(found.broker()->name_ == "eodhd");
  CHECK(found.broker()->read_ == nullptr);
  CHECK(found.broker()->fields_.size() == 1);
  CHECK(found.broker()->fields_[0].secret_);
  CHECK_FALSE(found.broker()->has_demo_);
  // One read: the files compiled are the files whose broker.toml was read.
  const auto& snapshot = found.folder_->snapshot_;
  CHECK(snapshot.name_ == "eodhd");
  CHECK(snapshot.folder_ == folder.string());
  REQUIRE(snapshot.files_.size() == 3);
  CHECK(snapshot.files_[0].first == "adapter.hpp");
  CHECK(snapshot.files_[1] == std::make_pair(std::string("broker.toml"), VENDOR_TOML));
  CHECK(snapshot.files_[2].first == "src/client.hpp");

  // A trailing separator is the same folder; a path through `..` is resolved by the system, never simplified first.
  CHECK(auth::lookup_broker("eodhd", folder.string() + "/").folder_->folder_ == folder);
  CHECK(found_at("eodhd", folder / ".." / "vendor") == auth::BrokerFound::FOLDER);

  // A built-in name is never a folder, and a folder is named only for a name that is not built in.
  const auto kis = auth::lookup_broker("kis", "");
  CHECK(kis.found_ == auth::BrokerFound::BUILT_IN);
  CHECK(kis.broker()->read_ != nullptr);
  CHECK(found_at("kis", folder) == auth::BrokerFound::INVALID);
  for (const auto* name : {"mock", "paper", "csv", "wide", "store", "Vendor", "a_b", "-x", "1x", ""}) {
    INFO(name);
    CHECK_FALSE(cmdatl::user_broker_name(name));
    CHECK(found_at(name, folder) != auth::BrokerFound::FOLDER);
  }
  CHECK(cmdatl::user_broker_name("eodhd"));
  CHECK(cmdatl::user_broker_name("twelve-data"));

  // No path, or nothing at it: never a folder found elsewhere.
  CHECK(auth::lookup_broker("eodhd", "").found_ == auth::BrokerFound::NOT_FOUND);
  CHECK(found_at("eodhd", project.root() / "adapters" / "missing") == auth::BrokerFound::NOT_FOUND);
  CHECK(code(auth::broker_refusal(auth::lookup_broker("eodhd", ""))) == "adapter.not_found");
}

TEST_CASE("a broker folder another user could change is refused, in it and on the way to it") {
  const BrokerFolders project;
  const auto folder = project.folder("vendor", VENDOR_TOML);
  const auto adapters = folder.parent_path();
  auto path = folder;
  const auto unsafe = [&path] {
    return found_at("vendor", path) == auth::BrokerFound::UNSAFE;
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
  SECTION("a directory on the way writable by its group") {
    ::chmod(adapters.c_str(), 0770);
    CHECK(unsafe());
  }
  SECTION("the project directory writable by others") {
    ::chmod(project.root().c_str(), 0702);
    CHECK(unsafe());
  }
  SECTION("a link on the way into a directory others can write") {
    const auto shared = project.root() / "shared";
    std::filesystem::create_directory(shared);
    std::filesystem::rename(folder, shared / "vendor");
    ::chmod(shared.c_str(), 0707);
    std::filesystem::create_directory_symlink(shared, project.root() / "link");
    path = project.root() / "link" / "vendor";
    CHECK(unsafe());
  }
  SECTION("a link before `..`, into a directory others can write") {
    // Written link/../vendor: simplified it would be the safe adapters/vendor; the system reads open/vendor.
    const auto open = project.root() / "open";
    std::filesystem::create_directories(open / "deep");
    ::chmod((open / "deep").c_str(), 0700);
    std::filesystem::copy(folder, open / "vendor");
    ::chmod((open / "vendor").c_str(), 0700);
    ::chmod(open.c_str(), 0770);
    std::filesystem::create_directory_symlink(open / "deep", adapters / "link");
    path = adapters / "link" / ".." / "vendor";
    CHECK(unsafe());
  }
  SECTION("a link to a file") {
    BrokerFolders::file(folder / "real.hpp", "#pragma once\n");
    std::filesystem::create_symlink(folder / "real.hpp", folder / "link.hpp");
    CHECK(unsafe());
  }
  SECTION("the folder a link") {
    std::filesystem::rename(folder, adapters / "elsewhere");
    std::filesystem::create_directory_symlink(adapters / "elsewhere", folder);
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
  SECTION("an access control list on a directory on the way granting a file's addition") {
    REQUIRE(std::system(("/bin/chmod +a 'everyone allow add_file' '" + adapters.string() + "'").c_str()) == 0);
    CHECK(unsafe());
  }
#endif
  const auto reply = auth::broker_refusal(auth::lookup_broker("vendor", path.string()));
  CHECK(code(reply) == "adapter.unsafe");
  CHECK(reply.exit_ == Exit::FAILED);
}

TEST_CASE("a sticky directory on the way, and a link the user owns, lead to a broker folder") {
  const BrokerFolders project;
  const auto folder = project.folder("vendor", VENDOR_TOML);
  // Others may add to a sticky directory, never replace what is the user's in it.
  ::chmod(folder.parent_path().c_str(), 01777);
  CHECK(found_at("vendor", folder) == auth::BrokerFound::FOLDER);
  ::chmod(folder.parent_path().c_str(), 0700);
  // A link on the way, followed: the folder is where it leads, canonical, and that is what a login is bound to.
  std::filesystem::create_directory_symlink(project.root(), project.root() / "here");
  const auto found = auth::lookup_broker("vendor", (project.root() / "here" / "adapters" / "vendor").string());
  REQUIRE(found.found_ == auth::BrokerFound::FOLDER);
  CHECK(found.folder_->folder_ == folder);
}

TEST_CASE("a broker folder is refused beyond its bounds, or without its two files") {
  const BrokerFolders project;
  const auto folder = project.folder("vendor", VENDOR_TOML);
  SECTION("too many entries") {
    for (std::size_t k = 0; k < auth::MAX_ADAPTER_ENTRIES; ++k) {
      BrokerFolders::file(folder / ("f" + std::to_string(k)), "");
    }
    CHECK(found_at("vendor", folder) == auth::BrokerFound::INVALID);
  }
  SECTION("too deep") {
    auto path = folder;
    for (int k = 0; k <= auth::MAX_ADAPTER_DEPTH; ++k) {
      path /= "d";
      std::filesystem::create_directory(path);
      ::chmod(path.c_str(), 0700);
    }
    CHECK(found_at("vendor", folder) == auth::BrokerFound::INVALID);
  }
  SECTION("no adapter.hpp") {
    std::filesystem::remove(folder / "adapter.hpp");
    CHECK(found_at("vendor", folder) == auth::BrokerFound::INVALID);
  }
  SECTION("broker.toml too large") {
    BrokerFolders::file(folder / "broker.toml", VENDOR_TOML + std::string(auth::MAX_BROKER_TOML_BYTES, '#'));
    CHECK(found_at("vendor", folder) == auth::BrokerFound::INVALID);
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

TEST_CASE("broker.toml declares its adapter's options strictly, each refusal at its line, never a login's field") {
  // The display on line 1, the field on 2-4, [[options]] on 5 and its keys from 6.
  const std::string head = "display = \"x\"\n[[fields]]\nname = \"api_token\"\nprompt = \"T: \"\n[[options]]\n";
  const std::string good = "name = \"exchange\"\ntype = \"string\"\nrequired = true\n";
  const std::vector<std::tuple<const char*, std::string, std::string, std::string>> cases{
    {"options not tables", "display = \"x\"\noptions = [1]\n[[fields]]\nname = \"k\"\nprompt = \"K\"\n",
      "2:", "[[options]] tables"},
    {"an unknown key", head + good + "default = \"US\"\n", "9:", "has no key default"},
    {"no name", head + "type = \"string\"\nrequired = true\n", "5:", "options.name"},
    {"a name with a dot", head + "name = \"a.b\"\ntype = \"string\"\nrequired = true\n", "6:", "options.name"},
    {"a login's field", head + "name = \"api_token\"\ntype = \"string\"\nrequired = true\n",
      "6:", "a credential is never an option"},
    {"a name twice", head + good + "[[options]]\n" + good, "10:", "given twice"},
    {"an unknown type", head + "name = \"e\"\ntype = \"date\"\nrequired = true\n", "7:", "options.type"},
    {"no required", head + "name = \"e\"\ntype = \"string\"\n", "5:", "options.required must be written"},
    {"required not a boolean", head + "name = \"e\"\ntype = \"string\"\nrequired = \"yes\"\n",
      "8:", "options.required"},
    {"choices of a number", head + "name = \"e\"\ntype = \"number\"\nrequired = true\nchoices = [1]\n",
      "9:", "for a string or an integer"},
    {"choices of a boolean", head + "name = \"e\"\ntype = \"boolean\"\nrequired = true\nchoices = [true]\n",
      "9:", "for a string or an integer"},
    {"no choice", head + good + "choices = []\n", "9:", "options.choices"},
    {"a choice of another type", head + good + "choices = [\"US\", 1]\n", "9:", "are a string"},
  };
  for (const auto& [what, toml, line, says] : cases) {
    INFO(what);
    auth::UserBroker broker;
    const auto problem = auth::read_broker_toml("x", toml, broker);
    REQUIRE(problem);
    INFO(*problem);
    CHECK(problem->rfind(line, 0) == 0);
    CHECK(problem->find(says) != std::string::npos);
  }
  std::string many = "display = \"x\"\n[[fields]]\nname = \"k\"\nprompt = \"P\"\n";
  for (std::size_t k = 0; k <= cmdatl::options::MAX_OPTIONS; ++k) {
    many += "[[options]]\nname = \"o" + std::to_string(k) + "\"\ntype = \"string\"\nrequired = false\n";
  }
  auth::UserBroker broker;
  CHECK(auth::read_broker_toml("x", many, broker));
  // Each type, optional or required, choices for a string and an integer.
  REQUIRE_FALSE(
    auth::read_broker_toml(
      "x",
      head + good + "choices = [\"US\", \"KO\"]\n[[options]]\nname = \"lot\"\ntype = \"integer\"\nrequired = false\n" +
        "choices = [1, 10]\n[[options]]\nname = \"rate\"\ntype = \"number\"\nrequired = false\n" +
        "[[options]]\nname = \"live\"\ntype = \"boolean\"\nrequired = false\n",
      broker
    )
  );
  REQUIRE(broker.options_.size() == 4);
  CHECK(broker.options_[0].required_);
  CHECK(broker.options_[0].choices_ == std::vector<json::value>{"US", "KO"});
  CHECK(broker.options_[1].type_ == cmdatl::options::Type::INTEGER);
  CHECK(broker.options_[1].choices_ == std::vector<json::value>{1, 10});
  CHECK(broker.options_[2].type_ == cmdatl::options::Type::NUMBER);
  CHECK_FALSE(broker.options_[3].required_);
  // A folder without [[options]] declares none.
  REQUIRE_FALSE(auth::read_broker_toml("x", BROKER_TOML, broker));
  CHECK(broker.options_.empty());
}

TEST_CASE("auth login binds a broker folder's login to the folder, stores it unchecked, and shows none of its fields") {
  const BrokerFolders project;
  const auto folder = project.folder("acme", BROKER_TOML);
  Store store;

  auto reply = login_with(
    store, "acme", "demo", R"({"appKey":")" + KEY + R"(","appSecret":")" + SECRET + R"("})", folder.string()
  );
  REQUIRE(reply.exit_ == Exit::OK);
  CHECK(
    store.items_.at("acme/demo") ==
    R"({"version":1,"adapter":")" + folder.string() + R"(","login":{"appKey":")" + KEY + R"(","appSecret":")" + SECRET +
      R"(","region":"kr","demo":true}})"
  );
  CHECK(reply.body_.at("adapter").as_string() == folder.string());
  CHECK_FALSE(reply.body_.contains("key"));
  CHECK(reply.body_.at("checked").as_bool() == false);

  // A field the folder does not declare, a missing one, and demo for a vendor without one are refused.
  const auto at = folder.string();
  CHECK(
    code(login_with(store, "acme", "real", R"({"appKey":"a","appSecret":"b","x":"c"})", at)) == "auth.bad_document"
  );
  CHECK(code(login_with(store, "acme", "real", R"({"appKey":"a"})", at)) == "auth.bad_document");
  const auto vendor = project.folder("vendor", VENDOR_TOML);
  CHECK(code(login_with(store, "vendor", "demo", R"({"api_token":"t"})", vendor.string())) == "usage.bad_value");
  REQUIRE(login_with(store, "vendor", "", R"({"api_token":")" + SECRET + R"("})", vendor.string()).exit_ == Exit::OK);
  CHECK(store.items_.count("vendor/real") == 1);

  // A name that is not built in needs its folder; a built-in one takes none; --adapter needs --broker.
  const auto before = store.items_;
  CHECK(code(login_with(store, "nobroker", "", "{}")) == "usage.missing_argument");
  CHECK(code(login_with(store, "kis", "demo", "{}", at)) == "usage.bad_value");
  auth::LoginRequest unnamed;
  unnamed.adapter_ = at;
  unnamed.with_secret_ = true;
  const Input none("{}");
  std::ostringstream prompts;
  const auto unnamed_reply = auth::login(unnamed, none.fd(), prompts, &store);
  CHECK(code(unnamed_reply) == "usage.missing_argument");
  CHECK(std::string(unnamed_reply.body_.at("error").at("message").as_string()).find("--adapter") != std::string::npos);
  // No folder at the path, and a folder another user could change.
  CHECK(code(login_with(store, "acme", "real", "{}", (project.root() / "none").string())) == "usage.bad_value");
  ::chmod(folder.c_str(), 0777);
  CHECK(code(login_with(store, "acme", "real", R"({"appKey":"a","appSecret":"b"})", at)) == "adapter.unsafe");
  ::chmod(folder.c_str(), 0700);
  CHECK(store.items_ == before);

  // The path against the working directory, bound as the folder's canonical path.
  {
    const InDir in(project.root());
    REQUIRE(
      login_with(store, "acme", "real", R"({"appKey":"a","appSecret":"b"})", "adapters/../adapters/acme").exit_ ==
      Exit::OK
    );
  }
  CHECK(store.items_.at("acme/real").find(R"("adapter":")" + folder.string() + R"(")") != std::string::npos);

  // Status reads no folder login: neither its document nor the folder it is bound to.
  store.gets_ = 0;
  const auto status = auth::status({}, &store);
  REQUIRE(status.exit_ == Exit::OK);
  CHECK(store.gets_ == 0);
  const auto text = json::serialize(status.body_);
  CHECK(text.find(KEY) == std::string::npos);
  CHECK(text.find(SECRET) == std::string::npos);
  CHECK(text.find("SE****") == std::string::npos);
  const auto& rows = status.body_.at("logins").as_array();
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].at("broker").as_string() == "acme");
  CHECK(rows[0].at("adapter").is_null());
  CHECK_FALSE(rows[0].as_object().contains("key"));
}

TEST_CASE("a broker folder's login reaches the adapter only for the folder it is bound to") {
  const BrokerFolders project;
  const auto folder = project.folder("vendor", VENDOR_TOML);
  const auto copy = project.folder("copy", VENDOR_TOML);
  const auto found = auth::lookup_broker("vendor", folder.string());
  const auto other = auth::lookup_broker("vendor", copy.string());
  REQUIRE(found.folder_);
  REQUIRE(other.folder_);
  const auto stored =
    auth::folder_login_text(folder.string(), json::parse(R"({"api_token":")" + SECRET + R"("})").as_object());

  std::string document;
  CHECK_FALSE(auth::folder_login(stored, "vendor/real", *found.folder_, false, document));
  CHECK(document == R"({"api_token":")" + SECRET + R"("})");

  // The same files in another folder (a project cloned elsewhere), a login stored before logins were bound, and an
  // envelope that is not cmdatl's: each refused, the stored text never quoted, nothing handed on.
  const auto refused = [&](const std::string& text, const auth::UserBroker& broker) {
    std::string out;
    const auto reply = auth::folder_login(text, "vendor/real", broker, false, out);
    REQUIRE(reply);
    CHECK(out.empty());
    CHECK(reply->exit_ == Exit::FAILED);
    CHECK(json::serialize(reply->body_).find(SECRET) == std::string::npos);
    return *reply;
  };
  const auto moved = refused(stored, *other.folder_);
  CHECK(code(moved) == "auth.adapter_mismatch");
  const std::string message(moved.body_.at("error").at("message").as_string());
  CHECK(message.find(folder.string()) != std::string::npos);
  CHECK(message.find(copy.string()) != std::string::npos);
  CHECK(
    std::string(moved.body_.at("error").at("next").as_string()) ==
    "the operator logs in for this folder: cmdatl auth login --broker vendor --env real --adapter " + copy.string()
  );
  const std::string folder_text = folder.string();
  for (
    const auto& text : {
      R"({"api_token":")" + SECRET + R"("})",
      std::string("not json ") + SECRET,
      R"({"version":2,"adapter":")" + folder_text + R"(","login":{"api_token":")" + SECRET + R"("}})",
      R"({"version":1,"adapter":")" + folder_text + R"(","login":")" + SECRET + R"("})",
      R"({"version":1,"adapter":")" + folder_text + R"(","login":{},"extra":")" + SECRET + R"("})",
      R"({"version":1,"login":{"api_token":")" + SECRET + R"("}})",
    }
  ) {
    INFO(text);
    CHECK(code(refused(text, *found.folder_)) == "auth.adapter_mismatch");
  }
}

TEST_CASE("a broker folder's login outlives its folder and its binding: status shows it, logout removes it") {
  Store store;
  {
    const BrokerFolders project;
    const auto folder = project.folder("vendor", VENDOR_TOML);
    REQUIRE(login_with(store, "vendor", "", R"({"api_token":"t"})", folder.string()).exit_ == Exit::OK);
  }
  // A login stored before logins were bound to a folder: listed and removed by name all the same.
  store.items_["acme/demo"] = R"({"appKey":"k","appSecret":"s","demo":true})";
  const auto status = auth::status({}, &store);
  REQUIRE(status.body_.at("logins").as_array().size() == 2);
  CHECK(status.body_.at("logins").as_array()[1].at("adapter").is_null());
  const Input none("");
  std::ostringstream prompts;
  auto removed = auth::logout({std::string("vendor"), std::nullopt}, none.fd(), prompts, &store);
  REQUIRE(removed.exit_ == Exit::OK);
  CHECK(removed.body_.at("removed").as_string() == "vendor/real");
  removed = auth::logout({std::string("acme"), std::nullopt}, none.fd(), prompts, &store);
  REQUIRE(removed.exit_ == Exit::OK);
  CHECK(store.items_.empty());
}
#endif
