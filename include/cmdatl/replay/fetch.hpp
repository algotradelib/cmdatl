#pragma once

// A backtest on a broker's or a broker folder's history (`bars.source = "<name>"`): the backtest driver, built with the
// broker's client or the folder's adapter, connects with the login `strategy backtest` hands it, fetches each symbol's
// last `count` bars, drops a bar not finished when the fetch began, writes them
// into the store as one version (`store_write.hpp`), then replays exactly the bars it received (written as a CSV beside
// its outcome), never a later revision by another writer or an older history already in the store.
//
// The adapter's error text is never written: its category, HTTP status and code are (a vendor may put its token in a
// URL). cmdatl's wiring calls only `connect` and, through `BarsOnly`, `bars`.

#include "./adapter_text.hpp"
#include "./replay.hpp"
#include "./settings.hpp"
#include "./store_write.hpp"

#include <algotradelib/broker>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cmdatl {
namespace replay {

/**
 * @brief An instant as UTC text, to the microsecond, as `bars.as_of` takes it and DuckDB reads it.
 * @param us Unix microseconds.
 * @return `YYYY-MM-DDTHH:MM:SS.ffffffZ`.
 */
inline std::string utc_instant(std::int64_t us) {
  const std::time_t seconds = static_cast<std::time_t>(us / 1000000);
  std::tm parts{};
  ::gmtime_r(&seconds, &parts);
  char text[96];
  std::snprintf(
    text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
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
  char text[64];
  std::snprintf(text, sizeof text, "%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
  return text;
}

/**
 * @brief The market date a bar states, as the replay dates it (`market_noon`): a daily bar stamped 00:00 KST, 15:00 UTC
 *   the day before, is that KST date.
 * @param seconds The bar's stamp, Unix seconds.
 * @param market The market (`kr`, `us`).
 * @return `YYYY-MM-DD`.
 */
inline std::string market_date(long long seconds, const std::string& market) {
  using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
  const auto session = session_of(market);
  const auto noon = market_noon(std::chrono::system_clock::time_point(std::chrono::seconds(seconds)), session);
  return utc_date(std::chrono::floor<Days>(noon + session.utc_offset_).time_since_epoch().count() * 86400);
}

/**
 * @brief Checks a fetched series is what was asked: the instrument, the interval, raw prices, ascending unique times.
 * @param series The series.
 * @param query The query.
 * @param who What answered: `the adapter`, or `the broker` for a built-in.
 * @return What is wrong, or none.
 */
inline std::optional<std::string> fetched_problem(
  const atl::BarSeries& series, const atl::BarQuery& query, const std::string& who = "the adapter"
) {
  if (!(series.instrument_ == query.instrument_)) {
    return who + " answered another instrument";
  }
  if (series.interval_ != query.interval_) {
    return who + " answered another interval";
  }
  if (series.adjusted_) {
    return who + " answered adjusted prices; raw ones were asked";
  }
  for (std::size_t k = 1; k < series.bars_.size(); ++k) {
    if (!(series.bars_[k - 1].timestamp_ < series.bars_[k].timestamp_)) {
      return who + "'s bars are not in ascending time, each once";
    }
  }
  return std::nullopt;
}

/**
 * @brief The bars of a series that were finished when the fetch began: an intraday bar that ended by then (its stamp
 *   for `close`, its stamp plus the interval for `open`); a daily bar of an earlier market date (the extended sessions
 *   can still change today's); a week whose date plus six days, or a month whose last day, is before the fetch's date.
 * @param series The series.
 * @param fetched_at When the fetch began.
 * @param market The market (`kr`, `us`), whose session dates the bars.
 * @return The series with only those bars.
 */
inline atl::BarSeries finished_bars(
  atl::BarSeries series, std::chrono::system_clock::time_point fetched_at, const std::string& market
) {
  using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
  const auto session = session_of(market);
  const auto date_of = [&session](std::chrono::system_clock::time_point at) {
    return std::chrono::floor<Days>(at + session.utc_offset_).time_since_epoch().count();
  };
  const auto today = date_of(fetched_at);
  const auto length = atl::bar_interval_length(series.interval_);
  const auto unfinished = [&](const atl::Bar& bar) {
    if (length) {
      const bool open = series.stamp_ != atl::BarStamp::CLOSE;
      return bar.timestamp_ + (open ? *length : std::chrono::seconds(0)) > fetched_at;
    }
    const auto date = date_of(market_noon(bar.timestamp_, session));
    if (series.interval_ == atl::BarInterval::ONE_DAY || series.interval_ == atl::BarInterval::ONE_WEEK) {
      return date + (series.interval_ == atl::BarInterval::ONE_WEEK ? 6 : 0) >= today;
    }
    // A month: the day before the next month's first, from the year and month (never an invalid date).
    const std::time_t at = static_cast<std::time_t>(date * 86400);
    std::tm parts{};
    ::gmtime_r(&at, &parts);
    const int next = parts.tm_mon == 11 ?
      (parts.tm_year + 1901) * 10000 + 101 :
      (parts.tm_year + 1900) * 10000 + (parts.tm_mon + 2) * 100 + 1;
    return day_seconds(next) / 86400 - 1 >= today;
  };
  series.bars_.erase(std::remove_if(series.bars_.begin(), series.bars_.end(), unfinished), series.bars_.end());
  return series;
}

/**
 * @brief What a fetch may call on a session: its bars, and nothing else (no order, no account read).
 * @tparam Session The session, borrowed.
 */
template <typename Session>
class BarsOnly {
public:
  /** @brief Borrows a session. @param session The session. */
  explicit BarsOnly(Session& session) : session_(session) {}

  /** @brief The session's bars. @param query The query. @return Its answer. */
  auto bars(const atl::BarQuery& query) {
    return session_.bars(query);
  }

private:
  Session& session_;
};

/**
 * @brief Appends a fetched series to the replay's CSV: one row per bar, as the CSV loader reads a long file.
 * @param csv The CSV so far.
 * @param series The series.
 */
inline void append_csv(std::string& csv, const atl::BarSeries& series) {
  const char* stamp = series.stamp_ == atl::BarStamp::CLOSE ? "close" : "open";
  for (const auto& bar : series.bars_) {
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(bar.timestamp_.time_since_epoch()).count();
    csv += series.instrument_.symbol_ + "," + std::to_string(ts) + "," + stamp + "," +
      (bar.open_ ? bar.open_->str() : std::string()) + "," + bar.high_.str() + "," + bar.low_.str() + "," +
      bar.close_.str() + "," + bar.volume_.str() + "\n";
  }
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
 * @tparam Measures A callable that gives the report's measures.
 * @tparam Connect A callable from the login's JSON text and the options' to the adapter's connection (`client_`,
 *   `account_`).
 * @param argc The argument count.
 * @param argv The settings, each `key=value`, with `fetch.source`, `fetch.count`, `fetch.interval`, `fetch.options`
 *   (the options checked against the folder's declarations, a JSON object's text; `{}` without it), `fetch.outcome`
 *   (the file the outcome goes to) and, for a built-in broker, `fetch.store_source` (the store's `source=`, else
 *   `fetch.source`); `bars.source = "store"` and `bars.store_root` given.
 * @param fills_of Gives the fill model.
 * @param measures_of Gives the report's measures.
 * @param connect Connects with the login.
 * @param login The login read from descriptor 3, or none.
 * @return The replay's exit (0 or 1), or 3 when the fetch failed (its outcome written).
 */
template <typename Strategy, typename Fills, typename Measures, typename Connect>
int fetch_main(
  int argc, char** argv, Fills fills_of, Measures measures_of, Connect connect, const std::optional<std::string>& login
) {
  namespace json = boost::json;
  std::vector<std::string> args;
  std::string source;
  std::string store_source;
  std::string interval = "1d";
  std::string outcome_path;
  std::string options = "{}";
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
      char* end = nullptr;
      const long parsed = std::strtol(v->c_str(), &end, 10);
      count = end != v->c_str() && *end == '\0' && parsed > 0 && parsed <= 100000 ? static_cast<int>(parsed) : 0;
    } else if (const auto v = take("fetch.outcome")) {
      outcome_path = *v;
    } else if (const auto v = take("fetch.options")) {
      options = *v;
    } else if (const auto v = take("fetch.store_source")) {
      store_source = *v;
    } else {
      args.push_back(arg);
    }
  }
  json::object outcome{{"source", source}};
  const std::string who = store_source.empty() ? "the broker folder's adapter" : "the broker";
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
  // The bars received, as a CSV the replay reads: exactly this fetch's bars, never another version or symbol's.
  std::string replayed = "symbol,ts,stamp,open,high,low,close,volume\n";
  try {
    // The adapter's exceptions are not cmdatl's: only that one was thrown is written.
    auto connection = [&]() -> std::optional<decltype(connect(*login, options))> {
      try {
        return connect(*login, options);
      } catch (...) {
        return std::nullopt;
      }
    }();
    if (!connection) {
      return failed(
        {{"message",
          who + " could not use the stored login" + (store_source.empty() ? " or the options" : "") +
            " (its text is not shown)"}}
      );
    }
    using Client = typename std::decay_t<decltype(connection->client_)>::element_type;
    atl::BlockingSession<Client> session(connection->client_, std::move(connection->account_));
    BarsOnly<atl::BlockingSession<Client>> history(session);
    for (const auto& symbol : symbols) {
      atl::BarQuery query;
      query.instrument_ = instrument_of(symbol, market).id_;
      query.interval_ = interval_of(interval);
      query.count_ = count;
      query.adjusted_ = false;
      std::optional<atl::Result<atl::BarSeries>> answer;
      try {
        answer.emplace(history.bars(query));
      } catch (...) {
        return failed({{"symbol", symbol}, {"message", who + " threw (its text is not shown)"}});
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
      if (
        const auto problem =
          fetched_problem(answer->value(), query, store_source.empty() ? "the adapter" : "the broker")
      ) {
        return failed({{"symbol", symbol}, {"message", *problem}});
      }
      const auto series = finished_bars(
        answer->value(), std::chrono::system_clock::time_point(std::chrono::microseconds(fetched_at)), market
      );
      // No history is no backtest: never replaced by bars an earlier fetch left in the store.
      if (series.bars_.empty()) {
        return failed({{"symbol", symbol}, {"message", who + " answered no bars finished when the fetch began"}});
      }
#if defined(CMDATL_DUCKDB)
      const auto written =
        store_series(root, store_source.empty() ? source : store_source, interval, series, fetched_at);
      append_csv(replayed, series);
      stored.emplace_back(
        json::object{
          {"symbol", symbol},
          {"rows", written.rows_},
          {"new_rows", written.new_rows_},
          {"first", written.rows_ > 0 ? json::value(market_date(written.first_, market)) : json::value(nullptr)},
          {"last", written.rows_ > 0 ? json::value(market_date(written.last_, market)) : json::value(nullptr)},
          {"path", written.path_},
        }
      );
#else
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
  // The replay of exactly the bars received, written beside the outcome file; the store keeps them as a version.
  const auto csv = std::filesystem::path(outcome_path).parent_path() / "fetched.csv";
  {
    std::ofstream out(csv, std::ios::trunc);
    out << replayed;
    out.flush();
    if (!out) {
      std::cerr << "error: the fetched bars cannot be written to " << csv.string() << '\n';
      return 1;
    }
  }
  std::vector<std::string> kept;
  for (const auto& arg : args) {
    if (arg.rfind("bars.source=", 0) != 0 && arg.rfind("bars.store_root=", 0) != 0) {
      kept.push_back(arg);
    }
  }
  args = std::move(kept);
  args.push_back("bars.source=csv");
  args.push_back("bars.file=" + csv.string());
  args.push_back("bars.symbol_column=symbol");
  args.push_back("bars.interval=" + interval);
  std::vector<std::string> owned{"driver"};
  owned.insert(owned.end(), args.begin(), args.end());
  std::vector<char*> raw;
  for (auto& arg : owned) {
    raw.push_back(arg.data());
  }
  return main<Strategy>(static_cast<int>(raw.size()), raw.data(), fills_of, measures_of);
}

} // namespace replay
} // namespace cmdatl
