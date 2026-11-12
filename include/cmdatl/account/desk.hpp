#pragma once

// The paper market between processes: rebuilt in every process from the operator's file, the bars, and the account's
// write-ahead log of its inputs, `paper.jsonl`.
//
// The market is a pure function of its bars, its config and its inputs (clock advances, placements and
// cancellations), so the log is its whole state.
// Each input is appended and `fsync`ed before it is applied; a torn last line is an input that never reached the
// market and is cut off, after the complete lines before it were checked.
// After each placement or cancellation a `result` line records what the market answered, and replay refuses a market
// that answers differently.
//
// Each line is exactly what the writer prints: replay rebuilds every line from its checked, typed fields through the
// same writer and compares the bytes, so an extra, missing, reordered or repeated member, another number spelling or
// whitespace is refused.
// `format` versions the line grammar, `market` the matching, costs, acceptance hours and rounding; either changes, the
// version rises.

#include "../paper/broker.hpp"
#include "../paper/market.hpp"
#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "./operator.hpp"

#include <algotradelib/broker>

#include <boost/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {

class Account;

namespace paper {

namespace json = boost::json;

/** @brief The paper log's line grammar version. */
inline constexpr std::int64_t LOG_FORMAT = 1;
/** @brief The paper market's semantics version: matching, costs, acceptance hours and rounding. */
inline constexpr std::int64_t MARKET_VERSION = 1;
/** @brief The largest paper log read, in bytes. */
inline constexpr std::size_t MAX_LOG_BYTES = 64u << 20;

/** @brief The paper market's state cannot be rebuilt: exit 4 `unknown.paper_state`. */
class PaperStateError : public std::runtime_error {
public:
  /**
   * @brief Builds the error.
   * @param what What is wrong.
   */
  explicit PaperStateError(const std::string& what) : std::runtime_error(what) {}
};

/** @brief The account's lock was replaced after this process took it: exit 4 `unknown.lock_lost`. */
class LockLost : public std::runtime_error {
public:
  /** @brief Builds the error. */
  LockLost()
  : std::runtime_error("the account's lock file was replaced while this process held it; nothing was written") {}
};

/**
 * @brief The fingerprint of a paper market: FNV-1a 64 over the bars file's bytes, then the cash, the start and the
 *   two lags, as sixteen hex digits.
 * @param data The bars, whose hash is of the file's bytes.
 * @param settings The paper settings.
 * @return The fingerprint.
 */
inline std::string fingerprint_of(const MarketData& data, const PaperSettings& settings) {
  const auto text = "|" + std::to_string(settings.cash_) + "|" + settings.start_ + "|" +
    std::to_string(settings.listing_lag_) + "|" + std::to_string(settings.holdings_lag_);
  char out[20];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(fnv1a(text, data.hash_)));
  return out;
}

/** @brief A placement's fields as the log writes them. */
struct PlaceFields {
  /** @brief The symbol. */
  std::string symbol_;
  /** @brief `kr`. */
  std::string country_;
  /** @brief The instrument's venue: `combined`. */
  std::string route_;
  /** @brief `buy` or `sell`. */
  std::string side_;
  /** @brief `limit` or `market`. */
  std::string type_;
  /** @brief `day`. */
  std::string tif_;
  /** @brief `regular`. */
  std::string session_;
  /** @brief The routing venue: `unspecified`. */
  std::string venue_;
  /** @brief Whole shares. */
  std::int64_t quantity_{0};
  /** @brief The limit as written, for a limit order. */
  std::optional<std::string> price_;
};

/**
 * @brief The `genesis` line, first and only first.
 * @param fingerprint The market's fingerprint.
 * @param start The first instant, nanoseconds.
 * @return The line, without its newline.
 */
inline std::string genesis_line(const std::string& fingerprint, std::int64_t start) {
  json::object o;
  o["op"] = "genesis";
  o["format"] = LOG_FORMAT;
  o["market"] = MARKET_VERSION;
  o["fingerprint"] = fingerprint;
  o["start"] = start;
  return json::serialize(o);
}

/**
 * @brief An `advance` line.
 * @param to The new instant, nanoseconds.
 * @return The line, without its newline.
 */
inline std::string advance_line(std::int64_t to) {
  json::object o;
  o["op"] = "advance";
  o["to"] = to;
  return json::serialize(o);
}

/**
 * @brief A `place` line: the fields in a fixed order, `price` only for a limit.
 * @param at The clock, nanoseconds.
 * @param fields The placement.
 * @return The line, without its newline.
 */
inline std::string place_line(std::int64_t at, const PlaceFields& fields) {
  json::object o;
  o["op"] = "place";
  o["at"] = at;
  o["symbol"] = fields.symbol_;
  o["country"] = fields.country_;
  o["route"] = fields.route_;
  o["side"] = fields.side_;
  o["type"] = fields.type_;
  o["tif"] = fields.tif_;
  o["session"] = fields.session_;
  o["venue"] = fields.venue_;
  o["quantity"] = fields.quantity_;
  if (fields.type_ == "limit" && fields.price_) {
    o["price"] = *fields.price_;
  }
  return json::serialize(o);
}

/**
 * @brief A `cancel` line.
 * @param at The clock, nanoseconds.
 * @param id The order's id.
 * @return The line, without its newline.
 */
inline std::string cancel_line(std::int64_t at, const std::string& id) {
  json::object o;
  o["op"] = "cancel";
  o["at"] = at;
  o["id"] = id;
  return json::serialize(o);
}

/**
 * @brief A `result` line, right after its `place` or `cancel`.
 * @param of `place` or `cancel`.
 * @param id The order's id (empty for a refused placement).
 * @param error The market's refusal, or empty.
 * @return The line, without its newline.
 */
inline std::string result_line(const std::string& of, const std::string& id, const std::string& error) {
  json::object o;
  o["op"] = "result";
  o["of"] = of;
  o["id"] = id;
  o["error"] = error;
  return json::serialize(o);
}

/**
 * @brief Whether a text is an order id of the market's form: `[1-9][0-9]{0,17}`.
 * @param id The text.
 * @return Whether it is.
 */
inline bool valid_order_id(const std::string& id) {
  return !id.empty() && id.size() <= 18 && id.front() != '0' && std::all_of(id.begin(), id.end(), [](char c) {
    return c >= '0' && c <= '9';
  });
}

/** @brief A placement the shared rules accepted, in the market's terms. */
struct CheckedPlace {
  /** @brief Whole shares. */
  std::int64_t quantity_{0};
  /** @brief The limit, for a limit order. */
  std::optional<double> limit_;
};

/**
 * @brief The one set of input rules, for the live path before anything is logged and for replay alike: a symbol in
 *   the bars, the paper market's country, route, time in force, session and venue, a buy or a sell, a limit or a
 *   market order, a whole quantity from 1 to 10^8, a price by the bars' price grammar on a limit and none on a market
 *   order.
 * @param market The market (its symbols).
 * @param fields The placement.
 * @param checked Where the accepted values go.
 * @return Why it is refused, or nothing.
 */
inline std::optional<std::string> check_place(
  const SimMarket& market, const PlaceFields& fields, CheckedPlace& checked
) {
  if (!market.has_symbol(fields.symbol_)) {
    return "the paper market has no bars for " + fields.symbol_;
  }
  if (
    fields.country_ != "kr" || fields.route_ != "combined" || fields.session_ != "regular" ||
    fields.venue_ != "unspecified"
  ) {
    return "the paper market takes KRX orders in the regular session, routed by the broker";
  }
  if (fields.tif_ != "day") {
    return "the paper market takes DAY orders only";
  }
  if (fields.side_ != "buy" && fields.side_ != "sell") {
    return "an order is a buy or a sell";
  }
  if (fields.type_ != "limit" && fields.type_ != "market") {
    return "the paper market takes limit and market orders only";
  }
  if (fields.quantity_ < 1 || fields.quantity_ > MAX_SHARES) {
    return "a quantity is a whole number of shares from 1 to " + std::to_string(MAX_SHARES);
  }
  checked.quantity_ = fields.quantity_;
  checked.limit_.reset();
  if (fields.type_ == "limit") {
    if (!fields.price_) {
      return "a limit order needs a price";
    }
    const auto price = parse_price(*fields.price_);
    if (!price) {
      return "a price is from 1 to 10000000 with at most four decimals";
    }
    checked.limit_ = price;
  } else if (fields.price_) {
    return "a market order has no price";
  }
  return std::nullopt;
}

/**
 * @brief A request's fields in the log's words; a value the paper market never takes becomes a word the rules refuse.
 * @param request The request.
 * @param fields Where the fields go.
 * @return Why the request cannot be written at all (a fractional quantity, a trigger price), or nothing.
 */
inline std::optional<std::string> fields_of(const atl::OrderRequest& request, PlaceFields& fields) {
  if (request.trigger_price_) {
    return "the paper market takes no trigger price";
  }
  fields.symbol_ = request.instrument_.symbol_;
  fields.country_ = request.instrument_.country_ == atl::Country::KR ? "kr" : "other";
  fields.route_ = request.instrument_.venue_ == atl::Venue::COMBINED ? "combined" : "other";
  fields.side_ =
    request.side_ == atl::OrderSide::BUY ? "buy" : (request.side_ == atl::OrderSide::SELL ? "sell" : "other");
  fields.type_ =
    request.type_ == atl::OrderType::LIMIT ? "limit" : (request.type_ == atl::OrderType::MARKET ? "market" : "other");
  fields.tif_ = request.time_in_force_ == atl::TimeInForce::DAY ? "day" : "other";
  fields.session_ = request.session_ == atl::OrderSession::REGULAR ? "regular" : "other";
  fields.venue_ = request.venue_ == atl::Venue::UNSPECIFIED ? "unspecified" : "other";
  const auto coefficient = request.quantity_.coefficient();
  std::int64_t whole = coefficient;
  for (unsigned k = 0; k < request.quantity_.scale(); ++k) {
    if (whole % 10 != 0) {
      return "a quantity is a whole number of shares";
    }
    whole /= 10;
  }
  fields.quantity_ = whole;
  if (request.price_) {
    fields.price_ = request.price_->str();
  } else {
    fields.price_.reset();
  }
  return std::nullopt;
}

/**
 * @brief The request a placement's fields stand for (after `check_place` accepted them).
 * @param fields The fields.
 * @return The request.
 */
inline atl::OrderRequest request_of(const PlaceFields& fields) {
  atl::OrderRequest request;
  request.instrument_ = kr(fields.symbol_);
  request.side_ = fields.side_ == "buy" ? atl::OrderSide::BUY : atl::OrderSide::SELL;
  request.type_ = fields.type_ == "limit" ? atl::OrderType::LIMIT : atl::OrderType::MARKET;
  request.time_in_force_ = atl::TimeInForce::DAY;
  request.session_ = atl::OrderSession::REGULAR;
  request.venue_ = atl::Venue::UNSPECIFIED;
  request.quantity_ = atl::Decimal64(fields.quantity_, 0);
  if (fields.price_) {
    request.price_ = atl::Decimal64::parse(*fields.price_);
  }
  return request;
}

/** @brief How an append to the paper log ended. */
enum class Appended {
  OK,
  LOCK_LOST,
  NOT_WRITTEN,
  NOT_SYNCED,
};

#if !defined(_WIN32)
/**
 * @brief The paper log: one descriptor, opened without following a link or blocking, through which it is read once
 *   and appended to; every append written whole and `fsync`ed, after a check that the lock is still this process's.
 */
class OpsLog {
public:
  /**
   * @brief Opens the log, creating it only when asked (exclusively, so an existing file is never taken for new).
   * @param path The log.
   * @param lock The held lock's identity.
   * @param create Whether to create it.
   */
  OpsLog(std::string path, LockIdentity lock, bool create) : path_(std::move(path)), lock_(std::move(lock)) {
    const int flags = O_RDWR | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0);
    fd_.reset(open_lifted(path_.c_str(), flags, 0600));
    open_error_ = fd_.get() < 0 ? errno : 0;
  }

  OpsLog(const OpsLog&) = delete;
  OpsLog& operator=(const OpsLog&) = delete;

  /**
   * @brief Why it did not open.
   * @return The error number, or 0.
   */
  int open_error() const noexcept {
    return open_error_;
  }

  /**
   * @brief The log's path.
   * @return The path.
   */
  const std::string& path() const noexcept {
    return path_;
  }

  /**
   * @brief Whether the log is a regular file with one name.
   * @param why Set to why not.
   * @return Whether it is.
   */
  bool check(std::string& why) const {
    int error = 0;
    if (single_regular(fd_.get(), error)) {
      return true;
    }
    why = error != 0 ?
      "its status cannot be read: " + std::string(std::strerror(error)) :
      "it is not a regular file with one name";
    return false;
  }

  /**
   * @brief Reads the whole log through its descriptor.
   * @param out Where the bytes go.
   * @return How it ended.
   */
  FileRead read(std::string& out) {
    if (::lseek(fd_.get(), 0, SEEK_SET) != 0) {
      FileRead failed;
      failed.status_ = FileRead::Status::FAILED;
      failed.error_ = errno;
      return failed;
    }
    return read_whole(fd_.get(), MAX_LOG_BYTES, out);
  }

  /**
   * @brief Cuts a torn last line off: truncated on the descriptor, flushed, and its size checked.
   * @param size The size of the complete lines.
   * @throws LockLost When the lock was replaced.
   * @throws PaperStateError When any step fails; nothing more is appended then.
   */
  void truncate_to(std::size_t size) {
    if (held_) {
      throw PaperStateError("the paper log is held: the account's journal was not established");
    }
    if (!lock_.still_at_path()) {
      poisoned_ = true;
      throw LockLost();
    }
    struct stat info{};
    if (
      sys::ftruncate(fd_.get(), static_cast<off_t>(size)) != 0 || sys::fsync(fd_.get()) != 0 ||
      sys::fstat(fd_.get(), &info) != 0 || info.st_size != static_cast<off_t>(size)
    ) {
      const int error = errno;
      poisoned_ = true;
      throw PaperStateError(
        "the paper log's torn last line could not be cut off: " + std::string(std::strerror(error))
      );
    }
  }

  /**
   * @brief Appends one line and flushes it to disk, after checking the lock; after any failure nothing more is
   *   appended by this log.
   * @param line The line, without its newline.
   * @param point The kill points' prefix (`advance`), named `<point>.written` and `<point>.synced`.
   * @param error Set to the error number of a failed write or flush.
   * @return How it ended.
   */
  Appended append(const std::string& line, const char* point, int& error) {
    error = 0;
    if (poisoned_ || held_) {
      error = EIO;
      return Appended::NOT_WRITTEN;
    }
    if (!lock_.still_at_path()) {
      poisoned_ = true;
      return Appended::LOCK_LOST;
    }
    const auto text = line + "\n";
    std::size_t written = 0;
    error = write_whole(fd_.get(), text.data(), text.size(), written);
    if (error != 0) {
      poisoned_ = true;
      return Appended::NOT_WRITTEN;
    }
    sys::kill_point((std::string(point) + ".written").c_str());
    if (sys::fsync(fd_.get()) != 0) {
      error = errno;
      poisoned_ = true;
      return Appended::NOT_SYNCED;
    }
    sys::kill_point((std::string(point) + ".synced").c_str());
    return Appended::OK;
  }

  /**
   * @brief Appends a line where any failure leaves the market's state unknown.
   * @param line The line.
   * @param point The kill points' prefix.
   * @throws LockLost When the lock was replaced.
   * @throws PaperStateError When the line may not be whole on disk.
   */
  void append_or_throw(const std::string& line, const char* point) {
    int error = 0;
    switch (append(line, point, error)) {
      case Appended::OK:
        return;
      case Appended::LOCK_LOST:
        throw LockLost();
      case Appended::NOT_WRITTEN:
      case Appended::NOT_SYNCED:
        break;
    }
    throw PaperStateError("the paper log could not be written: " + std::string(std::strerror(error)));
  }

  /**
   * @brief Holds the log, or lets it go: while held, nothing is cut or appended.
   * @param held Whether it is held.
   */
  void hold(bool held) noexcept {
    held_ = held;
  }

private:
  std::string path_;
  LockIdentity lock_;
  ScopedFd fd_;
  int open_error_{0};
  bool poisoned_{false};
  bool held_{false};
};

/**
 * @brief The paper client with every order-writing call checked by the shared rules and logged ahead of the market.
 *
 * Only `place` and `cancel` change the market, so only they are logged; reads go to `PaperClient` unchanged.
 * A placement is refused, not reached and not logged, while the armed file is missing; the armed file is always the
 * operator's `<stem>.armed`, so the check cannot be switched off.
 * An input line not written whole is refused, not reached; a whole line whose flush failed is an unknown outcome
 * (a transport error that reached the broker); a `result` line that cannot be written leaves the market's answer
 * standing.
 * After any failed append the log is latched, so every later input of this process is refused, not reached.
 * The log is shared, so no copy of a session that holds this client can outlive it.
 */
class LoggedClient : public PaperClient {
public:
  /**
   * @brief Builds the client.
   * @param market The market.
   * @param log The log.
   * @param armed The armed file.
   */
  LoggedClient(std::shared_ptr<SimMarket> market, std::shared_ptr<OpsLog> log, std::string armed)
  : PaperClient(std::move(market)), log_(std::move(log)), armed_(std::move(armed)) {}

  /**
   * @brief Places an order: the armed file, the shared rules, the `place` line, the market, the `result` line.
   * @param account The account.
   * @param request The request.
   * @return The acknowledgement, or a refusal (not reached when refused before the line was written whole).
   */
  atl::Result<PaperAck> place(const PaperAccount& account, const atl::OrderRequest& request) {
    sys::kill_point("send.before_input");
    std::error_code ec;
    if (armed_.empty() || !std::filesystem::exists(armed_, ec)) {
      return refusal("NOT_ARMED", "the operator's armed file does not exist: " + armed_, false);
    }
    PlaceFields fields;
    CheckedPlace checked;
    if (auto problem = fields_of(request, fields)) {
      return refusal("invalid_order", *problem, false);
    }
    if (auto problem = check_place(market(), fields, checked)) {
      return refusal("invalid_order", *problem, false);
    }
    if (auto failed = log_input(place_line(nanos(market().now()), fields), "place")) {
      return *failed;
    }
    sys::kill_point("send.after_input");
    auto result = place_checked(account, request, checked.quantity_, checked.limit_);
    log_result(result_line(
      "place", result ? result.value().id_ : std::string(), result ? std::string() : result.error().message_
    ));
    sys::kill_point("send.after_result");
    return result;
  }

  /**
   * @brief Cancels an order: an id of the market's form, the `cancel` line, the market, the `result` line.
   * @param account The account.
   * @param amendment The order.
   * @return The acknowledgement, or a refusal (not reached when refused before the line was written whole).
   */
  atl::Result<PaperAck> cancel(const PaperAccount& account, const PaperAmendment& amendment) {
    sys::kill_point("send.before_input");
    if (!valid_order_id(amendment.id_)) {
      return refusal("invalid_order", "no order of the paper market has the id " + amendment.id_, false);
    }
    if (auto failed = log_input(cancel_line(nanos(market().now()), amendment.id_), "cancel")) {
      return *failed;
    }
    sys::kill_point("send.after_input");
    auto result = PaperClient::cancel(account, amendment);
    log_result(result_line("cancel", amendment.id_, result ? std::string() : result.error().message_));
    sys::kill_point("send.after_result");
    return result;
  }

  /**
   * @brief How many `place` and `cancel` lines this process wrote whole, counted when the last byte was written.
   * @return The count.
   */
  std::uint64_t inputs_logged() const noexcept {
    return inputs_logged_;
  }

private:
  /**
   * @brief Appends an input line.
   * @param line The line.
   * @param point The kill points' prefix.
   * @return The refusal to answer with, or nothing when the line is whole and flushed.
   */
  std::optional<atl::BrokerError> log_input(const std::string& line, const char* point) {
    int error = 0;
    switch (log_->append(line, point, error)) {
      case Appended::OK:
        ++inputs_logged_;
        return std::nullopt;
      case Appended::LOCK_LOST:
        return refusal("lock_lost", "the account's lock file was replaced; nothing was written", false);
      case Appended::NOT_WRITTEN:
        return refusal(
          "paper_log",
          "the paper log could not be written (" + std::string(std::strerror(error)) + "); nothing reached the market",
          false
        );
      case Appended::NOT_SYNCED:
        break;
    }
    ++inputs_logged_;
    atl::BrokerError unknown;
    unknown.category_ = atl::ErrorCategory::TRANSPORT;
    unknown.reached_broker_ = true;
    unknown.broker_code_ = "paper_log_unsynced";
    unknown.message_ = "the input was written to the paper log but not flushed (" + std::string(std::strerror(error)) +
      "); the market may have it on the next replay";
    return unknown;
  }

  /**
   * @brief Appends a `result` line; a failure leaves the market's answer standing (the next open writes it again).
   * @param line The line.
   */
  void log_result(const std::string& line) {
    int error = 0;
    (void)log_->append(line, "result", error);
  }

  std::shared_ptr<OpsLog> log_;
  std::string armed_;
  std::uint64_t inputs_logged_{0};
};
#endif

} // namespace paper
} // namespace cmdatl

#if !defined(_WIN32)
namespace algotradelib {

/** @brief The paper broker's traits, with the two order-writing verbs going through the log. */
template <>
struct BrokerTraits<cmdatl::paper::LoggedClient> : BrokerTraits<cmdatl::paper::PaperClient> {
  /** @brief The client. */
  using Client = cmdatl::paper::LoggedClient;

  /**
   * @brief A placement, logged.
   * @param c The client.
   * @param a The account.
   * @param o The order.
   * @return The answer.
   */
  static Result<PlaceAck> place_order(Client& c, const Account& a, OrderRequest o) {
    return c.place(a, o);
  }
  /**
   * @brief A cancellation, logged.
   * @param c The client.
   * @param a The account.
   * @param m The cancellation.
   * @return The answer.
   */
  static Result<PlaceAck> cancel_order(Client& c, const Account& a, Amendment m) {
    return c.cancel(a, m);
  }
  /**
   * @brief No rate limit.
   * @param client The client.
   * @return Null.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    (void)client;
    return nullptr;
  }
};

} // namespace algotradelib

namespace cmdatl {
namespace paper {

struct DeskTest;

/**
 * @brief The key only `Account` can make: a desk is built only by an account holding its lock.
 *
 * `DeskTest` is defined by the tests alone, to hand a desk a lock it must refuse.
 */
class DeskKey {
  friend class cmdatl::Account;
  friend struct DeskTest;
  DeskKey() = default;
};

/** @brief How a clock advance ended. */
struct AdvanceOutcome {
  /** @brief What happened. */
  Appended status_{Appended::OK};
  /** @brief The error number of a failed write or flush. */
  int error_{0};
};

/**
 * @brief The paper market, account and clock of one account, as its log leaves them: the one desk (the interface the
 *   commands use, written out in `account.hpp`).
 */
class PaperDesk {
public:
  /** @brief The environment this session is: every reply's `env`. */
  static constexpr const char* env_name = "paper";
  /** @brief The broker this session is. */
  static constexpr const char* broker_name = "paper";
  /** @brief A simulated broker: it has its own cash, a clock moved by command and an end to its data. */
  static constexpr bool simulated = true;

  /** @brief The trading session: every read, placement and cancellation. */
  using Session = atl::BlockingSession<LoggedClient>;
  /** @brief The session market data is read from: the trading session itself. */
  using MarketSession = Session;

  /**
   * @brief Builds the market from the bars and replays the log, writing nothing: the log stays held until `repair`.
   *
   * Only an account holding this account's lock can.
   * @param key The account's key.
   * @param config The operator's file.
   * @param lock The held lock; its directory must be this config's account directory.
   * @param data The bars, read and checked.
   * @throws std::logic_error When the lock is not held or is another account's (a defect).
   * @throws PaperStateError When the log cannot be read or replayed; nothing was written then.
   */
  PaperDesk(DeskKey key, const OperatorConfig& config, const OwnerLock& lock, MarketData data) : config_(config) {
    (void)key;
    const auto dir = account_dir(config);
    if (!lock.held() || !dir || lock.dir() != *dir) {
      throw std::logic_error("a paper desk needs its own account's lock, held");
    }
    fingerprint_ = fingerprint_of(data, config.paper_);
    log_ = std::make_shared<OpsLog>(lock.dir() + "/paper.jsonl", lock.identity(), false);
    if (log_->open_error() != 0) {
      throw PaperStateError("the paper log cannot be opened: " + std::string(std::strerror(log_->open_error())));
    }
    std::string why;
    if (!log_->check(why)) {
      throw PaperStateError("the paper log is refused: " + why);
    }
    market_ =
      std::make_shared<SimMarket>(std::move(data), static_cast<double>(config.paper_.cash_), config.paper_.start_at_);
    client_ = std::make_shared<LoggedClient>(market_, log_, config.armed_path());
    client_->listing_lag_ = std::chrono::seconds(config.paper_.listing_lag_);
    client_->holdings_lag_ = std::chrono::seconds(config.paper_.holdings_lag_);
    session_ = std::make_unique<Session>(client_, PaperAccount{config.number_});
    log_->hold(true);
    replay();
    market_->drain_events();
  }

  /**
   * @brief Repairs the log once the account's journal is established: a torn tail cut, a missing result answered;
   *   then the log takes appends.
   * @throws LockLost When the lock was replaced.
   * @throws PaperStateError When the repair failed; nothing more is appended then.
   */
  void repair() {
    log_->hold(false);
    if (cut_to_) {
      // A kill during a write: that input never reached the market.
      log_->truncate_to(*cut_to_);
      cut_to_.reset();
    }
    if (owed_result_) {
      // A crash between an input and its answer: the answer, as the market gives it now, before anything else.
      log_->append_or_throw(*owed_result_, "result");
      owed_result_.reset();
    }
  }

  PaperDesk(const PaperDesk&) = delete;
  PaperDesk& operator=(const PaperDesk&) = delete;
  PaperDesk(PaperDesk&&) = delete;
  PaperDesk& operator=(PaperDesk&&) = delete;

  /**
   * @brief The session every read uses, borrowed.
   * @return The session.
   */
  Session& session() {
    return *session_;
  }

  /**
   * @brief The session quotes, price bands and the calendar are read from, borrowed: the trading session.
   * @return The session.
   */
  MarketSession& market_session() {
    return *session_;
  }

  /**
   * @brief The market's clock.
   * @return The instant.
   */
  Clock::time_point now() const {
    return market_->now();
  }

  /**
   * @brief The cash after every fill's value, commission and tax, floored.
   * @return The cash.
   * @throws OutOfRange When it left the supported range.
   */
  std::int64_t paper_cash() const {
    return amount_of(std::floor(market_->cash()));
  }

  /**
   * @brief What the market shows for an order: `pending`, `filled`, `canceled`.
   * @param id The order's id.
   * @return The status, or nothing for an order the market never named.
   */
  std::optional<std::string> order_status(const std::string& id) const {
    for (const auto& order : market_->orders()) {
      if (order.id_ != id) {
        continue;
      }
      switch (order.status_) {
        case atl::OrderStatus::PENDING:
        case atl::OrderStatus::PARTIAL_FILLED:
          return std::string("pending");
        case atl::OrderStatus::FILLED:
          return std::string("filled");
        default:
          return std::string("canceled");
      }
    }
    return std::nullopt;
  }

  /**
   * @brief The last date any symbol trades.
   * @return The date, as days from 1970-01-01.
   */
  std::int64_t data_end() const {
    return market_->last_day();
  }

  /**
   * @brief The latest instant the clock may reach: the last session's close plus a day, inside the supported interval.
   * @return The instant.
   */
  Clock::time_point clock_limit() const {
    const auto limit = open_of(market_->last_day()) + SESSION_LENGTH + std::chrono::hours(24);
    return std::min(limit, end_instant() - Clock::duration(std::chrono::minutes(1)));
  }

  /**
   * @brief The open of the next stored session after an instant.
   * @param at The instant.
   * @return The open, or nothing.
   */
  std::optional<Clock::time_point> next_session_after(Clock::time_point at) const {
    return market_->next_open_after(at);
  }

  /**
   * @brief Takes what the market did since the last call.
   * @return The events.
   */
  std::vector<MarketEvent> drain_events() {
    return market_->drain_events();
  }

  /**
   * @brief Whether the market takes an order for a symbol at an instant.
   * @param symbol The symbol.
   * @param at The instant.
   * @return Whether it does.
   */
  bool accepts_orders_at(const std::string& symbol, Clock::time_point at) const {
    return market_->accepts_orders_at(symbol, at);
  }

  /**
   * @brief Whether the bars have a symbol.
   * @param symbol The symbol.
   * @return Whether they do.
   */
  bool has_symbol(const std::string& symbol) const {
    return market_->has_symbol(symbol);
  }

  /**
   * @brief Moves the clock: the `advance` line logged and flushed, then applied.
   * @param target The new instant, after now and at most `clock_limit()`.
   * @return How the append ended; the market moved only when `OK`.
   */
  AdvanceOutcome advance_to(Clock::time_point target) {
    AdvanceOutcome outcome;
    outcome.status_ = log_->append(advance_line(nanos(target)), "advance", outcome.error_);
    if (outcome.status_ != Appended::OK) {
      return outcome;
    }
    market_->advance_to(target);
    sys::kill_point("advance.applied");
    return outcome;
  }

  /**
   * @brief How many placements and cancellations this process sent: the `place` and `cancel` lines it wrote whole
   *   (not those replayed at open).
   * @return The count.
   */
  std::uint64_t sends() const noexcept {
    return client_->inputs_logged();
  }

private:
  /**
   * @brief A replay failure.
   * @param line The line, from 1.
   * @param what What is wrong.
   * @return The error.
   */
  static PaperStateError broken(std::size_t line, const std::string& what) {
    return PaperStateError("the paper log's line " + std::to_string(line) + " " + what);
  }

  /**
   * @brief A string member of a parsed line.
   * @param o The line.
   * @param key The member.
   * @return The string, or nothing.
   */
  static std::optional<std::string> text_at(const json::object& o, const char* key) {
    const auto* value = o.if_contains(key);
    if (value == nullptr || !value->is_string()) {
      return std::nullopt;
    }
    return std::string(value->as_string());
  }

  /**
   * @brief An integer member of a parsed line.
   * @param o The line.
   * @param key The member.
   * @return The integer, or nothing.
   */
  static std::optional<std::int64_t> integer_at(const json::object& o, const char* key) {
    const auto* value = o.if_contains(key);
    if (value == nullptr || !value->is_int64()) {
      return std::nullopt;
    }
    return value->as_int64();
  }

  /**
   * @brief Replays the log: the complete lines checked and applied; a torn tail and a missing result kept for
   *   `repair`.
   */
  void replay() {
    std::string text;
    const auto read = log_->read(text);
    if (read.status_ != FileRead::Status::OK) {
      throw PaperStateError("the paper log was not read: " + read.text(MAX_LOG_BYTES));
    }
    const auto last_newline = text.rfind('\n');
    const std::size_t complete = last_newline == std::string::npos ? 0 : last_newline + 1;
    // What the last placement or cancellation answered, awaiting its `result` line.
    std::optional<std::string> pending_of;
    std::string pending_id;
    std::string pending_error;
    std::size_t start = 0;
    std::size_t number = 0;
    while (start < complete) {
      const auto end = text.find('\n', start);
      const auto line = text.substr(start, end - start);
      start = end + 1;
      ++number;
      boost::system::error_code error;
      const auto value = json::parse(line, error);
      if (error || !value.is_object()) {
        throw broken(number, "is not a line the paper log writes");
      }
      const auto& o = value.as_object();
      const auto op = text_at(o, "op");
      if (!op) {
        throw broken(number, "has no op");
      }
      if (number == 1) {
        const auto format = integer_at(o, "format");
        const auto market = integer_at(o, "market");
        const auto fingerprint = text_at(o, "fingerprint");
        const auto at = integer_at(o, "start");
        if (*op != "genesis" || !format || !market || !fingerprint || !at || genesis_line(*fingerprint, *at) != line) {
          throw broken(number, "is not the genesis line the paper log begins with");
        }
        if (*format != LOG_FORMAT || *market != MARKET_VERSION) {
          throw broken(
            number,
            "is of format " + std::to_string(*format) + " and market " + std::to_string(*market) +
              "; this version reads " + std::to_string(LOG_FORMAT) + " and " + std::to_string(MARKET_VERSION)
          );
        }
        if (*fingerprint != fingerprint_ || *at != nanos(config_.paper_.start_at_)) {
          throw broken(number, "was begun on other bars, cash, start or lags than the operator's file names now");
        }
        continue;
      }
      if (*op == "result") {
        const auto of = text_at(o, "of");
        const auto id = text_at(o, "id");
        const auto refused = text_at(o, "error");
        if (!of || !id || !refused || result_line(*of, *id, *refused) != line) {
          throw broken(number, "is not a result line the paper log writes");
        }
        if (!pending_of) {
          throw broken(number, "answers no placement or cancellation");
        }
        if (*of != *pending_of || *id != pending_id || *refused != pending_error) {
          throw broken(number, "says the market answered differently from what it answers now");
        }
        pending_of.reset();
        continue;
      }
      if (pending_of) {
        throw broken(number, "follows a placement or cancellation that has no result");
      }
      if (*op == "advance") {
        const auto to = integer_at(o, "to");
        if (!to || advance_line(*to) != line) {
          throw broken(number, "is not an advance line the paper log writes");
        }
        const auto target = from_nanos(*to);
        if (!target || *target <= market_->now() || *target > clock_limit()) {
          throw broken(number, "moves the clock backwards or past the data");
        }
        market_->advance_to(*target);
      } else if (*op == "place") {
        PlaceFields fields;
        const auto at = integer_at(o, "at");
        const auto symbol = text_at(o, "symbol");
        const auto country = text_at(o, "country");
        const auto route = text_at(o, "route");
        const auto side = text_at(o, "side");
        const auto type = text_at(o, "type");
        const auto tif = text_at(o, "tif");
        const auto session = text_at(o, "session");
        const auto venue = text_at(o, "venue");
        const auto quantity = integer_at(o, "quantity");
        if (!at || !symbol || !country || !route || !side || !type || !tif || !session || !venue || !quantity) {
          throw broken(number, "is not a placement line the paper log writes");
        }
        fields =
          PlaceFields{*symbol, *country, *route, *side, *type, *tif, *session, *venue, *quantity, text_at(o, "price")};
        if (place_line(*at, fields) != line) {
          throw broken(number, "is not a placement line the paper log writes");
        }
        if (*at != nanos(market_->now())) {
          throw broken(number, "places at another instant than the clock");
        }
        CheckedPlace checked;
        if (auto problem = check_place(*market_, fields, checked)) {
          throw broken(number, "places an order the paper market never takes: " + *problem);
        }
        std::string refused;
        pending_id = market_->place(request_of(fields), checked.quantity_, checked.limit_, refused);
        pending_error = pending_id.empty() ? refused : std::string();
        pending_of = "place";
      } else if (*op == "cancel") {
        const auto at = integer_at(o, "at");
        const auto id = text_at(o, "id");
        if (!at || !id || cancel_line(*at, *id) != line) {
          throw broken(number, "is not a cancellation line the paper log writes");
        }
        if (*at != nanos(market_->now())) {
          throw broken(number, "cancels at another instant than the clock");
        }
        if (!valid_order_id(*id)) {
          throw broken(number, "cancels an id the paper market never writes");
        }
        std::string refused;
        pending_id = *id;
        pending_error = market_->cancel(*id, refused) ? std::string() : refused;
        pending_of = "cancel";
      } else {
        throw broken(number, "has an op the paper log never writes: " + *op);
      }
    }
    if (number == 0 && complete == 0 && text.size() == 0) {
      throw PaperStateError("the paper log is empty");
    }
    if (number == 0) {
      throw PaperStateError("the paper log has no complete line");
    }
    if (complete < text.size()) {
      // Cut by `repair` only, after every complete line was checked.
      cut_to_ = complete;
    }
    if (pending_of) {
      owed_result_ = result_line(*pending_of, pending_id, pending_error);
    }
  }

  const OperatorConfig& config_;
  std::string fingerprint_;
  std::shared_ptr<OpsLog> log_;
  std::shared_ptr<SimMarket> market_;
  std::shared_ptr<LoggedClient> client_;
  std::unique_ptr<Session> session_;
  std::optional<std::size_t> cut_to_;
  std::optional<std::string> owed_result_;
};

} // namespace paper
} // namespace cmdatl
#endif
