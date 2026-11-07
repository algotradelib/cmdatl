#pragma once

// Writing fetched bars into the Parquet store `bars.source = "store"` reads, in the layout and by the rule the lab's
// fetcher keeps (`labs/atl-data`): `<root>/bars/source=S/interval=I/symbol=SYM/bars.parquet`, the columns `source,
// interval, symbol, ts, stamp, open, high, low, close, volume, adj_close, currency, basis, fetched_at`, prices
// DECIMAL(18,4) and volume DECIMAL(24,4), every version kept. A bar is appended when its `ts` is new or any value
// differs from the latest stored version (null equal to null); the file is rewritten whole under the symbol
// directory's `.lock`, held across the read, the comparison and the rename, so two writers never lose each other's
// versions.

#include "./replay.hpp"

#include <algotradelib/core>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cmdatl {
namespace replay {

/** @brief What writing one series into the store did. */
struct StoredSeries {
  /** @brief The symbol. */
  std::string symbol_;
  /** @brief The file. */
  std::string path_;
  /** @brief The bars fetched. */
  std::size_t rows_{0};
  /** @brief The bars appended: new, or changed since the latest stored version. */
  std::size_t new_rows_{0};
  /** @brief The first bar's time, Unix seconds (as stamped); 0 for none. */
  long long first_{0};
  /** @brief The last bar's time, Unix seconds; 0 for none. */
  long long last_{0};
};

/**
 * @brief Whether a symbol can be one directory of the store: not empty, `.` or `..`, and holding no `/`, `\`, `=`, `,`,
 *   control character or NUL (`005930.KO` and `^GSPC` can).
 * @param symbol The symbol.
 * @return Whether it can.
 */
inline bool store_symbol(const std::string& symbol) {
  if (symbol.empty() || symbol == "." || symbol == ".." || symbol.size() > 64) {
    return false;
  }
  return std::none_of(symbol.begin(), symbol.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f || c == '/' || c == '\\' || c == '=' || c == ',';
  });
}

/**
 * @brief Whether a decimal's text fits the store's column exactly: at most `whole` digits before the point and
 *   `fraction` after it.
 * @param text The decimal, as `Decimal64::str` writes it.
 * @param whole The most digits before the point.
 * @param fraction The most digits after it.
 * @return Whether it fits.
 */
inline bool fits_decimal(const std::string& text, std::size_t whole, std::size_t fraction) {
  const auto start = !text.empty() && text[0] == '-' ? 1u : 0u;
  const auto point = text.find('.');
  const auto before = (point == std::string::npos ? text.size() : point) - start;
  const auto after = point == std::string::npos ? 0 : text.size() - point - 1;
  if (before == 0 || before > whole || after > fraction) {
    return false;
  }
  return std::all_of(text.begin() + start, text.end(), [](char c) {
    return (c >= '0' && c <= '9') || c == '.';
  });
}

#if defined(CMDATL_DUCKDB) && !defined(_WIN32)
/** @brief A symbol directory's `.lock`, held with `flock` while this lives. */
class StoreLock {
public:
  /**
   * @brief Takes the lock, waiting for another writer.
   * @param path The lock file.
   * @throws std::runtime_error When it cannot be opened or taken.
   */
  explicit StoreLock(const std::filesystem::path& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      throw std::runtime_error("the store's lock " + path.string() + " cannot be opened");
    }
    while (::flock(fd_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        ::close(fd_);
        throw std::runtime_error("the store's lock " + path.string() + " cannot be taken");
      }
    }
  }

  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;

  /** @brief Releases it. */
  ~StoreLock() {
    ::close(fd_);
  }

private:
  int fd_{-1};
};

/**
 * @brief Writes one fetched series into the store as the version fetched at `fetched_at_us`; see the file comment.
 * @param root The store's root.
 * @param source The source (the broker folder's name).
 * @param interval The interval's name (`1d`).
 * @param series The bars, ascending, each price within DECIMAL(18,4).
 * @param fetched_at_us When they were fetched, Unix microseconds.
 * @return What was written.
 * @throws std::invalid_argument When a symbol, a path or a value cannot be stored, or the store holds a version at or
 *   after `fetched_at_us`; std::runtime_error when DuckDB or the file system fails.
 */
inline StoredSeries store_series(
  const std::string& root, const std::string& source, const std::string& interval, const atl::BarSeries& series,
  std::int64_t fetched_at_us
) {
  const auto& symbol = series.instrument_.symbol_;
  if (!store_symbol(symbol)) {
    throw std::invalid_argument("the symbol '" + symbol + "' cannot be a directory of the store");
  }
  // The root as the spec names it; below it, each directory made one at a time, none of them a link: a link would
  // write outside the store.
  std::filesystem::create_directories(root);
  std::filesystem::path dir(root);
  for (const auto& part : {std::string("bars"), "source=" + source, "interval=" + interval, "symbol=" + symbol}) {
    dir /= part;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(dir, ec);
    if (status.type() == std::filesystem::file_type::symlink) {
      throw std::invalid_argument("the store's " + dir.string() + " is a link: refused");
    }
    if (status.type() == std::filesystem::file_type::not_found) {
      std::filesystem::create_directory(dir);
    } else if (status.type() != std::filesystem::file_type::directory) {
      throw std::invalid_argument("the store's " + dir.string() + " is not a directory");
    }
  }
  const auto path = dir / "bars.parquet";
  {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (
      status.type() != std::filesystem::file_type::not_found && status.type() != std::filesystem::file_type::regular
    ) {
      throw std::invalid_argument("the store's " + path.string() + " is not a regular file: refused");
    }
  }
  const char* stamp = series.stamp_ == atl::BarStamp::CLOSE ? "close" : "open";
  const std::string currency = series.currency_.str();
  StoredSeries out;
  out.symbol_ = symbol;
  out.path_ = path.string();
  out.rows_ = series.bars_.size();
  StoreLock lock(dir / ".lock");
  duck::Connection con;
  con.exec("SET TimeZone = 'UTC'");
  con.exec("SET autoinstall_known_extensions = false");
  con.exec("SET autoload_known_extensions = false");
  con.exec(
    "CREATE TEMP TABLE incoming (source VARCHAR, interval VARCHAR, symbol VARCHAR, ts TIMESTAMPTZ, stamp VARCHAR, "
    "open DECIMAL(18,4), high DECIMAL(18,4), low DECIMAL(18,4), close DECIMAL(18,4), volume DECIMAL(24,4), "
    "adj_close DECIMAL(18,4), currency VARCHAR, basis VARCHAR, fetched_at TIMESTAMPTZ)"
  );
  const auto fetched = "make_timestamp(" + std::to_string(fetched_at_us) + ")::TIMESTAMPTZ";
  std::string values;
  std::size_t pending = 0;
  const auto flush = [&] {
    if (pending > 0) {
      con.exec("INSERT INTO incoming VALUES " + values);
      values.clear();
      pending = 0;
    }
  };
  for (const auto& bar : series.bars_) {
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(bar.timestamp_.time_since_epoch()).count();
    if (!bar.open_) {
      throw std::invalid_argument(
        symbol + ": a bar without an open price cannot be stored (the store's reader needs one)"
      );
    }
    for (const auto* price : {&*bar.open_, &bar.high_, &bar.low_, &bar.close_}) {
      if (!fits_decimal(price->str(), 14, 4)) {
        throw std::invalid_argument(symbol + ": the price " + price->str() + " does not fit the store's DECIMAL(18,4)");
      }
    }
    if (!fits_decimal(bar.volume_.str(), 20, 4)) {
      throw std::invalid_argument(symbol + ": the volume " + bar.volume_.str() + " does not fit DECIMAL(24,4)");
    }
    if (out.first_ == 0) {
      out.first_ = ts;
    }
    out.last_ = ts;
    values += std::string(pending == 0 ? "" : ", ") + "(" + duck::literal(source) + ", " + duck::literal(interval) +
      ", " + duck::literal(symbol) + ", to_timestamp(" + std::to_string(ts) + "), '" + stamp + "', " +
      bar.open_->str() + ", " + bar.high_.str() + ", " + bar.low_.str() + ", " + bar.close_.str() + ", " +
      bar.volume_.str() + ", NULL, " + duck::literal(currency) + ", 'raw', " + fetched + ")";
    if (++pending == 500) {
      flush();
    }
  }
  flush();
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (exists) {
    con.exec(
      "CREATE TEMP TABLE stored AS SELECT * FROM read_parquet(" + duck::literal(path.string()) +
      ", hive_partitioning = false)"
    );
    const auto newer =
      con.rows("SELECT CAST(coalesce(max(fetched_at) >= " + fetched + ", false) AS VARCHAR) FROM stored");
    if (!newer.empty() && newer[0][0] == "true") {
      throw std::invalid_argument(
        symbol +
        ": the store holds a version fetched at or after this fetch (a clock moved back, or another fetch "
        "wrote first); fetch again"
      );
    }
    con.exec(
      "CREATE TEMP TABLE latest AS SELECT * EXCLUDE (rn) FROM (SELECT *, row_number() OVER (PARTITION BY ts ORDER BY "
      "fetched_at DESC) AS rn FROM stored) WHERE rn = 1"
    );
    con.exec(
      "CREATE TEMP TABLE fresh AS SELECT i.* FROM incoming i WHERE NOT EXISTS (SELECT 1 FROM latest l WHERE l.ts = i.ts "
      "AND l.stamp IS NOT DISTINCT FROM i.stamp AND l.open IS NOT DISTINCT FROM i.open AND l.high IS NOT DISTINCT FROM "
      "i.high AND l.low IS NOT DISTINCT FROM i.low AND l.close IS NOT DISTINCT FROM i.close AND l.volume IS NOT "
      "DISTINCT FROM i.volume AND l.adj_close IS NOT DISTINCT FROM i.adj_close AND l.currency IS NOT DISTINCT FROM "
      "i.currency AND l.basis IS NOT DISTINCT FROM i.basis)"
    );
  } else {
    con.exec("CREATE TEMP TABLE stored AS SELECT * FROM incoming WHERE false");
    con.exec("CREATE TEMP TABLE fresh AS SELECT * FROM incoming");
  }
  const auto counted = con.rows("SELECT count(*)::VARCHAR FROM fresh");
  out.new_rows_ = counted.empty() ? 0 : static_cast<std::size_t>(std::stoull(counted[0][0]));
  if (out.new_rows_ == 0 && exists) {
    return out;
  }
  const auto temporary = dir / (".bars.parquet." + std::to_string(static_cast<long long>(::getpid())) + ".tmp");
  con.exec(
    "COPY (SELECT * FROM stored UNION ALL BY NAME SELECT * FROM fresh ORDER BY ts, fetched_at) TO " +
    duck::literal(temporary.string()) + " (FORMAT PARQUET)"
  );
  std::filesystem::rename(temporary, path);
  return out;
}
#endif

} // namespace replay
} // namespace cmdatl
