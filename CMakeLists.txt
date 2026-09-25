cmake_minimum_required(VERSION 3.16)
project(lob_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

set(LOB_WARNINGS -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_executable(lob_tests
  tests/test_main.cpp
  tests/test_ring.cpp
  tests/test_pool_map_hist.cpp
  tests/test_engine.cpp)
target_link_libraries(lob_tests PRIVATE lob Threads::Threads)
target_compile_options(lob_tests PRIVATE ${LOB_WARNINGS})

enable_testing()
add_test(NAME unit COMMAND lob_tests)
