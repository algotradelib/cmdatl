# The backtest driver of one program shape, built by `cmdatl strategy backtest` in its cache from the program's
# `atlc build` output, the main and the replay module cmdatl carries; never built from a checkout.
cmake_minimum_required(VERSION 3.20)
project(cmdatl_backtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only the algotradelib package cmdatl itself was built against, never another one CMake could find.
if(NOT CMDATL_ALGOTRADELIB_DIR)
  message(FATAL_ERROR "CMDATL_ALGOTRADELIB_DIR must name the algotradelib package cmdatl was built against")
endif()
set(algotradelib_DIR "${CMDATL_ALGOTRADELIB_DIR}" CACHE PATH "The algotradelib package cmdatl was built against" FORCE)
# The package's dependencies are found first in the package's own prefix (<prefix>/lib*/cmake/algotradelib), as the run
# driver finds them.
get_filename_component(cmdatl_package_name "${CMDATL_ALGOTRADELIB_DIR}" NAME)
get_filename_component(cmdatl_package_cmake "${CMDATL_ALGOTRADELIB_DIR}/.." ABSOLUTE)
get_filename_component(cmdatl_package_cmake_name "${cmdatl_package_cmake}" NAME)
get_filename_component(cmdatl_package_lib "${cmdatl_package_cmake}/.." ABSOLUTE)
get_filename_component(cmdatl_package_lib_name "${cmdatl_package_lib}" NAME)
if(cmdatl_package_name STREQUAL "algotradelib" AND cmdatl_package_cmake_name STREQUAL "cmake" AND cmdatl_package_lib_name MATCHES "^lib")
  get_filename_component(cmdatl_package_prefix "${cmdatl_package_lib}/.." ABSOLUTE)
  list(PREPEND CMAKE_PREFIX_PATH "${cmdatl_package_prefix}")
endif()
find_package(algotradelib CONFIG REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
get_filename_component(cmdatl_found_package "${algotradelib_DIR}" REALPATH)
get_filename_component(cmdatl_expected_package "${CMDATL_ALGOTRADELIB_DIR}" REALPATH)
if(NOT cmdatl_found_package STREQUAL cmdatl_expected_package)
  message(FATAL_ERROR "algotradelib was found in ${algotradelib_DIR}, not in ${CMDATL_ALGOTRADELIB_DIR}")
endif()

add_executable(driver main.cpp)
target_include_directories(driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(driver PRIVATE algotradelib::algotradelib)
# Floating-point contraction off: Clang by the runtime's pragmas, GCC by a flag, as atlc compiles its strategies.
target_compile_options(driver PRIVATE $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)

# A store backtest: DuckDB's prebuilt C library, from CMDATL_DUCKDB_DIR alone when cmdatl names it (`build.duckdb`), else
# DUCKDB_ROOT, then the system's paths; the executable finds the library by an rpath to its directory.
# A broker folder's history: its adapter is copied into this source directory as adapter/, and named by
# CMDATL_DRIVER_ADAPTER (a folder's name, checked by cmdatl); the fetch writes the store, so DuckDB is linked too.
if(CMDATL_DRIVER_ADAPTER)
  if(NOT CMDATL_DRIVER_ADAPTER MATCHES "^[a-z][a-z0-9-]*$")
    message(FATAL_ERROR "CMDATL_DRIVER_ADAPTER must be a broker folder's name, not ${CMDATL_DRIVER_ADAPTER}")
  endif()
  target_compile_definitions(driver PRIVATE CMDATL_FETCH_ADAPTER=1)
endif()

if(CMDATL_STORE)
  if(CMDATL_DUCKDB_DIR)
    set(cmdatl_duckdb_hints PATHS "${CMDATL_DUCKDB_DIR}" PATH_SUFFIXES include lib NO_DEFAULT_PATH)
    set(cmdatl_duckdb_where "in ${CMDATL_DUCKDB_DIR}, the settings' build.duckdb")
  else()
    set(cmdatl_duckdb_hints HINTS ENV DUCKDB_ROOT PATH_SUFFIXES include lib)
    set(cmdatl_duckdb_where "in DUCKDB_ROOT or the system's paths: set build.duckdb, or DUCKDB_ROOT, to the directory of DuckDB's C library")
  endif()
  find_path(CMDATL_DUCKDB_INCLUDE duckdb.h ${cmdatl_duckdb_hints})
  find_library(CMDATL_DUCKDB_LIBRARY duckdb ${cmdatl_duckdb_hints})
  if(NOT CMDATL_DUCKDB_INCLUDE OR NOT CMDATL_DUCKDB_LIBRARY)
    message(FATAL_ERROR "no DuckDB (duckdb.h and its library) ${cmdatl_duckdb_where}")
  endif()
  add_library(cmdatl_duckdb SHARED IMPORTED)
  set_target_properties(cmdatl_duckdb PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMDATL_DUCKDB_INCLUDE}"
    IMPORTED_LOCATION "${CMDATL_DUCKDB_LIBRARY}"
  )
  get_filename_component(cmdatl_duckdb_dir "${CMDATL_DUCKDB_LIBRARY}" DIRECTORY)
  set_target_properties(driver PROPERTIES BUILD_RPATH "${cmdatl_duckdb_dir}")
  target_link_libraries(driver PRIVATE cmdatl_duckdb)
  target_compile_definitions(driver PRIVATE CMDATL_DUCKDB=1)
endif()
