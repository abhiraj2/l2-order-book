cmake_minimum_required(VERSION 3.20)
project(l2_order_book CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(benchmark googletest)

# ---------------------------------------------------------------------------
# Compile options
# ---------------------------------------------------------------------------
add_compile_options(
    -O3 -march=native
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
)

# macOS: suppress deprecation noise from third-party headers
if(APPLE)
    add_compile_options(-Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(orderbook_lib
    src/itch/parser.cpp
    src/book/order_book.cpp
    src/book/price_level.cpp
    src/book/lobster_validator.cpp
    src/transport/pcap_replay.cpp
)
target_include_directories(orderbook_lib PUBLIC include)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(orderbook src/main.cpp)
target_link_libraries(orderbook PRIVATE orderbook_lib)

# ---------------------------------------------------------------------------
# Benchmark binaries
# ---------------------------------------------------------------------------
add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE orderbook_lib benchmark::benchmark)

# Standalone latency harness — uses rdtsc.hpp histogram, no benchmark dep.
add_executable(latency_bench bench/latency_harness.cpp)
target_link_libraries(latency_bench PRIVATE orderbook_lib)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(tests
    test/test_parser.cpp
    test/test_spsc.cpp
    test/test_book.cpp
    test/test_lobster_validate.cpp
)
target_link_libraries(tests PRIVATE orderbook_lib GTest::gtest_main)
add_test(NAME unit_tests COMMAND tests)
