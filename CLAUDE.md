# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Goal

Production-grade, full-depth L2 order book engine in C++20 ingesting NASDAQ TotalView-ITCH 5.0 binary market data. Target: p99 < 500 ns parse→BBO latency, 5M+ msgs/sec throughput, zero heap allocation on the hot path.

## Build System

Requires CMake 3.20+, GCC 12+/Clang 15+, Ninja. Dependencies (Google Benchmark v1.8.3, GoogleTest v1.14.0) are fetched via `FetchContent`.

```bash
# Configure and build (macOS: use Unix Makefiles; Linux: add -G Ninja)
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.logicalcpu)

# Run all tests
ctest --test-dir build -V

# Run a single test binary
./build/tests --gtest_filter=TestSuiteName.TestName

# Run benchmarks (outputs JSON for tracking)
./build/bench_book --benchmark_format=json > bench/results/$(date +%Y-%m-%d).json

# Run full replay
./build/orderbook --replay data/sample.bin

# perf profiling (root required)
sudo perf stat -e cycles,L1-dcache-load-misses,branch-misses ./build/orderbook --replay data/sample.bin
```

Release compile flags: `-O3 -march=native -fno-exceptions -fno-rtti`.

## Architecture

Four layers connected by lock-free SPSC queues:

```
ITCHFileReader → ITCHParser → SPSCQueue → OrderBook → Analytics
  (parser thread, core 0)       (book thread, core 1)   (core 2)
```

**Feed ingestion** (`include/transport/`, `src/transport/`): `ITCHFileReader` reads the Nasdaq length-prefixed binary stream (`[2-byte BE length][message bytes]...`). The parser `reinterpret_cast`s directly onto the raw buffer — no byte copies.

**SPSC queue** (`include/transport/spsc_queue.hpp`): Power-of-2 capacity ring buffer. `head_` and `tail_` atomics are each `alignas(64)` on separate cache lines to eliminate false sharing. No heap allocation after construction.

**Order book core** (`include/book/`, `src/book/`): The central design decision is a **contiguous `std::array<PriceLevel, 8192>`** indexed by `price - ref_price`, not `std::map`. The active ~500-tick range fits in L1D cache (32 KB). Bids index as `ref_price - price` (descending), asks as `price - ref_price` (ascending).

- `PriceLevel` is `alignas(64)` — one cache line per level, intrusive FIFO linked list of `Order*`
- `OrderPool` / `std::pmr::monotonic_buffer_resource` pre-allocates orders — zero `new`/`delete` on hot path
- `order_map_: unordered_map<uint64_t, Order*>` for O(1) lookup by `order_ref_num`
- `best_bid_` / `best_ask_` are direct pointers into the array, updated on every change — BBO is a pointer dereference, not a scan

All book operations (add/cancel/delete/replace/execute) are O(1).

**Benchmarking** (`include/bench/rdtsc.hpp`): Uses `rdtsc`/`rdtscp` (not `clock_gettime`) for sub-nanosecond timing. Calibrate TSC frequency against `CLOCK_MONOTONIC`. Record p50/p99/p999 histograms.

## ITCH 5.0 Protocol Details

- All fields are big-endian — use `__builtin_bswap16/32/64`
- Timestamps are 6-byte (48-bit) nanoseconds since midnight
- Prices are fixed-point integers: divide by 10000 for dollars
- Message types handled: `S` (System Event), `A`/`F` (Add Order), `E` (Execute), `C` (Execute w/ Price), `X` (Cancel), `D` (Delete), `U` (Replace), `P` (Trade)
- Hidden/non-displayed orders (`AddOrderMsg` display flag) must be excluded from the visible book
- Only process messages between System Event `O` (open) and `C` (close)

## Correctness Validation

Validate against LOBSTER ground-truth snapshots (lobsterdata.com free sample data). LOBSTER CSV columns: `timestamp, ask_price_1, ask_size_1, bid_price_1, bid_size_1, ...` (10 levels deep). Target: **0 mismatches** over a full trading day.

Common failure modes: forgetting `Replace` (`U`) messages, LOBSTER timestamps in microseconds vs ITCH nanoseconds, ignoring hidden orders, not handling `Execute with Price` (`C`) differently from `E`.

## Performance Targets

| Metric | Target |
|--------|--------|
| p99 latency | < 500 ns |
| Throughput | > 5M msgs/sec |
| L1D cache miss rate | < 2% |
| Branch mispredict rate | < 0.5% |
| Hot-path heap allocations | 0 |

Track regressions in `BENCHMARKS.md` with each commit.

## Thread Model

| Thread | Core | Role |
|--------|------|------|
| Parser | 0 | Read file/socket, parse ITCH, push to SPSC |
| Book | 1 | Pop SPSC, update book, publish BBO snapshot |
| Analytics | 2 | Read-only snapshots, compute OFI/mid/spread/VWAP |

Set affinity via `pthread_setaffinity_np`. On Linux, use `SCHED_FIFO` (requires root) to prevent preemption.

## Test Invariants

After every book operation assert:
- `best_bid >= all other bid prices`
- `best_ask <= all other ask prices`  
- `best_bid < best_ask` (no crossed book)
- `total_qty[level] == sum of all order quantities at that level`
- `order_map.size() == total outstanding orders`
