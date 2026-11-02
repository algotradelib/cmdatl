// Writes a Parquet store for the store backtest's acceptance case with DuckDB: one series, `test/1d/AAA`, 120 daily
// bars from 2020-01-01 that swing, so a mean-reversion program trades; one fetched version of each bar.
// Usage: make-store STORE_ROOT

#include <duckdb.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

/**
 * @brief Runs a statement, printing DuckDB's error when it fails.
 * @param con The connection.
 * @param sql The statement.
 * @return Whether it ran.
 */
bool exec(duckdb_connection con, const std::string& sql) {
  duckdb_result result;
  const bool ok = duckdb_query(con, sql.c_str(), &result) != DuckDBError;
  if (!ok) {
    std::fprintf(stderr, "duckdb: %s\n  in: %s\n", duckdb_result_error(&result), sql.c_str());
  }
  duckdb_destroy_result(&result);
  return ok;
}

} // namespace

/**
 * @brief Writes the store.
 * @param argc The count.
 * @param argv The store's root.
 * @return 0, 1 when DuckDB fails, 2 on bad usage.
 */
int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: make-store STORE_ROOT\n");
    return 2;
  }
  const auto dir = std::filesystem::path(argv[1]) / "bars" / "source=test" / "interval=1d" / "symbol=AAA";
  std::filesystem::create_directories(dir);
  duckdb_database db;
  duckdb_connection con;
  if (duckdb_open(nullptr, &db) == DuckDBError || duckdb_connect(db, &con) == DuckDBError) {
    std::fprintf(stderr, "cannot open DuckDB\n");
    return 1;
  }
  const std::string price = "(100 + 10 * sin(d / 6.0))";
  const std::string select =
    "SELECT TIMESTAMPTZ '2020-01-01 00:00:00+00' + to_days(d::INTEGER) AS ts, 'close' AS stamp, (" + price +
    ")::DECIMAL(18,4) AS open, (" + price + " + 2)::DECIMAL(18,4) AS high, (" + price +
    " - 2)::DECIMAL(18,4) AS low, (" + price +
    " + cos(d / 3.0))::DECIMAL(18,4) AS close, 1000::DECIMAL(24,4) AS volume, "
    "TIMESTAMPTZ '2026-01-01 00:00:00+00' AS fetched_at FROM range(0, 120) t(d)";
  const auto path = (dir / "bars.parquet").generic_string();
  const bool ok = exec(con, "SET TimeZone = 'UTC'") &&
    exec(con, "COPY (" + select + " ORDER BY ts) TO '" + path + "' (FORMAT parquet)");
  duckdb_disconnect(&con);
  duckdb_close(&db);
  return ok ? 0 : 1;
}
