# L2 Order Book Engine

Full-depth Level 2 order book engine in C++20 consuming **NASDAQ TotalView-ITCH 5.0** binary market data. Sub-microsecond book-update latency with zero heap allocation on the hot path.

---

## What it does

- Parses the full NASDAQ TotalView-ITCH 5.0 binary feed (all order lifecycle message types)
- Maintains a full-depth bid/ask book with O(1) add, cancel, execute, and replace
- Publishes best-bid/offer (BBO) after every book event
- Computes mid-price, spread, order-flow imbalance, VWAP, and visible depth
- Validates book state against LOBSTER ground-truth snapshots (0 mismatches target)
- Replays historical ITCH binary files through a two-threaded parse → book pipeline

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                   Feed Ingestion Layer                    │
│  ITCHFileReader — length-prefixed binary stream          │
│  ITCHParser     — zero-copy parse onto raw buffer        │
│  ParsedMessage  — std::variant tagged union              │
└───────────────────────┬──────────────────────────────────┘
                        │  lock-free SPSC ring buffer
┌───────────────────────▼──────────────────────────────────┐
│                   Order Book Core                         │
│  Contiguous price-level array (no std::map)              │
│  Pre-allocated order pool — zero heap alloc after init   │
│  O(1) lookup by order reference number                   │
│  Operations: Add · Cancel · Execute · Replace  all O(1)  │
└───────────────────────┬──────────────────────────────────┘
                        │  BBO snapshot
┌───────────────────────▼──────────────────────────────────┐
│               Analytics Layer                             │
│  Mid-price · spread · OFI · VWAP · visible depth         │
└──────────────────────────────────────────────────────────┘
```

### Thread model

| Thread    | Core | Responsibility                              |
|-----------|------|---------------------------------------------|
| Parser    | 0    | Read file/socket, parse ITCH, push to SPSC  |
| Book      | 1    | Pop SPSC, update book, publish BBO snapshot |
| Analytics | 2    | Read-only snapshots, compute statistics     |

---

## Building

**Requirements:** CMake 3.20+, Clang 15+ / GCC 12+ (C++20), macOS or Linux.  
Dependencies (Google Benchmark v1.8.3, GoogleTest v1.14.0) are fetched automatically via CMake `FetchContent`.

```bash
# Configure
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release   # macOS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release              # Linux

# Build everything
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build -V

# Run a single suite
./build/tests --gtest_filter=BookTest.*

# Full ITCH replay
./build/orderbook --replay data/sample.bin

# Google Benchmark suite
./build/bench_book --benchmark_format=json \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true

# Tail-latency harness (p50/p99/p999)
./build/latency_bench
```

---

## Tests

39 tests across 5 suites, all passing (1 skipped — requires external LOBSTER data):

| Suite | Tests | Covers |
|-------|------:|--------|
| `ParserTest` | 11 | All 9 ITCH message types; byte-swap correctness; timestamp round-trip; truncated/null/unknown inputs |
| `SPSCQueueTest` | 6 | FIFO order; full-queue backpressure; wrap-around; 10M-message two-thread stress |
| `BookTest` | 20 | Add/cancel/execute/replace; BBO tracking; session gating; halt drain; 50k-op random stress with no-crossed-book assertion |
| `LobsterValidateTest` | 2 | Synthetic ITCH + LOBSTER CSV round-trip (0 mismatches); real-data path |
| `AnalyticsTest` | 1 | Spread, mid-price, OFI, VWAP, visible depth |

---

## Performance

Measured on macOS ARM64 (`-O3 -march=native`), Google Benchmark v1.8.3, 3 repetitions.
See [`BENCHMARKS.md`](BENCHMARKS.md) for full results and regression-tracking instructions.

| Operation | Mean (ns) |
|-----------|----------:|
| BBO query | **0.98** |
| Parse message (AddOrder) | **1.23** |
| SPSC queue round-trip | **2.44** |
| Cancel partial | **2.99** |
| Add / Delete / Replace | **~26** |
| Mixed workload (realistic) | **20.2** |

**p99 ≤ 83 ns** across all book operations (500 ns target).

---

## ITCH 5.0 Message Types

| Code | Message | Effect |
|------|---------|--------|
| `S` | System Event | Session open / close / halt |
| `A` | Add Order | New resting order |
| `F` | Add Order (MPID) | New resting order with participant ID |
| `E` | Order Executed | Partial or full fill |
| `C` | Execute with Price | Fill at auction price |
| `X` | Order Cancel | Partial cancellation |
| `D` | Order Delete | Full removal |
| `U` | Order Replace | Atomic cancel + re-add |
| `P` | Trade | Informational — no book change |

---

## Project Structure

```
include/itch/       ITCH message structs and parser interface
include/book/       OrderBook, PriceLevel, BBO, Analytics, LobsterValidator
include/transport/  SPSC queue, ITCH file reader
include/bench/      Nanosecond timer and latency histogram
src/                Implementations
test/               Unit and integration tests
bench/              Google Benchmark suite, latency harness, result snapshots
```
