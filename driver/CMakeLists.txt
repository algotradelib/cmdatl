# The run driver of one program shape, built by `cmdatl run start` in its cache from the sources cmdatl carries and the
# program's `atlc build` output; never built from a checkout.
cmake_minimum_required(VERSION 3.20)
project(cmdatl_driver LANGUAGES CXX)

# Only the algotradelib package cmdatl itself was built against, never another one CMake could find: the run's journal
# and recovery must be the library cmdatl reads.
if(NOT CMDATL_ALGOTRADELIB_DIR)
  message(FATAL_ERROR "CMDATL_ALGOTRADELIB_DIR must name the algotradelib package cmdatl was built against")
endif()
set(algotradelib_DIR "${CMDATL_ALGOTRADELIB_DIR}" CACHE PATH "The algotradelib package cmdatl was built against" FORCE)
find_package(algotradelib CONFIG REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
get_filename_component(cmdatl_found_package "${algotradelib_DIR}" REALPATH)
get_filename_component(cmdatl_expected_package "${CMDATL_ALGOTRADELIB_DIR}" REALPATH)
if(NOT cmdatl_found_package STREQUAL cmdatl_expected_package)
  message(FATAL_ERROR "algotradelib was found in ${algotradelib_DIR}, not in ${CMDATL_ALGOTRADELIB_DIR}")
endif()

add_executable(driver main.cpp)
target_compile_features(driver PRIVATE cxx_std_17)
set_target_properties(driver PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(driver SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# toml++ is used header-only, as cmdatl itself uses it.
target_compile_definitions(driver PRIVATE TOML_HEADER_ONLY=1)
target_link_libraries(driver PRIVATE algotradelib::algotradelib)
# As atlc compiles its exported strategies: no floating-point contraction on GCC.
target_compile_options(driver PRIVATE $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
target_link_options(driver PRIVATE
  $<$<PLATFORM_ID:Darwin>:LINKER:-dead_strip_dylibs>
  $<$<PLATFORM_ID:Linux>:LINKER:--as-needed>
)
