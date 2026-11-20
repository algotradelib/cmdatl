// A test-only cmdatl, never installed: the whole command line on the shipped KIS setup (`cli/builtin.hpp`) and the shipped
// table, with three differences, all made in this file: the logins come from files the test wrote (made-up ones), every
// KIS client talks to a loopback server, and the live clock starts at an instant the test gives.
//
//   kis-command INSTANT_MS TRADING_PORT MARKET_PORT LOGINS_DIR -- CMDATL_ARGS...
//
// INSTANT_MS is the live clock's start, in milliseconds since the epoch; the clock then moves with the steady clock.
// TRADING_PORT serves the account's client, MARKET_PORT the real login's client on a demo account (unused on a real
// account, whose one client serves both). LOGINS_DIR holds `kis-demo` and `kis-real`, each a login document; a missing
// file is a login not stored.
// Once a file `spoil-token` appears in LOGINS_DIR, the next call of the setup's `now()` or `stopped()` empties the account
// client's token in memory, once: the client's next request takes the token step (the store, then KIS) as it would for
// a token that expired then. The command reads the clock when it stamps a journal line and at the boundary, and asks
// for a stop before each read and at the boundary, so a test that creates the file while a held request proves where the
// command is knows which request comes next.
//
// It is the only test code that compiles the KIS client with its transport replaced.

#include <cmdatl/cli/builtin.hpp>
#include <cmdatl/cmdatl>

#include <algotradelib/kis>

#include <boost/asio/ssl/context.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "./local_https_server.hpp"

namespace algotradelib::kis {

/** @brief The KIS client's test access, defined here as the library's own tests define it. */
struct KisClientTestAccess {
  /**
   * @brief Points a blocking client at a loopback server with the transport options KIS's client resolves for its
   *   configuration (a demo order on a fresh connection) and no rate budget: the one option cmdatl sets
   *   (`paced_client` in `brokers/stores.hpp`) is the pacing, which is the library's to test.
   * @param client The client.
   * @param context The TLS context trusting the server; it must outlive the client.
   * @param port The server's port.
   */
  static void connect(KisBlockingClient& client, ssl::context& context, const std::string& port) {
    auto& inner = *client.client_;
    inner.http_ = std::make_shared<network::AsyncHttpClient>(
      inner.strand_, context, "localhost", port,
      KisClient::transport_options(network::HttpClientOptions{}, inner.config_)
    );
  }

  /**
   * @brief Empties the token a blocking client holds, as the library's own tests spoil one.
   * @param client The client.
   */
  static void spoil_token(KisBlockingClient& client) {
    client.client_->token_ = KisRest{};
  }
};

} // namespace algotradelib::kis

namespace {

/** @brief The logins, read from the files the test wrote. */
class FileLogins : public cmdatl::auth::CredentialStore {
public:
  /**
   * @brief Reads the folder.
   * @param dir The folder.
   */
  explicit FileLogins(const std::filesystem::path& dir) {
    for (
      const auto& [file, name] : {std::pair<const char*, const char*>{"kis-demo", "kis/demo"}, {"kis-real", "kis/real"}}
    ) {
      std::ifstream in(dir / file, std::ios::binary);
      if (in) {
        items_[name] = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
    }
  }

  /** @brief The store's kind. */
  std::string kind() const override {
    return "memory";
  }

  /** @brief Lists nothing: never called on this path. */
  cmdatl::auth::StoreList list() override {
    return {};
  }

  /**
   * @brief A login.
   * @param name Its name.
   * @return It, or not found.
   */
  cmdatl::auth::StoreAnswer get(const std::string& name) override {
    cmdatl::auth::StoreAnswer answer;
    const auto found = items_.find(name);
    answer.status_ = found == items_.end() ? cmdatl::auth::StoreStatus::NOT_FOUND : cmdatl::auth::StoreStatus::OK;
    if (found != items_.end()) {
      answer.value_ = found->second;
    }
    return answer;
  }

  /** @brief Never called on this path. */
  cmdatl::auth::StoreAnswer put(const std::string&, const std::string&) override {
    return {};
  }

  /** @brief Never called on this path. */
  cmdatl::auth::StoreAnswer remove(const std::string&) override {
    return {};
  }

private:
  std::map<std::string, std::string> items_;
};

/** @brief Where the clients go and when the clock starts. */
struct Wiring {
  /** @brief The live clock's start. */
  cmdatl::Clock::time_point instant_;
  /** @brief The trading server's port. */
  std::string trading_port_;
  /** @brief The market server's port. */
  std::string market_port_;
  /** @brief The file whose appearance spoils the account client's token. */
  std::filesystem::path spoil_;
};

/** @brief The shipped KIS setup, its clients pointed at the loopback servers after `identify`, on the test's clock. */
class WiredSetup {
public:
  /**
   * @brief Wraps the shipped setup.
   * @param store The logins, borrowed.
   * @param wiring Where the clients go and when the clock starts.
   */
  WiredSetup(cmdatl::auth::CredentialStore& store, Wiring wiring)
  : context_(boost::asio::ssl::context::tls_client),
    wiring_(std::move(wiring)),
    started_(std::chrono::steady_clock::now()),
    inner_(&store) {
    support::trust_localhost_certificate(context_);
  }

  /**
   * @brief The shipped `identify`, then each client replaced once: on a real account the trading and market sessions
   *   share one client.
   * @param config The operator's file.
   * @param effective Its number set to the login's account.
   * @return The refusal, or nothing.
   */
  std::optional<cmdatl::Reply> identify(const cmdatl::OperatorConfig& config, cmdatl::OperatorConfig& effective) {
    if (auto refused = inner_.identify(config, effective)) {
      return refused;
    }
    auto& trading = inner_.trading().client();
    auto& market = inner_.market().client();
    algotradelib::kis::KisClientTestAccess::connect(trading, context_, wiring_.trading_port_);
    if (&market != &trading) {
      algotradelib::kis::KisClientTestAccess::connect(market, context_, wiring_.market_port_);
    }
    wired_ = true;
    return std::nullopt;
  }

  /** @brief The account's session. */
  cmdatl::BuiltinSessions<algotradelib::kis::KisBlockingClient>::Session& trading() {
    return inner_.trading();
  }

  /** @brief The market-data session. */
  cmdatl::BuiltinSessions<algotradelib::kis::KisBlockingClient>::Session& market() {
    return inner_.market();
  }

  /** @brief The test's instant, moved on by the steady clock since this process started. */
  cmdatl::Clock::time_point now() {
    spoil_once();
    return wiring_.instant_ +
      std::chrono::duration_cast<cmdatl::Clock::duration>(std::chrono::steady_clock::now() - started_);
  }

  /** @brief The shipped stop. */
  bool stopped() {
    spoil_once();
    return inner_.stopped();
  }

private:
  /** @brief Empties the account client's token the first time the test's file exists, once the clients are wired. */
  void spoil_once() {
    std::error_code ec;
    if (!spoiled_ && wired_ && std::filesystem::exists(wiring_.spoil_, ec)) {
      spoiled_ = true;
      algotradelib::kis::KisClientTestAccess::spoil_token(inner_.trading().client());
    }
  }

  // Declared first, so it is destroyed after the clients that borrow it.
  boost::asio::ssl::context context_;
  Wiring wiring_;
  std::chrono::steady_clock::time_point started_;
  cmdatl::BuiltinSetup<algotradelib::kis::KisBlockingClient> inner_;
  bool wired_{false};
  bool spoiled_{false};
};

/**
 * @brief Whether stdout is a terminal.
 * @return Whether it is.
 */
bool stdout_is_terminal() {
  return ::isatty(STDOUT_FILENO) != 0;
}

} // namespace

/**
 * @brief The test-only cmdatl; see the file comment.
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return The exit code: cmdatl's, or 64 for a bad command line of its own.
 */
int main(int argc, char** argv) {
  cmdatl::handle_signals();
  const std::vector<std::string> all(argv + 1, argv + argc);
  if (all.size() < 5 || all[4] != "--") {
    std::cerr << "usage: kis-command INSTANT_MS TRADING_PORT MARKET_PORT LOGINS_DIR -- CMDATL_ARGS...\n";
    return 64;
  }
  try {
    Wiring wiring;
    wiring.instant_ = cmdatl::Clock::time_point(std::chrono::milliseconds(std::stoll(all[0])));
    wiring.trading_port_ = all[1];
    wiring.market_port_ = all[2];
    wiring.spoil_ = std::filesystem::path(all[3]) / "spoil-token";
    FileLogins logins(all[3]);
    const std::vector<std::string> args(all.begin() + 5, all.end());
    const cmdatl::BrokerPath kis =
      [&](
        const cmdatl::Command& command, const cmdatl::Invocation& invocation, const cmdatl::ConfigSnapshot& snapshot,
        cmdatl::Output& output
      ) {
        WiredSetup setup(logins, wiring);
        return cmdatl::broker_command_reply(command, invocation, *snapshot.config_, output, setup);
      };
    return cmdatl::run(args, std::cout, std::cerr, stdout_is_terminal(), kis);
  } catch (const std::exception& e) {
    std::cerr << "kis-command: internal error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "kis-command: internal error\n";
  }
  return 1;
}
