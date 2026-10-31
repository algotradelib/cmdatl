#pragma once

// A paper account in a scratch directory for the tests: the operator's file, its bars and the state root, with
// CMDATL_CONFIG and XDG_STATE_HOME pointed at them while the fixture lives (never at the real home).

#include "./support.hpp"

#include <cmdatl/account/account.hpp>
#include <cmdatl/cli/cli.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace support {

/** @brief Three trading dates of two symbols, 2024-03-25 to 2024-03-27; 000660 does not trade on the last. */
inline const std::string BARS =
  "symbol,date,open,high,low,close,volume\n"
  "005930,2024-03-25,78000,79000,77500,78500,1000000\n"
  "000660,2024-03-25,180000,182000,178000,181000,500000\n"
  "005930,2024-03-26,78500,80000,78000,79800,1200000\n"
  "000660,2024-03-26,181000,185000,180000,184000,600000\n"
  "005930,2024-03-27,79800,81000,79000,80500,900000\n";

/** @brief The parts of an operator's file a test changes. */
struct ConfigParts {
  /** @brief The `[account]` table's body. */
  std::string account_{"broker = \"paper\"\nnumber = \"paper-0001\"\n"};
  /** @brief The `[limits]` table's body. */
  std::string limits_{
    "symbols = [\"005930\", \"000660\"]\n"
    "position = { \"005930\" = 100, \"000660\" = 20 }\n"
    "order_value = 5000000\n"
    "daily_orders = 3\n"
  };
  /** @brief The `[paper]` table's body. */
  std::string paper_{"bars = \"bars.csv\"\ncash = 100000000\nstart = \"2024-03-25T16:00\"\n"};
  /** @brief Lines before the tables (top-level keys). */
  std::string top_;

  /**
   * @brief The whole file.
   * @return The text.
   */
  std::string text() const {
    return top_ + "[account]\n" + account_ + "\n[limits]\n" + limits_ + "\n[paper]\n" + paper_;
  }
};

/** @brief Sets an environment variable while its owner lives, and puts back what was there. */
class ScopedVariable {
public:
  /**
   * @brief Sets or unsets the variable.
   * @param name The variable.
   * @param value The value, or nothing to unset it.
   */
  ScopedVariable(std::string name, const std::optional<std::string>& value)
  : name_(std::move(name)) {
    if (const char* old = std::getenv(name_.c_str())) {
      old_ = std::string(old);
    }
    if (value) {
      ::setenv(name_.c_str(), value->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ScopedVariable(const ScopedVariable&) = delete;
  ScopedVariable& operator=(const ScopedVariable&) = delete;

  /** @brief Puts back what was there. */
  ~ScopedVariable() {
    if (old_) {
      ::setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  std::optional<std::string> old_;
};

/** @brief A paper account in a scratch directory, named by CMDATL_CONFIG and XDG_STATE_HOME while it lives. */
class PaperFixture {
public:
  /**
   * @brief Writes the operator's file and the bars.
   * @param parts The operator's file.
   * @param bars The bars.
   */
  explicit PaperFixture(const ConfigParts& parts = {}, const std::string& bars = BARS)
  : config_var_("CMDATL_CONFIG", (dir_.path() / "paper.toml").string()),
    state_var_("XDG_STATE_HOME", (dir_.path() / "state").string()),
    atlc_var_("CMDATL_ATLC", std::nullopt) {
    write_config(parts.text());
    write_bars(bars);
  }

  PaperFixture(const PaperFixture&) = delete;
  PaperFixture& operator=(const PaperFixture&) = delete;

  /**
   * @brief The scratch directory.
   * @return The path.
   */
  const std::filesystem::path& dir() const noexcept { return dir_.path(); }

  /**
   * @brief The operator's file.
   * @return The path.
   */
  std::filesystem::path config() const { return dir() / "paper.toml"; }

  /**
   * @brief The account's state directory.
   * @return The path.
   */
  std::filesystem::path account() const { return dir() / "state" / "cmdatl" / "accounts" / "paper-paper-paper-0001"; }

  /**
   * @brief The paper log.
   * @return The path.
   */
  std::filesystem::path log() const { return account() / "paper.jsonl"; }

  /**
   * @brief The journal.
   * @return The path.
   */
  std::filesystem::path journal() const { return account() / "journal.jsonl"; }

  /**
   * @brief The armed file.
   * @return The path.
   */
  std::filesystem::path armed() const { return dir() / "paper.armed"; }

  /**
   * @brief Writes the operator's file.
   * @param text Its text.
   */
  void write_config(const std::string& text) const { spit(config(), text); }

  /**
   * @brief Writes the bars.
   * @param text Their text.
   */
  void write_bars(const std::string& text) const { spit(dir() / "bars.csv", text); }

  /** @brief Creates the armed file. */
  void arm() const { spit(armed(), ""); }

  /**
   * @brief Runs a command line in process, in JSON.
   * @param args The command line, without `--json`.
   * @return What it wrote and returned.
   */
  Ran json(std::vector<std::string> args) const {
    args.insert(args.begin(), "--json");
    return support::run(args);
  }

private:
  ScratchDir dir_;
  ScopedVariable config_var_;
  ScopedVariable state_var_;
  ScopedVariable atlc_var_;
};

/** @brief An account opened in process, with the streams its output writes to. */
struct OpenAccount {
  /** @brief stdout. */
  std::ostringstream out_;
  /** @brief stderr. */
  std::ostringstream err_;
  /** @brief The output. */
  cmdatl::Output output_{out_, err_, cmdatl::Rendering{}, "paper"};
  /** @brief The operator's file. */
  cmdatl::ConfigSnapshot snapshot_;
  /** @brief The account, or null. */
  std::unique_ptr<cmdatl::Account> account_;
  /** @brief The refusal, when not opened. */
  std::optional<cmdatl::Reply> refusal_;
};

/**
 * @brief Opens the account the environment names, in process, as an account command would.
 * @return The opened account or its refusal.
 */
inline std::unique_ptr<OpenAccount> open_account() {
  auto opened = std::make_unique<OpenAccount>();
  opened->snapshot_ = cmdatl::load_operator_config();
  REQUIRE(opened->snapshot_.state_ == cmdatl::ConfigSnapshot::State::OK);
  auto result = cmdatl::Account::open(opened->snapshot_, opened->output_, [] { return false; });
  opened->account_ = std::move(result.account_);
  opened->refusal_ = std::move(result.refusal_);
  return opened;
}

/**
 * @brief The error code of a reply, or empty for a success.
 * @param reply The reply.
 * @return The code.
 */
inline std::string code_of_reply(const cmdatl::Reply& reply) {
  const auto* error = reply.body_.if_contains("error");
  return error == nullptr ? std::string() : std::string(error->as_object().at("code").as_string());
}

/**
 * @brief An order request for the paper market.
 * @param symbol The symbol.
 * @param buy Whether a buy.
 * @param quantity The quantity.
 * @param price The limit, or empty for a market order.
 * @return The request.
 */
inline algotradelib::OrderRequest order(
  const std::string& symbol,
  bool buy,
  const std::string& quantity,
  const std::string& price = ""
) {
  algotradelib::OrderRequest request;
  request.instrument_ = cmdatl::paper::kr(symbol);
  request.side_ = buy ? algotradelib::OrderSide::BUY : algotradelib::OrderSide::SELL;
  request.type_ = price.empty() ? algotradelib::OrderType::MARKET : algotradelib::OrderType::LIMIT;
  request.quantity_ = algotradelib::Decimal64::parse(quantity);
  if (!price.empty()) {
    request.price_ = algotradelib::Decimal64::parse(price);
  }
  return request;
}

/**
 * @brief A KST instant.
 * @param text `YYYY-MM-DDTHH:MM`.
 * @return The instant.
 */
inline cmdatl::paper::Clock::time_point kst(const std::string& text) {
  const auto at = cmdatl::paper::parse_kst(text);
  REQUIRE(at.has_value());
  return *at;
}

}  // namespace support
