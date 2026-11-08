# What backtests and runs need

Backtests and runs compile each strategy at run time, so they also need CMake and a C++17 compiler, the algotradelib cmdatl was built against, and `atlc`, the atls compiler, on `PATH` or named by `CMDATL_ATLC` ([its install](https://github.com/algotradelib/atlc#install): `cargo install --git https://github.com/algotradelib/atlc`, with a Rust toolchain).
Those builds find algotradelib where cmdatl was built against it, but not through `CMAKE_PREFIX_PATH`: of the variables that locate packages they pass on only `OPENSSL_ROOT_DIR`, `BOOST_ROOT`, `Boost_ROOT` and `Eigen3_ROOT` (and `DUCKDB_ROOT` for a store).
So keep `OPENSSL_ROOT_DIR` exported on macOS for every backtest and run as for the build in the [README's Install](../README.md#install), and export the others when Boost or Eigen live outside the library's prefix and the system's paths.
atlc only translates a program to C++; cmdatl builds that once per program, keeps the build, and replays it.
A backtest that reads a Parquet store also needs DuckDB's C library, named by the `[build]` table's `duckdb` or by `DUCKDB_ROOT`.
`cmdatl strategy reference` needs atlc only; `cmdatl strategy new` needs neither.
