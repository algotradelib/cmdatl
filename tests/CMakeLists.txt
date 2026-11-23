
find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
  if(NOT CMDATL_FETCH_DEPS)
    message(FATAL_ERROR "Catch2 3 is needed for the tests: install it, or configure with -DCMDATL_FETCH_DEPS=ON")
  endif()
  FetchContent_Declare(catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.8.1 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(catch2)
endif()

# Threads for the evaluations the replay module's tests and the store's backtest driver run.
find_package(Threads REQUIRED)
# A stand-in for atlc whose behaviour each test writes in a script.
add_executable(fake-atlc fake-atlc.cpp)
# Holds an account's lock, as another cmdatl process would.
add_executable(hold-lock hold-lock.cpp)
add_executable(test-cli
  test-cli.cpp test-process.cpp test-strategy.cpp test-acceptance.cpp test-operator.cpp
  test-account.cpp test-paper.cpp test-mock-desk.cpp test-orders.cpp
  test-order-process.cpp test-run-start.cpp test-guides.cpp test-spec.cpp test-auth.cpp
  test-user-brokers.cpp test-broker-reads.cpp
)
target_link_libraries(test-cli PRIVATE cmdatl_builtins Catch2::Catch2WithMain)
# forkpty, for the terminal cases of test-auth.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(test-cli PRIVATE util)
endif()
add_dependencies(test-cli cmdatl fake-atlc hold-lock)
# The documentation checks read README.md and docs/ from the source tree; the process tests run cmdatl itself.
target_compile_definitions(test-cli PRIVATE
  CMDATL_RUN_TEST_HOOKS
  CMDATL_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  CMDATL_BINARY="$<TARGET_FILE:cmdatl>"
  CMDATL_FAKE_ATLC="$<TARGET_FILE:fake-atlc>"
  CMDATL_HOLD_LOCK="$<TARGET_FILE:hold-lock>"
)
add_test(NAME test-cli COMMAND test-cli "~[acceptance]")
# The acceptance cases compare replays with figures computed in the test: no floating-point contraction on GCC, as
# the drivers are built.
target_compile_options(test-cli PRIVATE $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
# The store backtest's acceptance case: DuckDB's C library, whose directory CMDATL_TEST_DUCKDB names (a cache
# variable or the environment), writes its store; without it the case is skipped, and says so.
if(NOT CMDATL_TEST_DUCKDB AND DEFINED ENV{CMDATL_TEST_DUCKDB})
  set(CMDATL_TEST_DUCKDB "$ENV{CMDATL_TEST_DUCKDB}")
endif()
if(CMDATL_TEST_DUCKDB)
  add_executable(make-store make-store.cpp)
  target_compile_features(make-store PRIVATE cxx_std_17)
  target_include_directories(make-store SYSTEM PRIVATE ${CMDATL_TEST_DUCKDB})
  find_library(CMDATL_TEST_DUCKDB_LIBRARY duckdb PATHS ${CMDATL_TEST_DUCKDB} NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(make-store PRIVATE ${CMDATL_TEST_DUCKDB_LIBRARY})
  set_target_properties(make-store PROPERTIES BUILD_RPATH ${CMDATL_TEST_DUCKDB})
  add_dependencies(test-cli make-store)
  target_compile_definitions(test-cli PRIVATE
    CMDATL_TEST_DUCKDB="${CMDATL_TEST_DUCKDB}"
    CMDATL_MAKE_STORE="$<TARGET_FILE:make-store>"
  )
  # The replay module's store loader: its own executable, whose one translation unit defines CMDATL_DUCKDB, so no
  # inline definition without the store is mixed with it.
  add_executable(test-replay-store test-replay-store.cpp test-store-write.cpp)
  target_link_libraries(test-replay-store PRIVATE algotradelib::algotradelib Catch2::Catch2WithMain ${CMDATL_TEST_DUCKDB_LIBRARY})
  target_include_directories(test-replay-store PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_include_directories(test-replay-store SYSTEM PRIVATE ${CMDATL_TEST_DUCKDB})
  target_compile_definitions(test-replay-store PRIVATE CMDATL_DUCKDB=1)
  target_compile_options(test-replay-store PRIVATE $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
  set_target_properties(test-replay-store PROPERTIES BUILD_RPATH ${CMDATL_TEST_DUCKDB})
  add_test(NAME test-replay-store COMMAND test-replay-store)
  # The backtest driver that fetches through a broker folder's adapter (here a fixture data vendor), compiled as a
  # backtest's build compiles it.
  add_executable(backtest-adapter-vendor ${PROJECT_SOURCE_DIR}/driver/backtest/main.cpp)
  target_include_directories(backtest-adapter-vendor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/no-data-facts ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/adapters/vendor
  )
  target_include_directories(backtest-adapter-vendor SYSTEM PRIVATE ${CMDATL_TEST_DUCKDB})
  target_compile_definitions(backtest-adapter-vendor PRIVATE TOML_HEADER_ONLY=1 CMDATL_DUCKDB=1 CMDATL_FETCH_ADAPTER=1)
  target_link_libraries(backtest-adapter-vendor PRIVATE cmdatl_core Threads::Threads ${CMDATL_TEST_DUCKDB_LIBRARY})
  set_target_properties(backtest-adapter-vendor PROPERTIES BUILD_RPATH ${CMDATL_TEST_DUCKDB})
endif()
# The replay module with hand-written programs and files written by the test, as the drivers build it: no
# floating-point contraction on GCC.
add_executable(test-replay test-replay.cpp test-evaluate.cpp)
target_link_libraries(test-replay PRIVATE cmdatl_core Catch2::Catch2WithMain Threads::Threads)
target_compile_options(test-replay PRIVATE $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
add_test(NAME test-replay COMMAND test-replay)
# An outer bound for a stdin reader that ignores a stop: its case keeps the pipe open and would wait forever.
set_tests_properties(test-replay PROPERTIES TIMEOUT 600)
# The fault tests: their own executable, whose one translation unit defines CMDATL_FAULT_HOOKS and gives the
# system calls of the account's state files their stand-ins, so no inline definition is mixed with them.
add_executable(test-faults test-faults.cpp)
target_link_libraries(test-faults PRIVATE cmdatl_builtins Catch2::Catch2WithMain)
add_dependencies(test-faults cmdatl fake-atlc hold-lock)
target_compile_definitions(test-faults PRIVATE
  CMDATL_FAULT_HOOKS
  CMDATL_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  CMDATL_BINARY="$<TARGET_FILE:cmdatl>"
  CMDATL_FAKE_ATLC="$<TARGET_FILE:fake-atlc>"
  CMDATL_HOLD_LOCK="$<TARGET_FILE:hold-lock>"
)
# On Linux the journal's own fsync, compiled into the test from the header-only library, can be failed too.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(test-faults PRIVATE LINKER:--wrap=fsync)
  target_compile_definitions(test-faults PRIVATE CMDATL_WRAP_FSYNC)
endif()
add_test(NAME test-faults COMMAND test-faults)
# The run driver in process, with a hand-written program and loader: no atlc header.
# The run driver against a program header without the data facts (from an atlc before data {}): it must compile.
add_executable(driver-no-data-facts ${PROJECT_SOURCE_DIR}/driver/main.cpp)
target_include_directories(driver-no-data-facts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/no-data-facts)
target_compile_definitions(driver-no-data-facts PRIVATE TOML_HEADER_ONLY=1)
target_link_libraries(driver-no-data-facts PRIVATE cmdatl_core)
# A KIS account's driver, compiled as a run's build compiles it, with the broker.hpp the build writes (checked by a
# test); it is never run here (it would call the broker).
add_executable(driver-kis ${PROJECT_SOURCE_DIR}/driver/main.cpp)
target_include_directories(driver-kis PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/no-data-facts ${CMAKE_CURRENT_SOURCE_DIR}/builtin/kis
)
target_compile_definitions(driver-kis PRIVATE TOML_HEADER_ONLY=1 CMDATL_DRIVER_BUILTIN=1 CMDATL_DRIVER_BROKER="kis")
target_link_libraries(driver-kis PRIVATE cmdatl_core)
# An Alpaca account's driver, as a run's build compiles it: run by a test only up to its refusal (Alpaca states no
# buying power, so no run starts without limits.budget), which comes before any login is read or the broker called.
add_executable(driver-alpaca ${PROJECT_SOURCE_DIR}/driver/main.cpp)
target_include_directories(driver-alpaca PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/no-data-facts ${CMAKE_CURRENT_SOURCE_DIR}/builtin/alpaca
)
target_compile_definitions(driver-alpaca PRIVATE
  TOML_HEADER_ONLY=1 CMDATL_DRIVER_BUILTIN=1 CMDATL_DRIVER_BROKER="alpaca"
)
target_link_libraries(driver-alpaca PRIVATE cmdatl_core)
# Each built-in broker's backtest driver, compiled as a backtest fetching its history compiles it (never run here: it
# would call the broker), with the broker.hpp the build writes.
foreach(broker IN ITEMS kis kiwoom tossinvest alpaca)
  add_executable(backtest-builtin-${broker} ${PROJECT_SOURCE_DIR}/driver/backtest/main.cpp)
  target_include_directories(backtest-builtin-${broker} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/no-data-facts ${CMAKE_CURRENT_SOURCE_DIR}/builtin/${broker}
  )
  target_compile_definitions(backtest-builtin-${broker} PRIVATE CMDATL_FETCH_BUILTIN=1 CMDATL_DRIVER_BROKER="${broker}")
  target_link_libraries(backtest-builtin-${broker} PRIVATE cmdatl_core Threads::Threads)
endforeach()
# A broker folder's account driver, compiled here as a run's build compiles it, against a fixture adapter over the
# library's mock broker and one whose client states none of what a run reads at its start (it must compile, and
# refuse at start).
# Each is built from its fixture folder, which a test's operator file names.
foreach(adapter IN ITEMS mockbroker limited)
  file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/adapters/${adapter}/adapter" folder)
  add_executable(driver-adapter-${adapter} ${PROJECT_SOURCE_DIR}/driver/main.cpp)
  target_include_directories(driver-adapter-${adapter} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/no-data-facts ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/adapters/${adapter}
  )
  target_compile_definitions(driver-adapter-${adapter} PRIVATE
    TOML_HEADER_ONLY=1 CMDATL_DRIVER_USER=1 CMDATL_ADAPTER_NAME="${adapter}" CMDATL_ADAPTER_FOLDER="${folder}"
  )
  target_link_libraries(driver-adapter-${adapter} PRIVATE cmdatl_core)
endforeach()
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/adapters/mockbroker/adapter" folder_mockbroker)
add_executable(test-runs test-runs.cpp test-account-run.cpp test-owed-recovery.cpp)
target_include_directories(test-runs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/adapters/mockbroker)
target_link_libraries(test-runs PRIVATE cmdatl_builtins Catch2::Catch2WithMain)
add_dependencies(
  test-runs cmdatl fake-atlc hold-lock driver-adapter-limited driver-adapter-mockbroker driver-alpaca
)
target_compile_definitions(test-runs PRIVATE
  CMDATL_RUN_TEST_HOOKS
  CMDATL_ADAPTER_NAME="mockbroker"
  CMDATL_ADAPTER_FOLDER="${folder_mockbroker}"
  CMDATL_DRIVER_LIMITED="$<TARGET_FILE:driver-adapter-limited>"
  CMDATL_DRIVER_MOCKBROKER="$<TARGET_FILE:driver-adapter-mockbroker>"
  CMDATL_DRIVER_ALPACA="$<TARGET_FILE:driver-alpaca>"
  CMDATL_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  CMDATL_BINARY="$<TARGET_FILE:cmdatl>"
  CMDATL_FAKE_ATLC="$<TARGET_FILE:fake-atlc>"
  CMDATL_HOLD_LOCK="$<TARGET_FILE:hold-lock>"
)
add_test(NAME test-runs COMMAND test-runs)
if(NOT WIN32)
  # The whole command line on KIS's shipped setup, its clients pointed at loopback servers: never installed, and the
  # one test program that compiles the KIS client's transport replacement.
  add_executable(kis-command kis-command.cpp)
  target_link_libraries(kis-command PRIVATE cmdatl_builtins)
  # Runs every command as a kis-command child against two loopback servers (demo) or one (real).
  add_executable(test-kis test-kis-commands.cpp)
  target_link_libraries(test-kis PRIVATE cmdatl_builtins Catch2::Catch2WithMain)
  add_dependencies(test-kis kis-command)
  # support.hpp names cmdatl and the fake atlc for helpers this program does not call.
  target_compile_definitions(test-kis PRIVATE
    CMDATL_KIS_COMMAND="$<TARGET_FILE:kis-command>"
    CMDATL_BINARY="$<TARGET_FILE:cmdatl>"
    CMDATL_FAKE_ATLC="$<TARGET_FILE:fake-atlc>"
  )
  add_test(NAME test-kis COMMAND test-kis)
endif()
# Against a real atlc and algotradelib, named by CMDATL_TEST_ATLC and CMDATL_TEST_PREFIX; skipped, and reported
# as skipped, without them.
add_test(NAME test-acceptance COMMAND test-cli "[acceptance]")
set_tests_properties(test-acceptance PROPERTIES SKIP_RETURN_CODE 4 TIMEOUT 3600)
# Every header in a module, including only the modules check-layout.cpp's table allows.
add_executable(check-layout check-layout.cpp)
add_test(NAME layout COMMAND check-layout ${PROJECT_SOURCE_DIR})
add_test(NAME layout-lexer COMMAND check-layout --self-test)
