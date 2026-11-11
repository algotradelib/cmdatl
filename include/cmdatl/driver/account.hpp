#pragma once

// A run on a broker's account: the driver `cmdatl run` builds for a KIS account and then becomes, in the caller's
// foreground. It trades the program through the same live wiring as a mock run (`live.hpp`), behind the same placement
// boundary (`session.hpp`), on the wall clock (`WallClock`).
//
// The account is shared with everything else that trades it, so the run holds the account's lock for its life and uses
// the account's journal, kept across runs.
// It starts only with no order resting anywhere on the account (the library's withdraw cancels every working order).
// The account's holdings of its symbols are its position from the start: every sellable share of them is its to sell.
// The daily allowance is what the journal leaves today.
// A signal or a closed output withdraws; removing the armed file quiesces.
//
// Generic over its sessions and clock, so the tests drive it with the library's mock client; `main.cpp` builds the KIS
// sessions from the logins `cmdatl run` hands over on descriptor 3 (`kis_account.hpp`).

#include "../account/account.hpp"
#include "../account/operator.hpp"
#include "../core/errors.hpp"
#include "../core/reply.hpp"
#include "../orders/rails.hpp"
#include "../paper/time.hpp"
#include "../replay/adapter_text.hpp"
#include "../system/lock.hpp"
#include "../system/process.hpp"
#include "./clock.hpp"
#include "./events.hpp"
#include "./live.hpp"
#include "./session.hpp"

#include <algotradelib/broker>
#include <algotradelib/runner>
#include <algotradelib/strategy>

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace cmdatl {
namespace driver {

namespace atl = algotradelib;
namespace json = boost::json;

/** @brief What an account run does after an unknown ending: the broker's own lists decide. */
inline constexpr const char* ACCOUNT_UNKNOWN_NEXT =
  "read the account's order list and holdings in the broker's app before trading it again; the account's journal "
  "keeps what this run could not confirm";

/**
 * @brief Whether a login is the operator file's account: the account number and product code as `12345678-01`, and the
 *   environment.
 * @param number The operator file's `[account] number`, or empty when it names none (the login decides).
 * @param demo Whether the operator file's environment is demo.
 * @param account_number The login's account number.
 * @param product_code The login's product code.
 * @param login_demo Whether the login is a demo login.
 * @return What differs, or nothing.
 */
inline std::optional<std::string> kis_account_mismatch(
  const std::string& number, bool demo, const std::string& account_number, const std::string& product_code,
  bool login_demo
) {
  if (login_demo != demo) {
    return std::string("the login is a ") + (login_demo ? "demo" : "real") +
      " login, and the operator file's account is " + (demo ? "demo" : "real");
  }
  if (!number.empty() && account_number + "-" + product_code != number) {
    return "the login's account is not the operator file's account.number " + number;
  }
  return std::nullopt;
}

/**
 * @brief What an account holds of each instrument: the net over every row of a holdings read, as the runner sizes a
 *   target against it (`Portfolio::net_quantity_of`).
 * @tparam Tag The broker's tag.
 * @param positions The holdings read's rows.
 * @param ids The instruments.
 * @return Each instrument's symbol mapped to its net quantity as a decimal string, `"0"` when no row names it; nothing
 *   when a sum does not fit a decimal.
 */
template <typename Tag>
std::optional<json::object> net_holdings(
  std::vector<atl::HoldingPosition> positions, const std::vector<atl::InstrumentId>& ids
) {
  atl::strategy::Portfolio<Tag> portfolio;
  portfolio.positions_ = std::move(positions);
  json::object held;
  for (const auto& id : ids) {
    const auto net = portfolio.net_quantity_of(id);
    if (!net) {
      return std::nullopt;
    }
    held[id.symbol_] = net->str();
  }
  return held;
}

/** @brief The account's holdings at a run's end, as its reply shows them. */
struct EndHoldings {
  /** @brief The shares held of each run symbol, as a decimal string; null for each when the read failed. */
  json::object held_;
  /** @brief Why the read failed: the broker's code, else the error's category as a word; null when it was read. */
  json::value error_;
};

/**
 * @brief Reads the account's holdings of the run's symbols once more, for the reply.
 *
 * Read again after a refusal for the broker's request rate, as the start's reads are: right after a withdraw's
 * cancellation and its confirming reads, KIS can refuse it for its per-second rate. A signal does not cut the waits
 * short, since a signal is what usually ends a run, and KIS's token minute is not waited out at the end.
 * @tparam Session The session.
 * @tparam Clock The clock: `wait_for(duration)`.
 * @tparam Note A callable taking a line, for the operator.
 * @param session The session.
 * @param symbols The run's symbols.
 * @param clock The clock waited on.
 * @param note Told before each wait.
 * @return The holdings, or why they are not known.
 */
template <typename Session, typename Clock, typename Note>
EndHoldings holdings_at_end(Session& session, const std::vector<std::string>& symbols, Clock& clock, Note&& note) {
  EndHoldings out;
  for (const auto& symbol : symbols) {
    out.held_[symbol] = nullptr;
  }
  StartBudget budget;
  budget.token_waited_ = true;
  const auto read = read_at_start(
    [&session]() {
      return session.holdings();
    },
    clock, note, budget, false
  );
  const auto& held = read.answer_;
  if (!held) {
    const auto code = replay::shown_code(held.error().broker_code_);
    out.error_ = code.empty() ? json::value(replay::category_word(held.error().category_)) : json::value(code);
    return out;
  }
  for (const auto& symbol : symbols) {
    out.held_[symbol] = "0";
  }
  for (const auto& position : held.value().positions_) {
    if (out.held_.contains(position.instrument_.symbol_)) {
      out.held_[position.instrument_.symbol_] = position.quantity_.str();
    }
  }
  return out;
}

/** @brief How often the run writes a heartbeat with what it last read. */
inline constexpr std::chrono::seconds ACCOUNT_HEARTBEAT{60};

/** @brief What a run on an account is given: fixed by the driver, never by a run's arguments. */
struct AccountRun {
  /** @brief The reply's base: `env` and `run`. */
  json::object base_;
  /** @brief The run's id. */
  std::string id_;
  /** @brief The symbols traded. */
  std::vector<std::string> symbols_;
  /** @brief The instruments traded, in the same order. */
  std::vector<atl::InstrumentId> ids_;
  /** @brief The program's warm-up, in bars. */
  std::size_t warmup_{0};
  /** @brief Where market data is read from, for the start line (`own`, or the real login on a KIS demo account). */
  std::string market_data_{"own"};
  /** @brief The account's directory: its lock and journal. */
  std::string account_dir_;
  /**
   * @brief Whether an exception's text is left out of the replies: a broker folder's adapter is not cmdatl's code, and
   *   its text could carry a credential (its errors' text is replaced before the run sees it, `user_account.hpp`).
   */
  bool opaque_errors_{false};
};

/** @brief What a reply says of an exception whose text is not shown. */
inline constexpr const char* OPAQUE_EXCEPTION =
  "the broker folder's adapter threw an exception; its text is not shown, since it could carry a credential";

/** @brief The last answers the boundary forwarded, for the heartbeat: the loop's own reads, never a call of its own. */
struct LastReads {
  /** @brief The run's symbols held, by symbol, and when that was read. */
  std::optional<std::pair<json::object, std::chrono::system_clock::time_point>> holdings_;
  /** @brief The working orders, and when they were read. */
  std::optional<std::pair<json::array, std::chrono::system_clock::time_point>> orders_;
  /** @brief The KRW buying power, and when it was read. */
  std::optional<std::pair<std::string, std::chrono::system_clock::time_point>> cash_;
};

/**
 * @brief A session that records what the loop read through it, for the heartbeat, and forwards everything, calling
 *   `on_call` before and after each call.
 * @tparam Inner The session it wraps (the placement boundary, or the market-data session).
 */
template <typename Inner>
class RecordingSession {
public:
  /** @brief The broker's capabilities. */
  using Caps = typename Inner::Caps;
  /** @brief The broker's tag. */
  using BrokerTag = typename Inner::BrokerTag;
  /** @brief The broker's traits. */
  using Traits = typename Inner::Traits;
  /** @brief An order reference. */
  using Ref = typename Inner::Ref;

  /**
   * @brief Wraps a session.
   * @param inner The session; borrowed.
   * @param symbols The run's symbols, the holdings recorded.
   * @param now The clock.
   * @param reads Where the answers are recorded; borrowed.
   * @param on_call Called before and after each broker call (the run takes a stop signal there); may be empty.
   */
  RecordingSession(
    Inner& inner, std::vector<std::string> symbols, std::function<std::chrono::system_clock::time_point()> now,
    LastReads& reads, std::function<void()> on_call = {}
  )
  : inner_(&inner), symbols_(std::move(symbols)), now_(std::move(now)), reads_(&reads), on_call_(std::move(on_call)) {}

  /** @brief Forwarded. */
  void close() noexcept {
    inner_->close();
  }
  /** @brief Forwarded. @param order The order. @return The answer. */
  auto place(atl::OrderRequest order) {
    return noted([&]() {
      return inner_->place(std::move(order));
    });
  }
  /** @brief Forwarded. @param ref The order. @param instruction What to cancel. @return The answer. */
  auto cancel(const Ref& ref, const atl::AmendInstruction& instruction) {
    return noted([&]() {
      return inner_->cancel(ref, instruction);
    });
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto bars(atl::BarQuery query) {
    return noted([&]() {
      return inner_->bars(std::move(query));
    });
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto quote(atl::InstrumentId instrument) {
    return noted([&]() {
      return inner_->quote(std::move(instrument));
    });
  }
  /** @brief Forwarded. @param subject The instrument. @return The answer. */
  auto instrument(atl::InstrumentId subject) {
    return noted([&]() {
      return inner_->instrument(std::move(subject));
    });
  }
  /** @brief Forwarded. @param country The country. @param from The first date. @return The answer. */
  auto calendar(atl::Country country, std::optional<atl::Date> from = std::nullopt) {
    return noted([&]() {
      return inner_->calendar(country, std::move(from));
    });
  }
  /** @brief Forwarded, the run's symbols recorded. @return The answer. */
  auto holdings() {
    auto answer = noted([&]() {
      return inner_->holdings();
    });
    if (answer) {
      json::object held;
      for (const auto& symbol : symbols_) {
        held[symbol] = "0";
      }
      for (const auto& position : answer.value().positions_) {
        if (held.contains(position.instrument_.symbol_)) {
          held[position.instrument_.symbol_] = position.quantity_.str();
        }
      }
      reads_->holdings_ = std::make_pair(std::move(held), now_());
    }
    return answer;
  }
  /** @brief Forwarded, KRW recorded. @param currency The currency. @return The answer. */
  auto buying_power(atl::Currency currency) {
    auto answer = noted([&]() {
      return inner_->buying_power(currency);
    });
    if (answer && currency == atl::Currency::krw()) {
      reads_->cash_ = std::make_pair(answer.value().cash_.amount_.str(), now_());
    }
    return answer;
  }
  /** @brief Forwarded, recorded. @return The answer. */
  auto working_orders() {
    auto answer = noted([&]() {
      return inner_->working_orders();
    });
    if (answer) {
      json::array rows;
      for (const auto& row : answer.value().orders_) {
        json::object order{
          {"order_id", row.order_.order_id_},
          {"symbol", row.order_.instrument_.symbol_},
          {"side", row.order_.side_ == atl::OrderSide::BUY ? "buy" : "sell"},
          {"quantity", row.order_.quantity_.str()},
        };
        if (row.order_.price_) {
          order["price"] = row.order_.price_->str();
        }
        rows.push_back(std::move(order));
      }
      reads_->orders_ = std::make_pair(std::move(rows), now_());
    }
    return answer;
  }
  /** @brief Forwarded. @param query The query. @return The answer. */
  auto order_history(const atl::OrderHistoryQuery& query) {
    return noted([&]() {
      return inner_->order_history(query);
    });
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto sellable_quantity(atl::InstrumentId instrument) {
    return noted([&]() {
      return inner_->sellable_quantity(std::move(instrument));
    });
  }
  /** @brief Forwarded. @param instrument The instrument. @return The answer. */
  auto price_limits(atl::InstrumentId instrument) {
    return noted([&]() {
      return inner_->price_limits(std::move(instrument));
    });
  }

private:
  template <typename Call>
  auto noted(Call&& call) {
    if (on_call_) {
      on_call_();
    }
    auto answer = call();
    if (on_call_) {
      on_call_();
    }
    return answer;
  }

  Inner* inner_;
  std::vector<std::string> symbols_;
  std::function<std::chrono::system_clock::time_point()> now_;
  LastReads* reads_;
  std::function<void()> on_call_;
};

/**
 * @brief The heartbeat: what the loop last read, each with its time, and the phase; no broker call of its own beyond the
 *   calendar the boundary reads (cached by the client).
 * @param at Now.
 * @param reads The last reads.
 * @param phase The phase now.
 * @return The event.
 */
inline json::object account_heartbeat(
  std::chrono::system_clock::time_point at, const LastReads& reads, atl::strategy::MarketPhase phase
) {
  json::object event{{"event", "heartbeat"}, {"at", at_text(at)}, {"phase", atl::strategy::market_phase_name(phase)}};
  const auto stamped = [](const auto& read, const char* key, json::object& out) {
    if (read) {
      out[key] = json::object{{"value", read->first}, {"read_at", at_text(read->second)}};
    } else {
      out[key] = nullptr;
    }
  };
  stamped(reads.holdings_, "holdings", event);
  stamped(reads.cash_, "cash", event);
  stamped(reads.orders_, "orders", event);
  return event;
}

/**
 * @brief Runs a program on a broker's account; see the file comment. Writes the events and the reply, and returns the
 *   exit.
 *
 * **This places orders** on the account, through the placement boundary.
 * @tparam Program The program.
 * @tparam Inner The trading session.
 * @tparam Market The market-data session (the trading session itself, or one that cannot trade).
 * @tparam MakeClock A callable taking the per-slice callback and returning the clock (`WallClock`).
 * @param run The run's fixed inputs.
 * @param config The operator's file.
 * @param strategy The program, built.
 * @param inner The trading session.
 * @param market The market-data session.
 * @param make_clock Builds the clock.
 * @return The exit.
 */
template <typename Program, typename Inner, typename Market, typename MakeClock>
int run_account(
  const AccountRun& run, const OperatorConfig& config, Program strategy, Inner& inner, Market& market,
  MakeClock make_clock
) {
  const auto& base = run.base_;
  // One clock for everything: the journal's lines, today's allowance, the boundary and the heartbeat.
  std::function<void()> on_slice;
  auto clock = make_clock([&on_slice]() {
    if (on_slice) {
      on_slice();
    }
  });
  const auto now = [&clock] {
    return clock.now();
  };
  // The account's lock, held for the run's life: the agent commands and another run answer busy.
  std::error_code ec;
  if (make_private_dirs(run.account_dir_) != 0 || !std::filesystem::is_directory(run.account_dir_, ec)) {
    return run_error(
      base, Exit::FAILED, "state.unavailable", "the account's directory cannot be made: " + run.account_dir_
    );
  }
  OwnerLock lock(run.account_dir_);
  const auto taken = lock.acquire(
    std::chrono::milliseconds(0),
    [] {
      return interrupted();
    },
    [](const std::string&) {}
  );
  if (taken == OwnerLock::Outcome::BUSY) {
    return run_error(
      base, Exit::FAILED, "busy.locked", "another command or run holds the account", "run it again when it is free"
    );
  }
  if (taken != OwnerLock::Outcome::HELD) {
    return run_error(base, Exit::FAILED, "file.unwritable", "the account's lock could not be taken: " + lock.error());
  }
  bool trading = false;
  try {
    if (!write_owner_record(lock, "run " + run.id_ + " pid " + std::to_string(static_cast<long long>(::getpid())))) {
      return relay(base, lock_lost_reply());
    }
    if (!std::filesystem::exists(config.armed_path(), ec)) {
      return run_error(base, Exit::REFUSED, "refused.not_armed", "the operator's armed file does not exist");
    }
    // The account's journal, kept across runs: what an earlier run left owed is this run's to account for first.
    const auto journal_path = run.account_dir_ + "/journal.jsonl";
    const auto scope = config.scope();
    {
      const auto replay = atl::runner::read_journal_file(journal_path);
      const bool fresh = !std::filesystem::exists(journal_path, ec);
      if (!fresh) {
        const auto view = journal_view(inner, replay, scope, paper::kst_day(now()));
        if (!view.owned_ || !view.intact_) {
          return run_error(
            base, Exit::UNKNOWN, "unknown.journal_damaged",
            view.detail_.empty() ? "the account's journal is not intact" : view.detail_, ACCOUNT_UNKNOWN_NEXT
          );
        }
        if (!view.owed_.empty()) {
          return run_error(
            base, Exit::UNKNOWN, "unknown.owed_at_start",
            "the account's journal waits on " + std::to_string(view.owed_.size()) + " order(s) an earlier run sent",
            "read the account's order list in the broker's app; reading it does not clear the journal, which "
            "cmdatl order reconcile will on a broker account in a later version"
          );
        }
      }
    }
    std::int64_t left = config.limits_.daily_orders_;
    if (std::filesystem::exists(journal_path, ec)) {
      const auto view = journal_view(inner, atl::runner::read_journal_file(journal_path), scope, paper::kst_day(now()));
      const auto allowance = orders_left(config, view);
      if (!allowance) {
        return run_error(
          base, Exit::UNKNOWN, "unknown.journal_damaged",
          view.detail_.empty() ? "the account's journal is not intact" : view.detail_, ACCOUNT_UNKNOWN_NEXT
        );
      }
      left = *allowance;
    }
    if (left <= 0) {
      return run_error(
        base, Exit::REFUSED, "refused.daily_order_limit",
        "the operator allows " + std::to_string(config.limits_.daily_orders_) +
          " order(s) a day, and the account's journal shows them placed today"
      );
    }
    // The reads before the start are read again after a rate or token refusal, each wait noted on stderr at once.
    // One token wait per session; on a real account the market-data session is the trading one.
    StartBudget trading_budget;
    StartBudget market_budget;
    auto& market_waits =
      static_cast<const void*>(&market) == static_cast<const void*>(&inner) ? trading_budget : market_budget;
    const auto start_note = [](const std::string& line) {
      (void)write_line(line + "\n", STDERR_FILENO);
    };
    // No order resting: the withdraw cancels every order on the account.
    // The holdings of the run's symbols are its position: every sellable share of them is the run's to sell.
    json::object holdings_at_start;
    {
      const auto read_orders = read_at_start(
        [&inner]() {
          return inner.working_orders();
        },
        clock, start_note, trading_budget
      );
      if (read_orders.refused_ == StartRead::INTERRUPTED) {
        return run_error(base, Exit::INTERRUPTED, "interrupted", "a signal arrived before the run started");
      }
      const auto& orders = read_orders.answer_;
      if (!orders) {
        return run_error(
          base, Exit::FAILED, "run.account_unreadable",
          "the account's working orders could not be read: " + orders.error().message_,
          start_read_next(read_orders.refused_, "")
        );
      }
      if (!orders.value().orders_.empty()) {
        return run_error(
          base, Exit::REFUSED, "refused.orders_resting",
          std::to_string(orders.value().orders_.size()) +
            " order(s) rest on the account; a run's withdraw cancels them all"
        );
      }
      auto read_held = read_at_start(
        [&inner]() {
          return inner.holdings();
        },
        clock, start_note, trading_budget
      );
      if (read_held.refused_ == StartRead::INTERRUPTED) {
        return run_error(base, Exit::INTERRUPTED, "interrupted", "a signal arrived before the run started");
      }
      auto& held = read_held.answer_;
      if (!held) {
        return run_error(
          base, Exit::FAILED, "run.account_unreadable",
          "the account's holdings could not be read: " + held.error().message_, start_read_next(read_held.refused_, "")
        );
      }
      auto net = net_holdings<typename Inner::BrokerTag>(std::move(held.value().positions_), run.ids_);
      if (!net) {
        return run_error(
          base, Exit::FAILED, "run.account_unreadable", "the account's holdings of a run symbol do not add up"
        );
      }
      holdings_at_start = std::move(*net);
    }
    // The trading calendar, read before anything starts: the loop decides the phase from it, and a first read failing
    // after the start would cost the first decision until its next read, hours later. The client keeps what it read.
    {
      const auto read_calendar = read_at_start(
        [&market]() {
          return market.calendar(atl::Country::KR, std::nullopt);
        },
        clock, start_note, market_waits
      );
      if (read_calendar.refused_ == StartRead::INTERRUPTED) {
        return run_error(base, Exit::INTERRUPTED, "interrupted", "a signal arrived before the run started");
      }
      const auto& calendar = read_calendar.answer_;
      if (!calendar) {
        return run_error(
          base, Exit::FAILED, "run.reference_unreadable",
          "the trading calendar could not be read: " + calendar.error().message_,
          start_read_next(read_calendar.refused_, "nothing was sent; start the run again")
        );
      }
      const auto today = atl::strategy::SessionWindow::krx().local_date(now());
      if (!calendar.value().is_trading_day(today)) {
        return run_error(
          base, Exit::FAILED, "run.reference_unreadable", "the trading calendar does not cover today",
          "nothing was sent; start the run again"
        );
      }
    }
    atl::runner::FileJournal journal(journal_path, scope);
    if (!journal.open()) {
      return run_error(
        base, Exit::FAILED, "file.unwritable", "the account's journal could not be opened: " + journal.rejection()
      );
    }
    EventLog events;
    atl::runner::StopSource stop;
    Guard guard;
    guard.armed_ = config.armed_path();
    guard.lock_ = &lock;
    guard.stop_ = &stop;
    guard.events_ = &events;
    guard.end_ = std::chrono::system_clock::time_point::max();
    std::size_t session_refused = 0;
    std::size_t signal_refused = 0;
    std::size_t placed = 0;
    PlacementPolicy policy;
    policy.armed_ = config.armed_path();
    policy.lock_ = &lock;
    policy.limit_only_ = config.limits_.order_type_ == "limit";
    policy.collar_ = config.limits_.collar_;
    for (const auto& name : config.limits_.phases_) {
      if (const auto phase = orders_detail::phase_named(name)) {
        policy.phases_.push_back(*phase);
      }
    }
    policy.window_ = atl::strategy::SessionWindow::krx();
    GuardedSession<Inner, Market> guarded(
      inner, market, policy, now, [&guard, &session_refused, &signal_refused, &placed](const PlacementEvent& event) {
        json::object line{
          {"event", event.kind_},
          {"at", at_text(event.at_)},
          {"symbol", event.order_.instrument_.symbol_},
          {"side", event.order_.side_ == atl::OrderSide::BUY ? "buy" : "sell"},
          {"quantity", event.order_.quantity_.str()},
        };
        if (event.order_.price_) {
          line["price"] = event.order_.price_->str();
        }
        if (event.kind_ == "placed") {
          line["order_id"] = event.order_id_;
          ++placed;
        } else {
          // Refused because the run is stopping: the stop's doing, not a refused order.
          ++(event.reason_ == "signal" || event.reason_ == "stopping" ? signal_refused : session_refused);
          line["reason"] = event.reason_;
        }
        guard.emit(std::move(line));
      }
    );
    guarded.set_stopping([&stop]() {
      return stop.intent().has_value();
    });
    // A stop signal is taken at the edge of the broker call it arrived during, not at the loop's next sleep: an
    // iteration's reads can take many seconds at a broker's pace, and the loop takes a stop latched during an iteration
    // straight after it. Before each call too, so no placement leaves after the signal.
    const auto take_signal = [&guard, &now]() {
      if (interrupted()) {
        guard.latch("signal", atl::runner::StopIntent::WITHDRAW, now());
      }
    };
    LastReads reads;
    RecordingSession<GuardedSession<Inner, Market>> session(guarded, run.symbols_, now, reads, take_signal);
    // The market-data reads (bars, quotes) go through their own session, which is asked for no account read.
    LastReads market_reads;
    RecordingSession<Market> market_session(market, {}, now, market_reads, take_signal);
    std::optional<std::chrono::steady_clock::time_point> heartbeat;
    on_slice = [&]() {
      const auto wall = clock.monotonic_now();
      if (trading && (!heartbeat || wall - *heartbeat >= ACCOUNT_HEARTBEAT)) {
        heartbeat = wall;
        guard.emit(account_heartbeat(now(), reads, guarded.phase_now()));
      }
      guard.check(now());
    };
    Wiring wiring;
    wiring.universe_ = run.ids_;
    wiring.limits_ = risk_limits(config, run.symbols_, left);
    wiring.schedule_.tick_ = std::chrono::seconds(10);
    wiring.schedule_.calendar_ = std::chrono::hours(6);
    wiring.schedule_.bars_ = std::chrono::seconds(60);
    wiring.schedule_.holdings_ = std::chrono::seconds(10);
    wiring.schedule_.working_orders_ = std::chrono::seconds(10);
    wiring.schedule_.buying_power_ = std::chrono::seconds(10);
    wiring.schedule_.data_ = std::chrono::seconds(60);
    // The newest rows a broker returns include today's unfinished bar, which the source withholds: one more than the
    // warm-up, so the first decision has every bar it needs.
    wiring.bar_count_ = static_cast<int>(std::max<std::size_t>(run.warmup_, 1) + 1);
    wiring.maximum_portfolio_age_ = std::chrono::seconds(config.limits_.portfolio_age_);
    wiring.window_ = atl::strategy::SessionWindow::krx();
    wiring.closing_wait_ = std::chrono::seconds(30);
    wiring.final_rounds_ = 20;
    wiring.final_step_ = std::chrono::seconds(3);
    wiring.start_note_ = start_note;
    wiring.start_budget_ = &market_waits;
    const auto on_ready = [&]() {
      guard.check(now());
      json::object start{
        {"event", "start"},
        {"v", EVENT_FORMAT},
        {"at", at_text(now())},
        {"env", base.at("env")},
        {"run", run.id_},
        {"account", config.account_name()},
        {"market_data", run.market_data_},
        {"cap", wiring.limits_.max_position_quantity_ ? wiring.limits_.max_position_quantity_->str() : "0"},
        {"holdings_at_start", holdings_at_start},
        {"orders_left_today", left},
        {"warmup_bars", run.warmup_},
        {"pid", static_cast<std::int64_t>(::getpid())},
      };
      json::array listed;
      for (const auto& symbol : run.symbols_) {
        listed.push_back(json::value(symbol));
      }
      start["symbols"] = listed;
      if (!events.write(start)) {
        return false;
      }
      trading = true;
      guard.release_held();
      return true;
    };
    std::ostringstream diagnostics;
    Traded traded;
    std::string thrown;
    try {
      traded = trade_on_rails(
        session, market_session, journal, std::move(strategy), wiring, stop, clock, on_ready,
        [&inner]() {
          const auto orders = inner.working_orders();
          return orders && orders.value().orders_.empty();
        },
        [] {}, diagnostics
      );
    } catch (const std::exception& error) {
      thrown = run.opaque_errors_ ? OPAQUE_EXCEPTION : error.what();
    }
    (void)write_line(diagnostics.str(), STDERR_FILENO);
    if (!thrown.empty() && !trading) {
      return run_error(base, Exit::FAILED, "internal.error", thrown);
    }
    switch (traded.outcome_) {
      case Outcome::ON_FILL_UNSUPPORTED:
        if (thrown.empty()) {
          return run_error(
            base, Exit::PROGRAM, "program.live_unsupported", traded.detail_, "a run takes programs without on_fill",
            {{"unsupported", "on_fill"}}
          );
        }
        break;
      case Outcome::RECOVERY_HALTED:
        if (thrown.empty()) {
          return run_error(base, Exit::UNKNOWN, "unknown.recovery_halted", traded.detail_, ACCOUNT_UNKNOWN_NEXT);
        }
        break;
      case Outcome::REFERENCE_UNREADABLE:
        if (thrown.empty()) {
          return run_error(
            base, Exit::FAILED, "run.reference_unreadable", traded.detail_,
            start_read_next(traded.start_read_, "nothing was sent; start the run again")
          );
        }
        break;
      case Outcome::INTERRUPTED:
        if (thrown.empty()) {
          return run_error(base, Exit::INTERRUPTED, "interrupted", traded.detail_);
        }
        break;
      case Outcome::NOT_STARTED:
        if (thrown.empty()) {
          return run_error(
            base, Exit::FAILED, "file.unwritable", "the run's start line could not be written; nothing was sent"
          );
        }
        break;
      default:
        break;
    }
    json::object body = base;
    Exit exit = Exit::OK;
    std::string code;
    std::string message = traded.detail_;
    if (!thrown.empty()) {
      exit = Exit::UNKNOWN;
      code = "unknown.internal";
      message = thrown;
    } else if (traded.outcome_ != Outcome::CLEAN) {
      exit = Exit::UNKNOWN;
      code = std::string("unknown.") + outcome_name(traded.outcome_);
    }
    body["outcome"] = thrown.empty() ? outcome_name(traded.outcome_) : "stopped_by_error";
    const std::size_t guard_refused = traded.report_ ? traded.report_->orders_guard_refused_ : 0;
    body["refused"] = json::object{
      {"boundary", session_refused},
      {"guard", guard_refused},
      {"pricing", traded.pricing_refused_},
      {"signal", signal_refused},
    };
    if (
      thrown.empty() && traded.outcome_ == Outcome::CLEAN &&
      session_refused + guard_refused + traded.pricing_refused_ > 0
    ) {
      body["outcome"] = "refused_orders";
    }
    body["placed"] = placed;
    const auto view = journal_view(inner, atl::runner::read_journal_file(journal_path), scope, paper::kst_day(now()));
    if (exit == Exit::OK && (guard.lock_lost_ || !lock.still_at_path())) {
      exit = Exit::UNKNOWN;
      code = "unknown.lock_lost";
      message = "the account's lock file was replaced during the run";
    }
    if (exit == Exit::OK && (!view.owned_ || !view.intact_)) {
      exit = Exit::UNKNOWN;
      code = "unknown.journal_damaged";
      message = view.detail_.empty() ? "the account's journal is not intact after the run" : view.detail_;
    }
    if (exit == Exit::OK && !view.owed_.empty()) {
      exit = Exit::UNKNOWN;
      code = "unknown.owed_at_end";
      message = "the account's journal still waits on an order after the run";
    }
    body["stopped"] = guard.stopped_.empty() ? json::value(nullptr) : json::value(guard.stopped_);
    body["ended_at"] = at_text(now());
    // The broker's own figures for the run's symbols, read once more.
    auto holdings = holdings_at_end(inner, run.symbols_, clock, start_note);
    body["holdings_at_end"] = std::move(holdings.held_);
    body["holdings_at_end_error"] = std::move(holdings.error_);
    json::array owed;
    for (const auto& o : view.owed_) {
      owed.push_back(
        json::object{
          {"symbol", o.symbol_}, {"side", o.side_}, {"quantity", o.quantity_}, {"journal_id", o.journal_id_},
          {"order_id", o.order_id_}, {"state", o.state_}
        }
      );
    }
    body["owed_at_end"] = owed;
    if (exit != Exit::OK) {
      body["error"] = json::object{{"code", code}, {"message", message}, {"next", ACCOUNT_UNKNOWN_NEXT}};
    }
    body["result"] = exit == Exit::OK ? "ended" : "unknown";
    (void)events.write(
      json::object{
        {"event", "end"}, {"at", at_text(now())}, {"exit", static_cast<int>(exit)}, {"result", body["result"]}
      }
    );
    return write_run_reply(body, exit);
  } catch (const std::exception& error) {
    const char* text = run.opaque_errors_ ? OPAQUE_EXCEPTION : error.what();
    if (trading) {
      return run_error(base, Exit::UNKNOWN, "unknown.internal", text, ACCOUNT_UNKNOWN_NEXT);
    }
    return run_error(base, Exit::FAILED, "internal.error", text);
  }
}

} // namespace driver
} // namespace cmdatl
