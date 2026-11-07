# The run driver of one program shape, built by `cmdatl run` in its cache from the sources cmdatl carries and the
# program's `atlc build` output; never built from a checkout.
cmake_minimum_required(VERSION 3.20)
project(cmdatl_driver LANGUAGES CXX)

# Only the algotradelib package cmdatl itself was built against, never another one CMake could find: the run's journal
# and recovery must be the library cmdatl reads.
if(NOT CMDATL_ALGOTRADELIB_DIR)
  message(FATAL_ERROR "CMDATL_ALGOTRADELIB_DIR must name the algotradelib package cmdatl was built against")
endif()
set(algotradelib_DIR "${CMDATL_ALGOTRADELIB_DIR}" CACHE PATH "The algotradelib package cmdatl was built against" FORCE)
# The package's dependencies (Boost, Eigen) are found first in the package's own prefix (<prefix>/lib/cmake/algotradelib),
# where an installed package carries them; cmdatl's build strips CMAKE_PREFIX_PATH from this build's environment, so
# without this a prefix holding them all would not be searched at all.
# Only an installed layout, <prefix>/lib*/cmake/algotradelib: a build tree's directory has no prefix above it to search.
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
target_compile_features(driver PRIVATE cxx_std_17)
set_target_properties(driver PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(driver SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# toml++ is used header-only, as cmdatl itself uses it.
target_compile_definitions(driver PRIVATE TOML_HEADER_ONLY=1)
target_link_libraries(driver PRIVATE algotradelib::algotradelib)
# A run on a broker's account: the driver compiles that broker's client (`kis`), never named by a run's arguments.
if(CMDATL_DRIVER_BROKER STREQUAL "kis")
  target_compile_definitions(driver PRIVATE CMDATL_DRIVER_KIS=1)
elseif(CMDATL_DRIVER_BROKER)
  message(FATAL_ERROR "CMDATL_DRIVER_BROKER must be kis or empty, not ${CMDATL_DRIVER_BROKER}")
endif()
# As atlc compiles its exported strategies: no floating-point contraction on GCC.
target_compile_options(driver PRIVATE $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
target_link_options(driver PRIVATE
  $<$<PLATFORM_ID:Darwin>:LINKER:-dead_strip_dylibs>
  $<$<PLATFORM_ID:Linux>:LINKER:--as-needed>
)
