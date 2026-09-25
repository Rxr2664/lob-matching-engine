cmake_minimum_required(VERSION 3.16)
project(lob_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_NATIVE "Build with -march=native" OFF)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

set(LOB_WARNINGS -Wall -Wextra -Wpedantic)
if(LOB_NATIVE)
  list(APPEND LOB_WARNINGS -march=native)
endif()

find_package(Threads REQUIRED)

add_executable(lob_bench bench/bench_main.cpp)
target_link_libraries(lob_bench PRIVATE lob Threads::Threads)
target_compile_options(lob_bench PRIVATE ${LOB_WARNINGS})

add_executable(lob_replay tools/replay_main.cpp)
target_link_libraries(lob_replay PRIVATE lob)
target_compile_options(lob_replay PRIVATE ${LOB_WARNINGS})
# Capped below -O2 regardless of CMAKE_BUILD_TYPE: this is a printf-per-event
# demo tool with no performance requirement, and on MinGW-w64 GCC 16.1.0
# (Windows) building it at -O2/-O3 reliably segfaults (100% repro on the
# bundled example script) while -O0/-O1 do not. Same source/headers as
# lob_bench, which never crashes at -O3 -- this is a toolchain codegen issue,
# not a logic bug. See docs/BENCHMARKING.md. The trailing -O1 here wins on
# the command line over the -O3 CMake adds for Release. Re-test at -O2 next
# time the toolchain is upgraded; drop this cap if it no longer repros.
target_compile_options(lob_replay PRIVATE -O1)

add_executable(lob_tests
  tests/test_main.cpp
  tests/test_ring.cpp
  tests/test_pool_map_hist.cpp
  tests/test_engine.cpp
  tests/test_fuzz.cpp
  tests/test_no_alloc.cpp)
target_link_libraries(lob_tests PRIVATE lob Threads::Threads)
target_compile_options(lob_tests PRIVATE ${LOB_WARNINGS})

enable_testing()
add_test(NAME unit COMMAND lob_tests)
