# The run driver of one program shape, built by `cmdatl run start` in its cache from the sources cmdatl carries and the
# program's `atlc build` output; never built from a checkout.
cmake_minimum_required(VERSION 3.20)
project(cmdatl_driver LANGUAGES CXX)

find_package(algotradelib CONFIG REQUIRED)

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
