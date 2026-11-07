#pragma once

// A backtest on a broker folder's history (`bars.source = "<name>"`): the backtest driver, built with the folder's
// adapter, connects with the login `strategy backtest` hands it, fetches each symbol's last `count` bars, writes them
// into the store as one version (`store_write.hpp`), then replays that version: the store read as of the fetch, from
// its first bar, so a later revision by another writer and an older history already in the store are outside it.
//
// The adapter's error text is never written: its category, HTTP status and code are (a vendor may put its token in a
// URL). cmdatl's wiring calls only `connect` and `bars`, and builds no trading session.

#include "./replay.hpp"
#include "./settings.hpp"
#include "./store_write.hpp"

#include <algotradelib/broker>

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace replay {

/**
 * @brief An error's category, as a word.
 * @param category The category.
 * @return The word.
 */
inline const char* category_word(atl::ErrorCategory category) {
  switch (category) {
    case atl::ErrorCategory::CANCELLED:
      return "cancelled";
    case atl::ErrorCategory::TRANSPORT:
      return "transport";
    case atl::ErrorCategory::HTTP:
      return "http";
    case atl::ErrorCategory::AUTHENTICATION:
      return "authentication";
    case atl::ErrorCategory::RATE_LIMIT:
      return "rate_limit";
    case atl::ErrorCategory::PROTOCOL:
      return "protocol";
    case atl::ErrorCategory::INVALID_ARGUMENT:
      return "invalid_argument";
  }
  return "unknown";
}

/**
 * @brief A broker code as shown: a short printable one, else none (the adapter sets it, and it is not cmdatl's).
 * @param code The code.
 * @return It, or empty.
 */
inline std::string shown_code(const std::string& code) {
  const bool shown = !code.empty() && code.size() <= 32 && std::all_of(code.begin(), code.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
      c == '.';
  });
  return shown ? code : std::string();
}

/**
 * @brief What is shown of a broker folder adapter's error: its category, HTTP status and broker code, and whether it
 *   reached the broker; never its text.
 * @param error The error.
 * @return The text.
 */
inline std::string adapter_error_text(const atl::BrokerError& error) {
  std::string out = std::string("the broker folder's adapter answered ") + category_word(error.category_);
  if (error.http_status_ != 0) {
    out += ", HTTP " + std::to_string(error.http_status_);
  }
  const auto code = shown_code(error.broker_code_);
  if (!code.empty()) {
    out += ", code " + code;
  }
  out += error.reached_broker_ ? " (it reached the broker)" : " (before reaching the broker)";
  return out + "; its text is not shown";
}

/**
 * @brief An instant as UTC text DuckDB reads as a TIMESTAMPTZ, to the microsecond.
 * @param us Unix microseconds.
 * @return `YYYY-MM-DD HH:MM:SS.ffffff+00`.
 */
inline std::string utc_instant(std::int64_t us) {
  const std::time_t seconds = static_cast<std::time_t>(us / 1000000);
  std::tm parts{};
  ::gmtime_r(&seconds, &parts);
  char text[40];
  std::snprintf(
    text, sizeof text, "%04d-%02d-%02d %02d:%02d:%02d.%06lld+00", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
    parts.tm_hour, parts.tm_min, parts.tm_sec, static_cast<long long>(us % 1000000)
  );
  return text;
}

/**
 * @brief A Unix time's UTC date.
 * @param seconds Unix seconds.
 * @return `YYYY-MM-DD`.
 */
inline std::string utc_date(long long seconds) {
  const std::time_t at = static_cast<std::time_t>(seconds);
  std::tm parts{};
  ::gmtime_r(&at, &parts);
  char text[16];
  std::snprintf(text, sizeof text, "%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
  return text;
}

/**
 * @brief Checks a fetched series is what was asked: the instrument, the interval, raw prices, ascending unique times.
 * @param series The series.
 * @param query The query.
 * @return What is wrong, or none.
 */
inline std::optional<std::string> fetched_problem(const atl::BarSeries& series, const atl::BarQuery& query) {
  if (!(series.instrument_ == query.instrument_)) {
    return std::string("the adapter answered another instrument");
  }
  if (series.interval_ != query.interval_) {
    return std::string("the adapter answered another interval");
  }
  if (series.adjusted_) {
    return std::string("the adapter answered adjusted prices; raw ones were asked");
  }
  for (std::size_t k = 1; k < series.bars_.size(); ++k) {
    if (!(series.bars_[k - 1].timestamp_ < series.bars_[k].timestamp_)) {
      return std::string("the adapter's bars are not in ascending time, each once");
    }
  }
  return std::nullopt;
}

/**
 * @brief Writes the fetch's outcome where `strategy backtest` reads it.
 * @param path The file.
 * @param outcome The outcome.
 * @return Whether it was written.
 */
inline bool write_fetch_outcome(const std::string& path, const boost::json::object& outcome) {
  std::ofstream out(path, std::ios::trunc);
  out << boost::json::serialize(outcome) << '\n';
  out.flush();
  return static_cast<bool>(out);
}

/**
 * @brief The backtest driver's entry with a broker folder's adapter; see the file comment.
 * @tparam Strategy A strategy generated by `atlc`.
 * @tparam Fills A callable that gives the fill model the run's market-price rule.
 * @tparam Connect A callable from the login's JSON text to the adapter's connection (`client_`, `account_`).
 * @param argc The argument count.
 * @param argv The settings, each `key=value`, with `fetch.source`, `fetch.count`, `fetch.interval` and
 *   `fetch.outcome` (the file the outcome goes to); `bars.source = "store"` and `bars.store_root` given.
 * @param fills_of Gives the fill model.
 * @param connect Connects with the login.
 * @param login The login read from descriptor 3, or none.
 * @return The replay's exit (0 or 1), or 3 when the fetch failed (its outcome written).
 */
template <typename Strategy, typename Fills, typename Connect>
int fetch_main(int argc, char** argv, Fills fills_of, Connect connect, const std::optional<std::string>& login) {
  namespace json = boost::json;
  std::vector<std::string> args;
  std::string source;
  std::string interval = "1d";
  std::string outcome_path;
  int count = 0;
  for (int k = 1; k < argc; ++k) {
    const std::string arg(argv[k]);
    const auto take = [&arg](const char* key) {
      const std::string prefix = std::string(key) + "=";
      return arg.rfind(prefix, 0) == 0 ? std::optional<std::string>(arg.substr(prefix.size())) : std::nullopt;
    };
    if (const auto v = take("fetch.source")) {
      source = *v;
    } else if (const auto v = take("fetch.interval")) {
      interval = *v;
    } else if (const auto v = take("fetch.count")) {
      count = std::atoi(v->c_str());
    } else if (const auto v = take("fetch.outcome")) {
      outcome_path = *v;
    } else {
      args.push_back(arg);
    }
  }
  json::object outcome{{"source", source}};
  const auto failed = [&](json::object error) {
    outcome["error"] = std::move(error);
    (void)write_fetch_outcome(outcome_path, outcome);
    return 3;
  };
  if (source.empty() || outcome_path.empty() || count <= 0) {
    const std::string usage = "the driver needs fetch.source, fetch.count and fetch.outcome";
    std::cerr << "error: " << usage << "; run it with cmdatl strategy backtest\n";
    return 1;
  }
  if (!login || login->empty()) {
    return failed({{"message", "the driver received no login on descriptor 3; run it with cmdatl strategy backtest"}});
  }
  std::vector<std::string> symbols;
  std::string market;
  std::string root;
  try {
    std::vector<std::string> owned{"driver"};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> raw;
    for (auto& arg : owned) {
      raw.push_back(arg.data());
    }
    const auto s = Settings::from_args(static_cast<int>(raw.size()), raw.data());
    symbols = symbols_of<Strategy>(s);
    market = market_of(s);
    root = s.need("bars.store_root");
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
  if (symbols.empty()) {
    std::cerr << "error: a backtest on a login's history names its symbols in [instruments]\n";
    return 1;
  }
  for (const auto& symbol : symbols) {
    if (!store_symbol(symbol)) {
      return failed({{"symbol", symbol}, {"message", "the symbol cannot be a directory of the store"}});
    }
  }
  const auto fetched_at =
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  outcome["fetched_at"] = utc_instant(fetched_at);
  json::array stored;
  long long first = 0;
  try {
    // The adapter's exceptions are not cmdatl's: only that one was thrown is written.
    auto connection = [&]() -> std::optional<decltype(connect(*login))> {
      try {
        return connect(*login);
      } catch (...) {
        return std::nullopt;
      }
    }();
    if (!connection) {
      return failed(
        {{"message", "the broker folder's adapter could not use the stored login (its text is not shown)"}}
      );
    }
    using Client = typename std::decay_t<decltype(connection->client_)>::element_type;
    atl::BlockingSession<Client> session(connection->client_, std::move(connection->account_));
    for (const auto& symbol : symbols) {
      atl::BarQuery query;
      query.instrument_ = instrument_of(symbol, market).id_;
      query.interval_ = interval_of(interval);
      query.count_ = count;
      query.adjusted_ = false;
      std::optional<atl::Result<atl::BarSeries>> answer;
      try {
        answer.emplace(session.bars(query));
      } catch (...) {
        return failed({{"symbol", symbol}, {"message", "the broker folder's adapter threw (its text is not shown)"}});
      }
      if (!*answer) {
        const auto& error = answer->error();
        json::object detail{
          {"symbol", symbol},
          {"category", category_word(error.category_)},
          {"reached_broker", error.reached_broker_},
          {"message", adapter_error_text(error)},
        };
        if (error.http_status_ != 0) {
          detail["http_status"] = error.http_status_;
        }
        if (!shown_code(error.broker_code_).empty()) {
          detail["broker_code"] = shown_code(error.broker_code_);
        }
        return failed(std::move(detail));
      }
      const auto& series = answer->value();
      if (const auto problem = fetched_problem(series, query)) {
        return failed({{"symbol", symbol}, {"message", *problem}});
      }
#if defined(CMDATL_DUCKDB)
      const auto written = store_series(root, source, interval, series, fetched_at);
      if (written.rows_ > 0 && (first == 0 || written.first_ < first)) {
        first = written.first_;
      }
      stored.emplace_back(
        json::object{
          {"symbol", symbol},
          {"rows", written.rows_},
          {"new_rows", written.new_rows_},
          {"first", written.rows_ > 0 ? json::value(utc_date(written.first_)) : json::value(nullptr)},
          {"last", written.rows_ > 0 ? json::value(utc_date(written.last_)) : json::value(nullptr)},
          {"path", written.path_},
        }
      );
#else
      (void)first;
      return failed({{"message", "this driver was built without DuckDB, which writes the store"}});
#endif
    }
  } catch (const std::exception& e) {
    // Paths and values of cmdatl's own checks; never the adapter's text, which is caught above.
    return failed({{"message", e.what()}});
  }
  outcome["series"] = std::move(stored);
  if (!write_fetch_outcome(outcome_path, outcome)) {
    std::cerr << "error: the fetch's outcome cannot be written to " << outcome_path << '\n';
    return 1;
  }
  // The replay of this version: the store as of the fetch, from its first bar (or a later replay.from).
  std::string coordinates;
  for (const auto& symbol : symbols) {
    coordinates += (coordinates.empty() ? "" : ",") + source + "/" + interval + "/" + symbol;
  }
  args.push_back("bars.store=" + coordinates);
  args.push_back("bars.as_of=" + utc_instant(fetched_at));
  if (first > 0) {
    std::optional<std::string> from;
    for (const auto& arg : args) {
      if (arg.rfind("replay.from=", 0) == 0) {
        from = arg.substr(12);
      }
    }
    const auto start = utc_date(first);
    if (!from || *from < start) {
      args.push_back("replay.from=" + start);
    }
  }
  std::vector<std::string> owned{"driver"};
  owned.insert(owned.end(), args.begin(), args.end());
  std::vector<char*> raw;
  for (auto& arg : owned) {
    raw.push_back(arg.data());
  }
  return main<Strategy>(static_cast<int>(raw.size()), raw.data(), fills_of);
}

} // namespace replay
} // namespace cmdatl
