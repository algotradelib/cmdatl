#pragma once

// The stand-in for a broker's account: the real KIS login checks on an in-memory store and the library's mock sessions
// behind the shipped command path (`broker_command_reply`), so no case can reach a broker. Every login here is made up.

#include "./paper-support.hpp"

#include <cmdatl/cli/kis.hpp>

#include <algotradelib/mock>

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)

namespace broker_support {

namespace atl = algotradelib;
namespace runner = algotradelib::runner;
using support::kst;
using support::Ran;

/** @brief The account's login: a demo login of account 12345678-01. */
inline const std::string DEMO_LOGIN =
  R"({"app_key":"made-up-demo-key","app_secret":"MADE-UP-DEMO-SECRET","account_number":"12345678","demo":true})";
/** @brief The real login a demo account reads market data with. */
inline const std::string REAL_LOGIN =
  R"({"app_key":"made-up-real-key","app_secret":"MADE-UP-REAL-SECRET","account_number":"87654321","demo":false})";

/** @brief The account directory a KIS demo command takes: the login's account. */
inline const std::string ACCOUNT = "kis-demo-12345678-01";
/** @brief Its journal's scope. */
inline const std::string SCOPE = "kis:demo:12345678-01";

/**
 * @brief The operator's file for a KIS demo account.
 * @param number The `[account] number` line's value, or empty for none.
 * @param extra More `[limits]` lines, such as `phases = [...]`.
 * @return The text.
 */
inline std::string kis_config(const std::string& number = "12345678-01", const std::string& extra = "") {
  return "[account]\nbroker = \"kis\"\n" + (number.empty() ? std::string() : "number = \"" + number + "\"\n") +
    "\n[limits]\nsymbols = [\"005930\", \"000660\"]\nposition = { \"005930\" = 100, \"000660\" = 20 }\n"
    "order_value = 5000000\ndaily_orders = 3\n" +
    extra;
}

/** @brief A credential store in memory, recording what was asked. */
class Logins : public cmdatl::auth::CredentialStore {
public:
  /** @brief The logins by name. */
  std::map<std::string, std::string> items_;
  /** @brief Every name asked for. */
  std::vector<std::string> asked_;
  /** @brief How every read answers, when set: a locked or unusable store. */
  std::optional<cmdatl::auth::StoreStatus> failing_;

  /** @brief The store's kind. @return `memory`. */
  std::string kind() const override {
    return "memory";
  }

  /** @brief Lists nothing: never called here. @return Nothing. */
  cmdatl::auth::StoreList list() override {
    return {};
  }

  /**
   * @brief A login.
   * @param name Its name.
   * @return It, or not found.
   */
  cmdatl::auth::StoreAnswer get(const std::string& name) override {
    asked_.push_back(name);
    cmdatl::auth::StoreAnswer answer;
    if (failing_) {
      answer.status_ = *failing_;
      return answer;
    }
    const auto found = items_.find(name);
    answer.status_ = found == items_.end() ? cmdatl::auth::StoreStatus::NOT_FOUND : cmdatl::auth::StoreStatus::OK;
    if (found != items_.end()) {
      answer.value_ = found->second;
    }
    return answer;
  }

  /** @brief Never called here. @return Nothing. */
  cmdatl::auth::StoreAnswer put(const std::string&, const std::string&) override {
    return {};
  }

  /** @brief Never called here. @return Nothing. */
  cmdatl::auth::StoreAnswer remove(const std::string&) override {
    return {};
  }
};

/**
 * @brief A broker error a test scripts, to see whether a call consumed it.
 * @param code Its code.
 * @return The error, whose message stands for a broker's body.
 */
inline atl::BrokerError marker(const std::string& code) {
  atl::BrokerError error;
  error.category_ = atl::ErrorCategory::TRANSPORT;
  error.broker_code_ = code;
  error.message_ = "MADE-UP-BODY-TEXT";
  return error;
}

/**
 * @brief The library's mock session, counting the calls a test asserts on and running a test's hooks before some.
 *
 * The hooks run before the call reaches the mock client; one may script a fault on the client for that call, or throw.
 */
struct HookedSession : atl::BlockingSession<atl::mock::MockClient> {
  using atl::BlockingSession<atl::mock::MockClient>::BlockingSession;
  /** @brief The session's order reference. */
  using Ref = atl::BlockingSession<atl::mock::MockClient>::Ref;

  /** @brief Runs before each buying-power read, when set. */
  std::function<void()> before_power_;
  /** @brief Runs before each order-list read, when set. */
  std::function<void()> before_list_;
  /** @brief Runs before each quote, with the quote's number in this session (1 is the first), when set. */
  std::function<void(int)> before_quote_;
  /** @brief Runs before each calendar read, with its number in this session, when set. */
  std::function<void(int)> before_calendar_;
  /** @brief Runs before each placement reaches the mock client, when set. */
  std::function<void()> before_place_;
  /** @brief Runs before each cancellation reaches the mock client, with its number in this session, when set. */
  std::function<void(int)> before_cancel_;
  /** @brief Placements that reached this session. */
  int places_{0};
  /** @brief Cancellations that reached this session. */
  int cancels_{0};
  /** @brief Quotes, reference data and calendars read through this session. */
  int market_reads_{0};
  /** @brief Quotes read through this session. */
  int quotes_{0};
  /** @brief Calendars read through this session. */
  int calendars_{0};

  /**
   * @brief The buying power, after `before_power_`.
   * @param currency The currency.
   * @return The answer.
   */
  auto buying_power(atl::Currency currency) {
    if (before_power_) {
      before_power_();
    }
    return BlockingSession::buying_power(std::move(currency));
  }

  /**
   * @brief The working orders, after `before_list_`.
   * @return The answer.
   */
  auto working_orders() {
    if (before_list_) {
      before_list_();
    }
    return BlockingSession::working_orders();
  }

  /**
   * @brief A quote, counted, after `before_quote_`.
   * @param instrument The instrument.
   * @return The answer.
   */
  auto quote(atl::InstrumentId instrument) {
    ++market_reads_;
    ++quotes_;
    if (before_quote_) {
      before_quote_(quotes_);
    }
    return BlockingSession::quote(std::move(instrument));
  }

  /**
   * @brief Reference data, counted.
   * @param subject The instrument.
   * @return The answer.
   */
  auto instrument(atl::InstrumentId subject) {
    ++market_reads_;
    return BlockingSession::instrument(std::move(subject));
  }

  /**
   * @brief The calendar, counted, after `before_calendar_`.
   * @param country The country.
   * @param from The first date.
   * @return The answer.
   */
  auto calendar(atl::Country country, std::optional<atl::Date> from = std::nullopt) {
    ++market_reads_;
    ++calendars_;
    if (before_calendar_) {
      before_calendar_(calendars_);
    }
    return BlockingSession::calendar(country, std::move(from));
  }

  /**
   * @brief A placement, after `before_place_`, counted once it reaches the mock client.
   * @param order The order.
   * @return The answer.
   */
  auto place(atl::OrderRequest order) {
    if (before_place_) {
      before_place_();
    }
    ++places_;
    return BlockingSession::place(std::move(order));
  }

  /**
   * @brief A cancellation, after `before_cancel_`, counted once it reaches the mock client.
   * @param ref The order.
   * @param instruction What to cancel.
   * @return The answer.
   */
  auto cancel(const Ref& ref, const atl::AmendInstruction& instruction) {
    if (before_cancel_) {
      before_cancel_(cancels_ + 1);
    }
    ++cancels_;
    return BlockingSession::cancel(ref, instruction);
  }
};

/**
 * @brief A stand-in setup: the real login checks on an in-memory store, and the library's mock sessions.
 *
 * Its market-data session is its trading session, as on a real account, unless `split_` is set: then it is a second
 * session on its own client over the same market, as on a demo account, whose trading host answers no market data.
 */
struct StandIn {
  /** @brief The logins. */
  Logins logins_;
  /** @brief The market, from the fixture's bars. */
  std::unique_ptr<atl::mock::Market> market_;
  /** @brief The trading session's client, for scripting faults. */
  std::shared_ptr<atl::mock::MockClient> client_;
  /** @brief The trading session, and the market-data one unless split. */
  std::unique_ptr<HookedSession> session_;
  /** @brief The split market-data session's client. */
  std::shared_ptr<atl::mock::MockClient> market_client_;
  /** @brief The split market-data session. */
  std::unique_ptr<HookedSession> market_session_;
  /** @brief Whether market data comes from the split session. */
  bool split_{false};
  /** @brief The clock's answers, in order; the last repeats. */
  std::vector<cmdatl::Clock::time_point> times_{kst("2024-03-26T10:00")};
  /** @brief How many times the clock was read. */
  int clock_calls_{0};
  /** @brief How many times the stop was checked. */
  int checks_{0};
  /** @brief Stop from the check after this many, or never. */
  int stop_after_{-1};
  /** @brief How many times `identify` ran. */
  int identified_{0};
  /** @brief Whether the clock checks that the calendar was read before it, by a scripted calendar fault. */
  bool probe_calendar_{false};
  /** @brief Whether the calendar's fault was taken before the clock was read. */
  bool calendar_before_clock_{false};
  /** @brief Throw from the clock once it was read this many times, or never. */
  int throw_after_{-1};
  /** @brief Whether that throw is a stop (`cmdatl::Stopped`) rather than another exception. */
  bool throw_stop_{false};
  /** @brief The clock read (0 is the first) at which `hook_` runs, or none. */
  int hook_at_{-1};
  /** @brief What runs at that read, before it answers. */
  std::function<void()> hook_;

  /** @brief The market of the fixture's bars, with both logins stored. */
  StandIn() {
    cmdatl::MarketData data;
    REQUIRE(!cmdatl::parse_bars(support::BARS, data));
    std::vector<atl::BarSeries> series;
    for (const auto& entry : data.bars_) {
      series.push_back(cmdatl::mock_series(entry.first, entry.second));
    }
    atl::mock::MarketSettings settings;
    settings.cash_ = atl::Money{atl::Decimal64(100000000, 0), atl::Currency::krw()};
    settings.start_ = kst("2024-03-25T16:00");
    market_ = std::make_unique<atl::mock::Market>(std::move(series), std::move(settings));
    client_ = std::make_shared<atl::mock::MockClient>(*market_);
    session_ = std::make_unique<HookedSession>(client_, atl::mock::MockAccount{});
    market_client_ = std::make_shared<atl::mock::MockClient>(*market_);
    market_session_ = std::make_unique<HookedSession>(market_client_, atl::mock::MockAccount{});
    logins_.items_["kis/demo"] = DEMO_LOGIN;
    logins_.items_["kis/real"] = REAL_LOGIN;
  }

  /**
   * @brief Reads and checks the logins as KIS's setup does; the sessions are the mock's.
   * @param config The operator's file.
   * @param effective Its number set to the login's account.
   * @return The refusal, or nothing.
   */
  std::optional<cmdatl::Reply> identify(const cmdatl::OperatorConfig& config, cmdatl::OperatorConfig& effective) {
    ++identified_;
    std::vector<std::string> documents;
    if (
      auto refused = cmdatl::runs::read_run_logins(config, cmdatl::auth::lookup_broker("kis", ""), &logins_, documents)
    ) {
      return refused;
    }
    atl::kis::KisConfig trading;
    atl::kis::KisAccount account;
    std::optional<atl::kis::KisConfig> market;
    if (const auto mismatch = cmdatl::read_kis_logins(config, documents, trading, account, market, effective)) {
      return cmdatl::error_reply(cmdatl::Exit::FAILED, "auth.account_mismatch", *mismatch);
    }
    return std::nullopt;
  }

  /** @brief The trading session. @return It. */
  HookedSession& trading() {
    return *session_;
  }

  /** @brief The market-data session: the trading one, or the split one. @return It. */
  HookedSession& market() {
    return split_ ? *market_session_ : *session_;
  }

  /** @brief The scripted clock. @return Its next answer. */
  cmdatl::Clock::time_point now() {
    if (clock_calls_ == hook_at_ && hook_) {
      hook_();
    }
    if (throw_after_ >= 0 && clock_calls_ >= throw_after_) {
      if (throw_stop_) {
        throw cmdatl::Stopped();
      }
      throw std::runtime_error("the stand-in's clock failed");
    }
    if (probe_calendar_) {
      // The fault is still there only when the calendar was not read yet.
      calendar_before_clock_ = !unconsumed(
        [this] {
          return session_->calendar(atl::Country::KR);
        },
        "calendar-first"
      );
    }
    const auto at = times_[std::min<std::size_t>(static_cast<std::size_t>(clock_calls_), times_.size() - 1)];
    ++clock_calls_;
    return at;
  }

  /** @brief The scripted stop. @return Whether a stop was asked for. */
  bool stopped() {
    ++checks_;
    return stop_after_ >= 0 && checks_ > stop_after_;
  }

  /**
   * @brief Whether a scripted fault is still waiting for its call, which therefore was not made.
   * @tparam Read The call.
   * @param read Makes the call.
   * @param code The fault's code.
   * @return Whether the call answers the fault.
   */
  template <typename Read>
  static bool unconsumed(Read read, const std::string& code) {
    const auto answer = read();
    return !answer && answer.error().broker_code_ == code;
  }
};

/** @brief A KIS demo account in a scratch directory, its token folder there too. */
struct KisFixture {
  /** @brief The operator's file and state root. */
  support::PaperFixture paper_;
  /** @brief The cache root. */
  support::ScopedVariable cache_{"XDG_CACHE_HOME", (paper_.dir() / "cache").string()};

  /**
   * @brief Writes the operator's file.
   * @param number Its account number, or empty for none.
   * @param extra More `[limits]` lines.
   */
  explicit KisFixture(const std::string& number = "12345678-01", const std::string& extra = "") {
    paper_.write_config(kis_config(number, extra));
  }

  /**
   * @brief An account's directory.
   * @param name Its name.
   * @return The path.
   */
  std::filesystem::path account(const std::string& name = ACCOUNT) const {
    return paper_.dir() / "state" / "cmdatl" / "accounts" / name;
  }

  /** @brief The account's journal. @return The path. */
  std::filesystem::path journal_file() const {
    return account() / "journal.jsonl";
  }

  /**
   * @brief Writes the account's journal through the library's writer.
   * @param entries The entries.
   * @param scope Its scope.
   */
  void journal(const std::vector<runner::JournalEntry>& entries, const std::string& scope = SCOPE) const {
    std::filesystem::create_directories(account());
    runner::FileJournal file((account() / "journal.jsonl").string(), scope);
    REQUIRE(file.open());
    for (const auto& entry : entries) {
      REQUIRE(file.append(entry));
    }
  }

  /**
   * @brief The journal's entries as the library reads them, none when it is absent.
   * @return The entries.
   */
  std::vector<runner::JournalEntry> entries() const {
    if (!std::filesystem::exists(journal_file())) {
      return {};
    }
    return runner::read_journal_file(journal_file().string()).entries_;
  }
};

/**
 * @brief A placement's journal line.
 * @param id The journal id.
 * @param outcome What the line says.
 * @param at When it was written.
 * @param order_id The broker's id, or empty.
 * @return The entry.
 */
inline runner::JournalEntry place_entry(
  const std::string& id, runner::JournalOutcome outcome, cmdatl::Clock::time_point at, const std::string& order_id = ""
) {
  runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = outcome;
  entry.recorded_at_ = at;
  entry.instrument_ = cmdatl::kr("005930");
  entry.side_ = atl::OrderSide::BUY;
  entry.type_ = atl::OrderType::LIMIT;
  entry.time_in_force_ = atl::TimeInForce::DAY;
  entry.session_ = atl::OrderSession::REGULAR;
  entry.venue_ = atl::Venue::UNSPECIFIED;
  entry.quantity_ = atl::Decimal64(1, 0);
  entry.price_ = atl::Decimal64(70000, 0);
  entry.baseline_ = atl::Decimal64(0, 0);
  entry.order_id_ = order_id;
  return entry;
}

/**
 * @brief Every `next` in a reply's JSON, at any depth: the error's, the reply's own and each owed row's.
 * @param value The reply, or a part of it.
 * @param out The texts found.
 */
inline void nexts_of(const boost::json::value& value, std::vector<std::string>& out) {
  if (value.is_object()) {
    for (const auto& entry : value.as_object()) {
      if (entry.key() == "next" && entry.value().is_string()) {
        out.emplace_back(entry.value().as_string());
      } else {
        nexts_of(entry.value(), out);
      }
    }
  } else if (value.is_array()) {
    for (const auto& element : value.as_array()) {
      nexts_of(element, out);
    }
  }
}

/** @brief A stream buffer that refuses every write, as a closed pipe would. */
class RefusingBuffer : public std::streambuf {
protected:
  /** @brief Refuses a character. @return End of file. */
  int_type overflow(int_type) override {
    return traits_type::eof();
  }
};

/**
 * @brief Runs a command line through `cmdatl::run` and the shipped table, with the command path on a KIS account going
 *   to a setup; checks that no made-up secret or broker body reaches either stream and that no `next` tells the reader
 *   to run `cmdatl clock`, which a broker's account refuses.
 * @tparam Setup The setup.
 * @param setup The setup.
 * @param args The command line, without `--json`.
 * @param refuse_output Whether stdout refuses every write, as a closed pipe would.
 * @return What it wrote and returned.
 */
template <typename Setup>
Ran run_on(Setup& setup, std::vector<std::string> args, bool refuse_output = false) {
  args.insert(args.begin(), "--json");
  std::ostringstream out;
  std::ostringstream err;
  RefusingBuffer refusing;
  std::ostream refused(&refusing);
  const auto exit = cmdatl::run(
    args, refuse_output ? refused : static_cast<std::ostream&>(out), err, false,
    [&setup](
      const cmdatl::Command& command, const cmdatl::Invocation& invocation, const cmdatl::ConfigSnapshot& snapshot,
      cmdatl::Output& output
    ) {
      return cmdatl::broker_command_reply(command, invocation, *snapshot.config_, output, setup);
    }
  );
  const Ran ran{exit, out.str(), err.str()};
  for (const auto* secret : {"MADE-UP-DEMO-SECRET", "MADE-UP-REAL-SECRET", "MADE-UP-BODY-TEXT"}) {
    CHECK(ran.out_.find(secret) == std::string::npos);
    CHECK(ran.err_.find(secret) == std::string::npos);
  }
  if (!ran.out_.empty()) {
    std::vector<std::string> nexts;
    nexts_of(boost::json::parse(ran.out_), nexts);
    for (const auto& next : nexts) {
      INFO(next);
      CHECK(next.find("cmdatl clock") == std::string::npos);
    }
  }
  return ran;
}

/**
 * @brief A reply's words.
 * @param value An array of strings.
 * @return The words.
 */
inline std::vector<std::string> words(const boost::json::value& value) {
  std::vector<std::string> out;
  for (const auto& word : value.as_array()) {
    out.emplace_back(word.as_string());
  }
  return out;
}

} // namespace broker_support

#endif
