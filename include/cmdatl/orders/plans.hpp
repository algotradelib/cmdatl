#pragma once

// The plan book: the plan ids `order plan` issues and `order place` uses once, kept in the account's directory as
// `plans.json`, each bound to a snapshot of what the plan was made on.
//
// The book is a second wall behind the journal's size in the snapshot: a file that fails any check is read as an
// empty book as a whole, so a damaged entry never becomes an authorization.

#include "../paper/bars.hpp"
#include "../paper/time.hpp"
#include "../system/files.hpp"
#include "../system/lock.hpp"
#include "../system/syscalls.hpp"

#include <algotradelib/core>

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cmdatl {

/** @brief The plan book's format. */
inline constexpr std::int64_t PLAN_FORMAT = 1;
/** @brief The most plans a book keeps; the oldest goes first. */
inline constexpr std::size_t MAX_PLANS = 64;
/** @brief The largest plan book read, in bytes. */
inline constexpr std::size_t MAX_PLAN_BYTES = 64u << 10;

/**
 * @brief Whether a text is a target: `0|[1-9][0-9]{0,8}`, whole shares, no sign, fraction, exponent or leading zero.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_target(std::string_view text) {
  if (text.empty() || text.size() > 9 || (text.size() > 1 && text.front() == '0')) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) {
    return c >= '0' && c <= '9';
  });
}

/**
 * @brief Whether a text is a limit: the mock market's price grammar, from 1 to 10^7.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_limit(std::string_view text) {
  return paper::parse_price(text).has_value();
}

/**
 * @brief Whether a text is a plan id: `p` and ten lowercase hex digits.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_plan_id(std::string_view text) {
  return text.size() == 11 && text.front() == 'p' && std::all_of(text.begin() + 1, text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

/**
 * @brief Whether a text is a snapshot: sixteen lowercase hex digits.
 * @param text The text.
 * @return Whether it is.
 */
inline bool valid_snapshot(std::string_view text) {
  return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

/**
 * @brief The low hex digits of a value.
 * @param value The value.
 * @param digits How many, at most 16.
 * @return The digits.
 */
inline std::string hex_digits(std::uint64_t value, int digits) {
  char out[20];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(value));
  return std::string(out + 16 - digits);
}

/** @brief One plan. */
struct Plan {
  /** @brief The symbol. */
  std::string symbol_;
  /** @brief The target, as typed. */
  std::string target_;
  /** @brief The limit, as typed. */
  std::string limit_;
  /** @brief The snapshot it was made on. */
  std::string snapshot_;
  /** @brief The mock clock when it was made, nanoseconds. */
  std::int64_t sim_{0};
  /** @brief Whether it was used. */
  bool used_{false};
};

/**
 * @brief The text a plan's snapshot hashes: the account's scope, the request as typed, the holding of the symbol,
 *   each working order on the symbol as `order_id:filled`, the cash of the buying power and the journal's size; a read
 *   that fails is written `?`, so a failure differs from every successful read.
 * @tparam Session The session.
 * @param session The session.
 * @param scope The journal's scope.
 * @param symbol The symbol.
 * @param target The target, as typed.
 * @param limit The limit, as typed.
 * @param journal_size The journal's size in bytes, or nothing when it could not be read.
 * @return The text.
 */
template <typename Session>
std::string snapshot_text(
  Session& session, const std::string& scope, const std::string& symbol, const std::string& target,
  const std::string& limit, std::optional<std::int64_t> journal_size
) {
  std::string text = scope + "|" + symbol + "|" + target + "|" + limit;
  if (auto holdings = session.holdings()) {
    algotradelib::Decimal64 held(0, 0);
    for (const auto& position : holdings.value().positions_) {
      if (position.instrument_.symbol_ == symbol) {
        held = position.quantity_;
      }
    }
    text += "|held=" + held.str();
  } else {
    text += "|held=?";
  }
  if (auto working = session.working_orders()) {
    for (const auto& w : working.value().orders_) {
      if (w.order_.instrument_.symbol_ == symbol) {
        text += "|" + w.order_.order_id_ + ":" + w.order_.execution_.filled_quantity_.str();
      }
    }
  } else {
    text += "|orders=?";
  }
  if (auto cash = session.buying_power(algotradelib::Currency::krw())) {
    text += "|cash=" + cash.value().cash_.amount_.str();
  } else {
    text += "|cash=?";
  }
  text += journal_size ? "|journal=" + std::to_string(*journal_size) : std::string("|journal=?");
  return text;
}

/**
 * @brief A plan's snapshot: FNV-1a 64 of `snapshot_text`, as sixteen hex digits.
 * @tparam Session The session.
 * @param session The session.
 * @param scope The journal's scope.
 * @param symbol The symbol.
 * @param target The target, as typed.
 * @param limit The limit, as typed.
 * @param journal_size The journal's size in bytes, or nothing when it could not be read.
 * @return The snapshot.
 */
template <typename Session>
std::string snapshot_of(
  Session& session, const std::string& scope, const std::string& symbol, const std::string& target,
  const std::string& limit, std::optional<std::int64_t> journal_size
) {
  return hex_digits(paper::fnv1a(snapshot_text(session, scope, symbol, target, limit, journal_size)), 16);
}

/**
 * @brief How many keys a JSON text holds: its colons outside strings, which only separate a key from its value.
 * @param text The text, already parsed as JSON.
 * @return The count.
 */
inline std::size_t key_count(std::string_view text) {
  std::size_t count = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char c : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
    } else if (c == '"') {
      in_string = true;
    } else if (c == ':') {
      ++count;
    }
  }
  return count;
}

/** @brief The plan book, read whole and checked, or empty when any check failed. */
class PlanBook {
public:
  /**
   * @brief Reads a book's text: exactly the members of format 1, in any order, none repeated, every value by its rule.
   * @param text The text.
   * @return The book, or nothing when any rule fails.
   */
  static std::optional<std::vector<std::pair<std::string, Plan>>> parse(const std::string& text) {
    namespace json = boost::json;
    boost::system::error_code error;
    const auto value = json::parse(text, error);
    if (error || !value.is_object()) {
      return std::nullopt;
    }
    const auto& root = value.as_object();
    if (root.size() != 2 || !root.contains("format") || !root.contains("plans")) {
      return std::nullopt;
    }
    const auto& format = root.at("format");
    if (!format.is_int64() || format.as_int64() != PLAN_FORMAT || !root.at("plans").is_object()) {
      return std::nullopt;
    }
    const auto& plans = root.at("plans").as_object();
    if (plans.size() > MAX_PLANS || key_count(text) != 2 + 7 * plans.size()) {
      return std::nullopt;
    }
    std::vector<std::pair<std::string, Plan>> out;
    for (const auto& entry : plans) {
      const std::string id(entry.key());
      if (!valid_plan_id(id) || !entry.value().is_object()) {
        return std::nullopt;
      }
      const auto& o = entry.value().as_object();
      if (o.size() != 6) {
        return std::nullopt;
      }
      const auto* symbol = o.if_contains("symbol");
      const auto* target = o.if_contains("target");
      const auto* limit = o.if_contains("limit");
      const auto* snapshot = o.if_contains("snapshot");
      const auto* sim = o.if_contains("sim");
      const auto* used = o.if_contains("used");
      if (
        symbol == nullptr || target == nullptr || limit == nullptr || snapshot == nullptr || sim == nullptr ||
        used == nullptr || !symbol->is_string() || !target->is_string() || !limit->is_string() ||
        !snapshot->is_string() || !sim->is_int64() || !used->is_bool()
      ) {
        return std::nullopt;
      }
      Plan plan;
      plan.symbol_ = std::string(symbol->as_string());
      plan.target_ = std::string(target->as_string());
      plan.limit_ = std::string(limit->as_string());
      plan.snapshot_ = std::string(snapshot->as_string());
      plan.sim_ = sim->as_int64();
      plan.used_ = used->as_bool();
      if (
        !paper::valid_symbol(plan.symbol_) || !valid_target(plan.target_) || !valid_limit(plan.limit_) ||
        !valid_snapshot(plan.snapshot_) || !paper::from_nanos(plan.sim_)
      ) {
        return std::nullopt;
      }
      out.emplace_back(id, std::move(plan));
    }
    return out;
  }

#if !defined(_WIN32)
  /**
   * @brief Reads the book in a directory: a missing file is an empty book; one that fails the descriptor checks or any
   *   rule is an empty book marked damaged.
   * @param dir The account's directory.
   * @return The book.
   */
  static PlanBook load(const std::string& dir) {
    PlanBook book(dir);
    ScopedFd fd(open_lifted(book.path().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
      book.damaged_ = errno != ENOENT;
      return book;
    }
    int error = 0;
    std::string text;
    if (
      !single_regular(fd.get(), error) || read_whole(fd.get(), MAX_PLAN_BYTES, text).status_ != FileRead::Status::OK
    ) {
      book.damaged_ = true;
      return book;
    }
    auto plans = parse(text);
    if (!plans) {
      book.damaged_ = true;
      return book;
    }
    book.plans_ = std::move(*plans);
    return book;
  }
#endif

  /**
   * @brief The book's file.
   * @return The path.
   */
  std::string path() const {
    return dir_ + "/plans.json";
  }

  /**
   * @brief Whether the file failed a check and was read as empty.
   * @return Whether it did.
   */
  bool damaged() const noexcept {
    return damaged_;
  }

  /**
   * @brief A plan.
   * @param id The plan id.
   * @return The plan, or nothing.
   */
  std::optional<Plan> find(const std::string& id) const {
    for (const auto& entry : plans_) {
      if (entry.first == id) {
        return entry.second;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Adds a plan, dropping the oldest past `MAX_PLANS`; not written until `save`.
   * @param plan The plan.
   * @return Its id: `p` and ten hex digits of FNV-1a over the snapshot and the wall clock's nanoseconds.
   */
  std::string issue(Plan plan) {
    const auto wall =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto id = "p" + hex_digits(paper::fnv1a(plan.snapshot_ + std::to_string(wall)), 10);
    plans_.erase(
      std::remove_if(
        plans_.begin(), plans_.end(),
        [&id](const auto& entry) {
          return entry.first == id;
        }
      ),
      plans_.end()
    );
    plans_.emplace_back(id, std::move(plan));
    while (plans_.size() > MAX_PLANS) {
      plans_.erase(plans_.begin());
    }
    return id;
  }

  /**
   * @brief Marks a plan used; not written until `save`.
   * @param id The plan id.
   */
  void use(const std::string& id) {
    for (auto& entry : plans_) {
      if (entry.first == id) {
        entry.second.used_ = true;
      }
    }
  }

  /**
   * @brief The book's text.
   * @return The JSON, with its newline.
   */
  std::string text() const {
    namespace json = boost::json;
    json::object plans;
    for (const auto& entry : plans_) {
      json::object o;
      o["symbol"] = entry.second.symbol_;
      o["target"] = entry.second.target_;
      o["limit"] = entry.second.limit_;
      o["snapshot"] = entry.second.snapshot_;
      o["sim"] = entry.second.sim_;
      o["used"] = entry.second.used_;
      plans[entry.first] = std::move(o);
    }
    json::object root;
    root["format"] = PLAN_FORMAT;
    root["plans"] = std::move(plans);
    return json::serialize(root) + "\n";
  }

#if !defined(_WIN32)
  /** @brief How writing the book ended. */
  struct Written {
    /** @brief Whether the account's lock was no longer this process's, so nothing was published. */
    bool lock_lost_{false};
    /** @brief The error number of a failed write, or 0. */
    int error_{0};
  };

  /**
   * @brief Writes the book under the account's lock: the lock checked, a temporary created exclusively, written whole
   *   and flushed, the lock checked again, then the temporary renamed onto the book; a failure or a lost lock removes
   *   only this call's temporary and leaves the book as it was.
   * @param lock The account's lock, held.
   * @return How it ended.
   */
  Written save(const OwnerLock& lock) const {
    Written written;
    const auto content = text();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto salt = paper::fnv1a(std::to_string(stamp) + "|" + std::to_string(::getpid()) + "|" + content);
    const auto temporary = dir_ + "/plans." + std::to_string(::getpid()) + "." + hex_digits(salt, 16) + ".tmp";
    if (!lock.still_at_path()) {
      written.lock_lost_ = true;
      return written;
    }
    ScopedFd fd(open_lifted(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (fd.get() < 0) {
      written.error_ = errno;
      return written;
    }
    std::size_t count = 0;
    int error = write_whole(fd.get(), content.data(), content.size(), count);
    if (error == 0 && sys::fsync(fd.get()) != 0) {
      error = errno;
    }
    fd.reset();
    sys::kill_point("plans.written");
    if (error == 0 && !lock.still_at_path()) {
      ::unlink(temporary.c_str());
      written.lock_lost_ = true;
      return written;
    }
    if (error == 0 && sys::rename(temporary.c_str(), path().c_str()) != 0) {
      error = errno;
    }
    if (error != 0) {
      ::unlink(temporary.c_str());
    }
    written.error_ = error;
    return written;
  }
#endif

private:
  /**
   * @brief An empty book in a directory.
   * @param dir The directory.
   */
  explicit PlanBook(std::string dir) : dir_(std::move(dir)) {}

  std::string dir_;
  std::vector<std::pair<std::string, Plan>> plans_;
  bool damaged_{false};
};

} // namespace cmdatl
