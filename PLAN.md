# L2 Order Book Engine — Project Plan

> **Goal:** Build a production-grade, full-depth L2 order book engine in C++20 that ingests NASDAQ ITCH 5.0 binary market data, reconstructs the order book in real time with sub-500 ns p99 latency, and produces a benchmarked, documented GitHub project suitable for HFT/quant developer interviews.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Repository Structure](#3-repository-structure)
4. [Week-by-Week Build Plan](#4-week-by-week-build-plan)
   - [Week 1 — ITCH 5.0 Parser + Feed Ingestion](#week-1--itch-50-parser--feed-ingestion)
   - [Week 2 — Order Book Core Data Structures](#week-2--order-book-core-data-structures)
   - [Week 3 — End-to-End Integration + Correctness Validation](#week-3--end-to-end-integration--correctness-validation)
   - [Week 4 — Latency Benchmarking + Optimisation](#week-4--latency-benchmarking--optimisation)
   - [Week 5 — Polish, Documentation + Resume Packaging](#week-5--polish-documentation--resume-packaging)
5. [Key Design Decisions](#5-key-design-decisions)
6. [Performance Targets](#6-performance-targets)
7. [Toolchain & Build Setup](#7-toolchain--build-setup)
8. [Data Sources](#8-data-sources)
9. [Testing Strategy](#9-testing-strategy)
10. [References](#10-references)
11. [Resume Bullet](#11-resume-bullet)

---

## 1. Project Overview

| Property        | Detail                                                       |
|-----------------|--------------------------------------------------------------|
| Language        | C++20                                                        |
| Protocol        | NASDAQ TotalView-ITCH 5.0 (binary, big-endian)               |
| Input           | PCAP file replay (Week 1–3) → live UDP multicast (Week 5+)   |
| Target latency  | p99 < 500 ns (parse → BBO update, end-to-end)                |
| Target throughput | 5 M+ messages/sec sustained                                |
| Validation      | LOBSTER ground-truth snapshot comparison                     |
| Build system    | CMake 3.20+, Ninja, GCC 12+ / Clang 15+                     |
| Duration        | 5 weeks, ~10–12 hours/week                                   |

### What this project demonstrates

- Lock-free inter-thread communication (SPSC ring buffer)
- Cache-line-aware data structure design (price-level array, `alignas(64)`)
- Zero heap allocation on hot path (slab/pool allocator, `std::pmr`)
- Binary protocol parsing with zero-copy (`reinterpret_cast`, packed structs)
- Rigorous latency measurement (`rdtsc`, histograms, p50/p99/p999)
- Production-grade profiling (perf, flamegraph, cache miss analysis)
- Market microstructure understanding (BBO, order flow imbalance, VWAP)

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     Feed Ingestion Layer                 │
│  PCAP replay / UDP multicast socket                      │
│  → ITCH 5.0 binary parser (zero-copy, packed structs)   │
│  → SPSC ring buffer (lock-free, mmap-backed)             │
└────────────────────────┬────────────────────────────────┘
                         │  (cache-line aligned messages)
┌────────────────────────▼────────────────────────────────┐
│                    Order Book Core                        │
│  Price-level array  (contiguous, alignas(64))            │
│  Intrusive FIFO list per level (slab-allocated orders)   │
│  Order map: unordered_map<order_id, Order*>  → O(1)      │
│  BBO tracker: direct best-bid / best-ask pointers        │
│  Operations: Add · Cancel · Modify · Execute  all O(1)   │
└────────────────────────┬────────────────────────────────┘
                         │  (read-only snapshot copies)
┌────────────────────────▼────────────────────────────────┐
│                   Analytics Layer                         │
│  Best Bid/Offer (BBO) stream                             │
│  Order flow imbalance  (bid_qty − ask_qty) / total_qty   │
│  Mid-price, spread, VWAP per symbol                      │
│  Configurable depth snapshots                            │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│              Benchmarking & Observability                 │
│  rdtsc latency histogram (p50 / p99 / p999)              │
│  Google Benchmark suite (add / cancel / modify / BBO)    │
│  perf stat → cache miss rate, branch-mispredict rate     │
│  Flamegraph (perf record → stackcollapse → flamegraph.pl)│
└─────────────────────────────────────────────────────────┘
```

### Thread model

| Thread          | Pinned core | Responsibility                                    |
|-----------------|-------------|---------------------------------------------------|
| Parser thread   | Core 0      | Read socket/PCAP, parse ITCH, write to SPSC       |
| Book thread     | Core 1      | Consume SPSC, update book, publish BBO snapshot   |
| Analytics thread| Core 2      | Read snapshots (read-only), compute derived stats  |

All inter-thread communication is via lock-free SPSC queues. No mutexes on the hot path.

---

## 3. Repository Structure

```
l2-order-book/
├── CMakeLists.txt
├── PLAN.md                        ← this file
├── README.md                      ← benchmark results, architecture, usage
│
├── include/
│   ├── itch/
│   │   ├── messages.hpp           ← packed ITCH message structs
│   │   └── parser.hpp             ← stateless binary parser
│   ├── book/
│   │   ├── order.hpp              ← Order struct, slab pool
│   │   ├── price_level.hpp        ← PriceLevel (intrusive list + qty)
│   │   ├── order_book.hpp         ← OrderBook (price array + order map)
│   │   └── bbo.hpp                ← BBO snapshot struct
│   ├── transport/
│   │   ├── spsc_queue.hpp         ← lock-free SPSC ring buffer
│   │   └── pcap_replay.hpp        ← PCAP file reader
│   └── bench/
│       └── rdtsc.hpp              ← rdtsc latency timer + histogram
│
├── src/
│   ├── main.cpp                   ← entry point, thread setup, CPU affinity
│   ├── itch/
│   │   └── parser.cpp
│   ├── book/
│   │   ├── order_book.cpp
│   │   └── price_level.cpp
│   └── transport/
│       └── pcap_replay.cpp
│
├── bench/
│   └── bench_book.cpp             ← Google Benchmark suite
│
├── test/
│   ├── test_parser.cpp            ← unit tests: known ITCH byte sequences
│   ├── test_book.cpp              ← unit tests: add/cancel/modify/execute
│   └── test_lobster_validate.cpp  ← integration: compare vs LOBSTER snapshots
│
├── scripts/
│   ├── run_perf.sh                ← perf record + stat + flamegraph generation
│   └── download_sample_data.sh   ← fetch free ITCH sample from Nasdaq FTP
│
└── data/
    └── .gitkeep                   ← PCAP/ITCH sample files go here (gitignored)
```

---

## 4. Week-by-Week Build Plan

---

### Week 1 — ITCH 5.0 Parser + Feed Ingestion

**Goal:** Parse all relevant ITCH 5.0 message types from a PCAP file into a lock-free ring buffer. Validate correctness with unit tests against known byte sequences.

**Estimated time:** ~10 hours

#### Step 1.1 — Read the spec (2 h)

Download the NASDAQ TotalView-ITCH 5.0 specification from nasdaqtrader.com.

Focus on these message types — ignore everything else for now:

| Type | Code | Description                        |
|------|------|------------------------------------|
| System Event     | `S` | Session open/close               |
| Add Order        | `A` | New resting order                |
| Add Order (MPID) | `F` | Same as A, with market participant |
| Execute Order    | `E` | Partial/full fill                |
| Execute w/ Price | `C` | Fill at non-reference price      |
| Cancel Order     | `X` | Partial cancellation             |
| Delete Order     | `D` | Full cancellation                |
| Replace Order    | `U` | Cancel + new order in one        |
| Trade (non-cross)| `P` | Executed trade (reference only)  |

#### Step 1.2 — ITCH message structs (2 h)

Define a packed struct per message type. All ITCH fields are big-endian — use `__builtin_bswap16` / `__builtin_bswap32` / `__builtin_bswap64` for field access. Do not copy bytes; use `reinterpret_cast` directly onto the raw buffer.

```cpp
// include/itch/messages.hpp
#pragma pack(push, 1)

struct AddOrderMsg {
    char     msg_type;        // 'A'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];    // 48-bit nanoseconds since midnight
    uint64_t order_ref_num;
    char     buy_sell;        // 'B' or 'S'
    uint32_t shares;
    char     stock[8];
    uint32_t price;           // fixed-point: divide by 10000 for dollars
};

// ... similarly for DeleteOrderMsg, CancelOrderMsg, ReplaceOrderMsg, ExecuteOrderMsg

#pragma pack(pop)
```

Helper to read the 6-byte ITCH timestamp:

```cpp
inline uint64_t read_timestamp(const uint8_t* ts) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | ts[i];
    return v;  // nanoseconds since midnight
}
```

#### Step 1.3 — PCAP replay reader (2 h)

ITCH sample data comes as a gzip-compressed binary file, not a PCAP. The Nasdaq format is a simple length-prefixed binary stream:

```
[2-byte big-endian message length][message bytes][2-byte length][message bytes]...
```

```cpp
// include/transport/pcap_replay.hpp
class ITCHFileReader {
public:
    explicit ITCHFileReader(const std::string& path);
    // Returns pointer to next raw message and its length.
    // Returns nullptr at EOF.
    const uint8_t* next_message(uint16_t& out_len);
private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
};
```

Download the free sample file from Nasdaq FTP (see §8 Data Sources). Decompress with `zcat` before reading.

#### Step 1.4 — SPSC ring buffer (3 h)

A single-producer / single-consumer lock-free queue between the parser thread and the book thread. Key properties:

- Fixed-size power-of-2 capacity (e.g. 65536 slots)
- Cache-line-aligned head/tail atomics — each on its own 64-byte line to eliminate false sharing
- No heap allocation after construction
- Slots hold a tagged union of parsed message structs

```cpp
// include/transport/spsc_queue.hpp
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
public:
    bool try_push(const T& item) noexcept;
    bool try_pop(T& out) noexcept;
private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity> slots_;
};
```

**Important:** `head_` and `tail_` must each occupy their own cache line. Placing them in the same line causes false sharing — the producer invalidates the consumer's cache line on every push.

#### Step 1.5 — Parser dispatcher (1 h)

The parser reads a message from the file reader, switches on `msg_type`, fills the appropriate struct, and calls `try_push` into the SPSC queue.

#### Milestone 1 checklist

- [ ] Parser correctly decodes all 9 message types from a known byte sequence (unit tested)
- [ ] PCAP replay reader iterates through sample file without errors
- [ ] SPSC queue passes producer/consumer stress test with 10M messages, no data corruption
- [ ] Parser thread pushes 100k messages into SPSC; book-side consumer reads them all

---

### Week 2 — Order Book Core Data Structures

**Goal:** Implement a correct, zero-allocation-on-hot-path order book with O(1) add/cancel/modify/execute, and a direct best-bid/ask pointer. No live feed yet — test with synthetic workloads.

**Estimated time:** ~12 hours

#### Step 2.1 — The price-level array decision (critical) (1 h)

> This is the single most important design decision. Get it right before writing any code.

**Do not use `std::map<int, PriceLevel>`.**

`std::map` is a red-black tree. Every lookup follows 3–5 pointer dereferences through non-contiguous heap nodes. At 1M messages/sec, this is fatal.

**Use a contiguous array indexed by price tick.**

ITCH prices are fixed-point integers (e.g. price 1234567 = $123.4567). For a single equity, the active price range at any moment spans ~100–500 ticks around the current mid. Allocate a fixed-size array (e.g. 8192 slots) centred on a reference price. Lookup is a direct array index — one cache line, no pointer chasing.

```
active_price_range ≈ 500 ticks × sizeof(PriceLevel) = 500 × 64B = 32 KB
                                                                  → fits in L1D cache
```

For bids: index `ref_price - price` (descending). For asks: index `price - ref_price` (ascending). Re-centre the array if the mid moves far enough (rare).

#### Step 2.2 — PriceLevel struct (2 h)

```cpp
// include/book/price_level.hpp
struct Order {
    uint64_t order_ref;
    uint32_t shares;
    Order*   next;   // intrusive linked list — no heap allocation
    Order*   prev;
};

struct alignas(64) PriceLevel {
    uint64_t total_qty  = 0;
    uint32_t order_count = 0;
    Order*   head       = nullptr;  // FIFO front
    Order*   tail       = nullptr;  // FIFO back
    // pad to 64 bytes to occupy exactly one cache line
    uint8_t  _pad[64 - sizeof(uint64_t) - sizeof(uint32_t) - 2*sizeof(Order*)];
};
```

`alignas(64)` ensures each price level occupies exactly one cache line. Accessing bid level 100 never evicts ask level 100 from cache.

#### Step 2.3 — Order slab allocator (2 h)

Never call `new Order` on the hot path. Pre-allocate a pool of `Order` objects at startup and hand them out via a free-list.

```cpp
// include/book/order.hpp
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity);
    Order* alloc() noexcept;     // O(1), no syscall
    void   free(Order*) noexcept;
private:
    std::vector<Order> pool_;
    Order*             free_head_ = nullptr;
};
```

Alternatively, use `std::pmr::monotonic_buffer_resource` with a pre-allocated 8 MB stack buffer — even simpler.

Verify with Valgrind massif or `-fsanitize=address` that **zero heap allocations** occur during a 1M-message replay after warm-up.

#### Step 2.4 — OrderBook class (4 h)

```cpp
// include/book/order_book.hpp
class OrderBook {
public:
    void on_add    (const AddOrderMsg&);
    void on_cancel (const CancelOrderMsg&);
    void on_delete (const DeleteOrderMsg&);
    void on_replace(const ReplaceOrderMsg&);
    void on_execute(const ExecuteOrderMsg&);

    BBO  best_bid_offer() const noexcept;  // O(1) — direct pointer dereference
    void depth_snapshot(std::vector<PriceLevel>& out, int levels) const;

private:
    std::array<PriceLevel, 8192>              bid_levels_;
    std::array<PriceLevel, 8192>              ask_levels_;
    std::unordered_map<uint64_t, Order*>      order_map_;  // order_ref → Order*
    OrderPool                                 pool_;

    PriceLevel* best_bid_ = nullptr;  // direct pointer, updated on every change
    PriceLevel* best_ask_ = nullptr;
    uint32_t    ref_price_ = 0;       // centre of the price array

    int  bid_idx(uint32_t price) const noexcept { return ref_price_ - price; }
    int  ask_idx(uint32_t price) const noexcept { return price - ref_price_; }
    void update_best_bid_after_remove(int idx);
    void update_best_ask_after_remove(int idx);
};
```

All operations must be O(1):

| Operation   | Complexity | Key mechanism                          |
|-------------|------------|----------------------------------------|
| `on_add`    | O(1)       | Array index, intrusive list append, order_map insert |
| `on_cancel` | O(1)       | order_map lookup, intrusive list splice, pool free   |
| `on_delete` | O(1)       | Same as cancel                         |
| `on_replace`| O(1)       | Delete + add in one message            |
| `on_execute`| O(1)       | Decrement qty; delete if fully filled  |
| `best_bid_offer` | O(1) | Direct pointer dereference             |

#### Step 2.5 — BBO tracking (1 h)

Maintain `best_bid_` and `best_ask_` as direct pointers into the price-level array. Update them on every add/cancel/delete that touches the top of book. Never scan the array to find the best level.

When the best level is depleted (total_qty == 0), walk one step in the array direction — this is amortised O(1) in practice.

#### Milestone 2 checklist

- [ ] All 5 operations correct on synthetic workload (property-based tests)
- [ ] Zero heap allocations on hot path after warm-up (Valgrind massif)
- [ ] BBO invariant holds after every message (assert in debug build)
- [ ] Stress test: 10M random add/cancel/execute — no crashes, no leaks

---

### Week 3 — End-to-End Integration + Correctness Validation

**Goal:** Wire parser → SPSC → book end-to-end on real ITCH data. Validate book state against LOBSTER ground-truth snapshots to 100% correctness across a full trading day.

**Estimated time:** ~10 hours

#### Step 3.1 — Thread setup with CPU affinity (1 h)

```cpp
// src/main.cpp
auto set_affinity = [](std::thread& t, int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(t.native_handle(), sizeof(cpuset), &cpuset);
};

std::thread parser_thread([&]{ run_parser(reader, queue); });
std::thread book_thread  ([&]{ run_book  (queue, book);   });

set_affinity(parser_thread, 0);
set_affinity(book_thread,   1);
```

Also set thread priority (`SCHED_FIFO`) if running as root to prevent OS preemption.

#### Step 3.2 — LOBSTER validation (5 h)

LOBSTER provides reconstructed book snapshots at fixed intervals (e.g. every 10 seconds) for free sample symbols. These are CSV files with columns:

```
timestamp, ask_price_1, ask_size_1, bid_price_1, bid_size_1, ..., ask_price_10, ask_size_10, bid_price_10, bid_size_10
```

Write a validator that:

1. Replays a full day of ITCH data through your book
2. At each LOBSTER snapshot timestamp, takes a depth snapshot from your book
3. Compares your top-10 bid/ask levels (price + qty) against LOBSTER
4. Logs any mismatch with the offending message sequence

Zero mismatches is the target. Common failure modes:

- Forgetting to handle the `Replace` (`U`) message type — your order count will drift
- Off-by-one in the timestamp comparison (LOBSTER timestamps are microseconds; ITCH is nanoseconds)
- Ignoring hidden / non-displayed orders (`AddOrderMsg` has a `display` flag — these don't appear in the visible book)
- Not handling the `Execute with Price` (`C`) message differently from `Execute` (`E`) for auction crosses

#### Step 3.3 — Analytics layer (2 h)

Implement as a read-side observer that receives periodic snapshots from the book thread. Never stall the book thread.

```cpp
struct Analytics {
    double order_flow_imbalance() const;  // (bid_qty - ask_qty) / (bid_qty + ask_qty)
    double mid_price()           const;  // (best_bid + best_ask) / 2
    double spread()              const;  // best_ask - best_bid (in ticks)
    double vwap(Side, int levels) const; // volume-weighted avg price, N levels deep
};
```

#### Step 3.4 — Edge cases (2 h)

Handle these before benchmarking or they will corrupt results:

- **Out-of-sequence messages**: ITCH guarantees order within a session, but your PCAP parser must handle the framing correctly
- **Stock halts**: System Event `H` message — drain the book on halt
- **Pre-market / post-market**: Only process messages between `System Event O` (market open) and `System Event C` (market close)
- **Price array overflow**: If price moves outside your array bounds, re-centre gracefully

#### Milestone 3 checklist

- [ ] End-to-end replay runs without crashing on a full day of ITCH data
- [ ] Book state matches LOBSTER snapshots at every checkpoint (0 mismatches)
- [ ] Analytics values (OFI, mid, spread) are numerically correct vs manual calculation
- [ ] All edge cases handled: Replace, hidden orders, halts, boundary prices

---

### Week 4 — Latency Benchmarking + Optimisation

**Goal:** Instrument the hot path, achieve p99 < 500 ns, generate a flamegraph, and eliminate L1D cache misses and branch mispredictions.

**Estimated time:** ~12 hours

#### Step 4.1 — rdtsc latency measurement (2 h)

`rdtsc` reads the CPU timestamp counter in cycles. Convert to nanoseconds using the TSC frequency.

```cpp
// include/bench/rdtsc.hpp
#include <cstdint>

inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}

inline uint64_t tsc_freq_hz() {
    // Calibrate against CLOCK_MONOTONIC over 100ms
    // Returns cycles per second (typically 3e9 on a 3 GHz CPU)
}

inline double cycles_to_ns(uint64_t cycles) {
    return double(cycles) * 1e9 / tsc_freq_hz();
}
```

Measure from the byte arriving in the ring buffer to the BBO pointer being updated:

```cpp
uint64_t t0 = rdtsc();
book.on_add(msg);
uint64_t t1 = rdtsc();
histogram.record(t1 - t0);
```

Build a histogram with 1 ns buckets up to 10 µs. Report p50, p99, p999 at the end of replay.

**Important:** Use `rdtscp` (with `cpuid` serialisation) for the start timestamp to prevent out-of-order execution from skewing measurements.

#### Step 4.2 — Baseline measurement (1 h)

Before any optimisation, record the baseline numbers. Keep a `BENCHMARKS.md` file updated with each run:

```
Date        | Commit  | p50   | p99    | p999   | L1D miss% | Branch miss%
------------|---------|-------|--------|--------|-----------|-------------
[baseline]  | abc1234 | 280ns | 1200ns | 8400ns | 4.2%      | 1.8%
```

#### Step 4.3 — perf profiling (2 h)

Run `perf stat` on the book thread:

```bash
perf stat -e cycles,instructions,cache-references,cache-misses,\
branch-instructions,branch-misses,L1-dcache-loads,L1-dcache-load-misses \
./orderbook_bench --replay data/sample.bin
```

Targets:

| Metric                  | Target     |
|-------------------------|------------|
| L1D cache miss rate     | < 2%       |
| Branch mispredict rate  | < 0.5%     |
| IPC (instructions/cycle)| > 2.0      |

#### Step 4.4 — Flamegraph generation (1 h)

```bash
# Record with call graph
perf record -F 1000 -g --call-graph dwarf ./orderbook_bench --replay data/sample.bin

# Generate flamegraph
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg
```

Open in browser. The hottest function should be `OrderBook::on_add` or `OrderBook::on_delete`. If you see `malloc`, `new`, or anything from `unordered_map`'s rehash path — those are bugs to fix.

#### Step 4.5 — Optimisation passes (4 h)

Apply in this order, re-measuring after each:

**Pass 1 — Compiler flags**
```cmake
target_compile_options(orderbook PRIVATE
    -O3 -march=native -fno-exceptions -fno-rtti
    -funroll-loops -fomit-frame-pointer
)
```

**Pass 2 — False sharing elimination**

If `bid_levels_` and `ask_levels_` share a struct, split them into separate arrays so the parser thread's writes to bids don't invalidate the book thread's cache lines for asks.

**Pass 3 — Branch elimination via templates**

Replace:
```cpp
void on_add(const AddOrderMsg& msg) {
    if (msg.buy_sell == 'B') bid_levels_[bid_idx(price)].add(order);
    else                      ask_levels_[ask_idx(price)].add(order);
}
```

With:
```cpp
template <Side S>
void on_add_impl(uint32_t price, uint32_t shares, uint64_t order_ref);

// Explicit instantiations — compiler sees no branch, generates two code paths
```

**Pass 4 — Huge pages**

If `perf stat` shows high TLB miss rate, back the price-level arrays with huge pages (2 MB) via `mmap(MAP_HUGETLB)`.

**Pass 5 — `likely`/`unlikely` hints**

```cpp
if (__builtin_unlikely(best_bid_ == nullptr)) { ... }
```

Most messages are not top-of-book touches — hinting the branch predictor here saves a few cycles per message.

#### Step 4.6 — Google Benchmark suite (2 h)

```cpp
// bench/bench_book.cpp
#include <benchmark/benchmark.h>

static void BM_AddOrder(benchmark::State& state) {
    OrderBook book;
    AddOrderMsg msg = make_synthetic_add(100'000'000, 'B', 100, 12345678);
    for (auto _ : state) {
        msg.order_ref_num++;  // unique ref each time
        book.on_add(msg);
    }
}
BENCHMARK(BM_AddOrder)->Iterations(1'000'000);

// Also: BM_CancelOrder, BM_ModifyOrder, BM_BBO
```

Run with `--benchmark_format=json` and commit the output to `bench/results/`.

#### Milestone 4 checklist

- [ ] p99 latency < 500 ns on replay workload
- [ ] Flamegraph shows no unexpected hot spots (malloc, rehash, etc.)
- [ ] L1D miss rate < 2%, branch mispredict < 0.5% from `perf stat`
- [ ] Google Benchmark suite committed with baseline results
- [ ] `BENCHMARKS.md` updated with before/after for each optimisation pass

---

### Week 5 — Polish, Documentation + Resume Packaging

**Goal:** Make the project repo presentation-ready for HFT interviewers. Clean README, committed benchmark results, flamegraph screenshot, and an optional live feed extension.

**Estimated time:** ~8 hours

#### Step 5.1 — README.md (3 h)

The README is the first thing an interviewer sees. Structure it as follows:

```markdown
# L2 Order Book Engine

> Full-depth NASDAQ ITCH 5.0 order book in C++20.
> p99 book-update latency: **XXX ns** | Throughput: **X.X M msgs/sec**

## Performance

| Metric              | Result       |
|---------------------|--------------|
| p50 latency         | XXX ns       |
| p99 latency         | XXX ns       |
| p999 latency        | X.X µs       |
| Throughput          | X.X M msgs/s |
| L1D cache miss rate | X.X%         |
| Branch mispredict   | X.XX%        |

## Architecture
[diagram or ASCII art of 4-layer design]

## Key design decisions
[price-level array vs std::map, SPSC queue, slab allocator — one paragraph each]

## Building and running
[CMake instructions, how to get sample data, how to run the benchmark]

## Validation
[LOBSTER comparison: 0 mismatches over X hours of ITCH data]

![Flamegraph](bench/results/flamegraph.png)
```

Performance numbers go at the top — not buried at the end.

#### Step 5.2 — Commit benchmark artifacts (1 h)

```
bench/
└── results/
    ├── baseline_2025-XX-XX.json          ← Google Benchmark JSON
    ├── optimised_2025-XX-XX.json
    ├── flamegraph_baseline.svg
    ├── flamegraph_optimised.svg
    └── perf_stat_optimised.txt
```

Committed SVG flamegraphs render directly in GitHub — an interviewer can open the repo and immediately see your hot path.

#### Step 5.3 — Optional: live UDP multicast feed (2 h)

Replace the PCAP replay with a real UDP socket consuming a multicast feed. This requires access to Nasdaq's data (available via various market data vendors at low cost, or through university lab access).

```cpp
// include/transport/udp_feed.hpp
class UDPMulticastFeed {
public:
    UDPMulticastFeed(const std::string& mcast_group, uint16_t port);
    const uint8_t* recv_packet(std::size_t& out_len);  // blocks until packet arrives
private:
    int sockfd_;
};
```

If a live feed is not available, note in the README that the architecture supports it — the parser and SPSC are feed-agnostic.

#### Step 5.4 — Optional: market-making simulator (2 h)

A thin layer on top of the BBO stream that emits synthetic quotes. Demonstrates you understand how the book output is consumed downstream:

```cpp
class MarketMaker {
    // Consume BBO updates
    // Quote bid = mid - spread/2, ask = mid + spread/2
    // Adjust spread based on order flow imbalance
    void on_bbo(const BBO& bbo);
};
```

This is optional but adds a `market-making` keyword to your resume bullet.

#### Milestone 5 checklist

- [ ] README opens with concrete performance numbers
- [ ] Benchmark JSON results committed and referenced in README
- [ ] Flamegraph SVG committed and rendered in README
- [ ] LOBSTER validation results documented (0 mismatches, X hours of data)
- [ ] Repo is public on GitHub, builds cleanly with `cmake --build`
- [ ] Code is clean: no TODOs, no commented-out blocks, consistent naming

---

## 5. Key Design Decisions

### Why a price-level array, not `std::map`

`std::map<int, PriceLevel>` involves 3–5 pointer dereferences per lookup through a heap-allocated red-black tree. At 5M messages/sec, this is 15–25 ns of pointer chasing per message, plus constant TLB pressure.

A contiguous `std::array<PriceLevel, N>` indexed by `price - ref_price` is a single array offset calculation. The active tick range (~500 levels) fits entirely in L1D cache (32 KB). This is the same design used by most production HFT order books.

### Why SPSC, not a mutex

A mutex acquire/release involves a kernel syscall on contention (~1–5 µs). An SPSC queue using `std::atomic` with `memory_order_acquire` / `memory_order_release` costs ~5 ns. For a parser and book thread that need to communicate 5M times/sec, there is no alternative.

### Why `std::pmr` / slab allocation

`new`/`delete` call the OS allocator (ptmalloc2, jemalloc). Even fast allocators spend 30–100 ns per call with lock contention under multi-threading. A pre-allocated pool gives O(1) constant-time allocation with zero syscalls and no contention.

### Why `rdtsc` for latency measurement

`clock_gettime(CLOCK_MONOTONIC)` involves a VDSO call (~30–50 ns overhead) — larger than the latency you are trying to measure. `rdtsc` reads the TSC register directly in ~5 cycles (~2 ns). It is the only tool accurate enough for sub-microsecond latency measurement.

---

## 6. Performance Targets

| Metric                        | Target        | Stretch goal    |
|-------------------------------|---------------|-----------------|
| p50 book update latency       | < 200 ns      | < 100 ns        |
| p99 book update latency       | < 500 ns      | < 250 ns        |
| p999 book update latency      | < 2 µs        | < 1 µs          |
| Sustained throughput          | > 5 M msgs/s  | > 15 M msgs/s   |
| L1D cache miss rate           | < 2%          | < 0.5%          |
| Branch mispredict rate        | < 0.5%        | < 0.1%          |
| Hot-path heap allocations     | 0             | 0               |
| LOBSTER snapshot mismatches   | 0             | 0               |

---

## 7. Toolchain & Build Setup

### Requirements

- CMake 3.20+
- GCC 12+ or Clang 15+ (C++20 required)
- Linux (for `rdtsc`, `perf`, `pthread_setaffinity_np`, `SCHED_FIFO`)
- Google Benchmark (fetched via CMake FetchContent)
- Google Test (fetched via CMake FetchContent)
- Brendan Gregg flamegraph scripts (cloned separately)

### CMakeLists.txt skeleton

```cmake
cmake_minimum_required(VERSION 3.20)
project(l2_order_book CXX)
set(CMAKE_CXX_STANDARD 20)

# Release build flags
add_compile_options(-O3 -march=native -fno-exceptions -fno-rtti)

# Fetch dependencies
include(FetchContent)
FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0)
FetchContent_MakeAvailable(benchmark googletest)

# Main library
add_library(orderbook_lib
    src/itch/parser.cpp
    src/book/order_book.cpp
    src/book/price_level.cpp
    src/transport/pcap_replay.cpp)
target_include_directories(orderbook_lib PUBLIC include)

# Main executable
add_executable(orderbook src/main.cpp)
target_link_libraries(orderbook orderbook_lib)

# Benchmark binary
add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book orderbook_lib benchmark::benchmark)

# Tests
enable_testing()
add_executable(tests
    test/test_parser.cpp
    test/test_book.cpp
    test/test_lobster_validate.cpp)
target_link_libraries(tests orderbook_lib GTest::gtest_main)
add_test(NAME unit_tests COMMAND tests)
```

### Build commands

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build -V

# Run benchmark
./build/bench_book --benchmark_format=json > bench/results/$(date +%Y-%m-%d).json

# Run perf
sudo perf stat -e cycles,L1-dcache-load-misses,branch-misses \
    ./build/orderbook --replay data/sample.bin
```

---

## 8. Data Sources

### Free ITCH 5.0 sample data

Nasdaq provides free historical ITCH binary files via FTP:

```bash
# List available files (requires curl or FTP client)
ftp emi.nasdaq.com
# Navigate to: /NQTV/
# Download a sample .gz file (typically ~1–3 GB per trading day)
```

Alternatively: the LOBSTER Academic Data Service (lobsterdata.com) provides free sample data for AAPL and a handful of other symbols, including both the raw messages and pre-computed snapshots.

### LOBSTER validation files

Download `_orderbook_X.csv` and `_message_X.csv` for the same symbol and date as your ITCH file. The `_orderbook_X.csv` contains the ground-truth snapshots your book must match.

---

## 9. Testing Strategy

### Unit tests (Week 1–2)

- **Parser tests:** Construct known ITCH byte sequences by hand; assert struct fields decode correctly (especially byte-swap, 6-byte timestamp)
- **SPSC tests:** Hammer with 10M push/pop from two threads; assert no lost or duplicated items
- **Book tests:** Property-based testing — generate random sequences of add/cancel/execute; assert invariants after every operation:
  - `best_bid >= all other bid prices`
  - `best_ask <= all other ask prices`
  - `best_bid < best_ask` (no crossed book)
  - `total_qty[level] == sum of all order quantities at that level`
  - `order_map.size() == total outstanding orders`

### Integration tests (Week 3)

- **LOBSTER validation:** Full-day replay, 0 mismatches vs LOBSTER snapshots
- **Throughput test:** Assert > 5M messages/sec sustained for 60 seconds

### Regression benchmarks (Week 4–5)

- Run `bench_book` on every commit; fail CI if p99 regresses by > 10%

---

## 10. References

| Resource | Link | Why it matters |
|----------|------|----------------|
| NASDAQ ITCH 5.0 spec | nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf | The authoritative binary message format reference |
| LOBSTER data | lobsterdata.com | Free reconstructed L2 snapshots for validation |
| LMAX Disruptor paper | lmax-exchange.github.io/disruptor | The canonical reference for lock-free ring buffer design |
| "What every programmer should know about memory" (Drepper) | akkadia.org/drepper/cpumemory.pdf | Essential reading before cache-line optimisation |
| Brendan Gregg flamegraph | github.com/brendangregg/FlameGraph | Flamegraph generation scripts |
| Google Benchmark | github.com/google/benchmark | Microbenchmark harness |
| "How to Build a Fast Limit Order Book" (WK Selph) | web.archive.org/web/20110219163448/http://howtohft.wordpress.com/2011/02/15/how-to-build-a-fast-limit-order-book/ | Seminal blog post on the data structure — read this early |
| Dreyer paper on L2 LOB implementation | arxiv.org/abs/2309.04259 | C++ design patterns for low-latency HFT |

---

## 11. Resume Bullet

Once the project is complete with measured numbers, use this bullet (fill in your actual figures):

> *"Built a full-depth L2 order book engine in C++20 consuming NASDAQ ITCH 5.0 binary market data via a lock-free SPSC ring buffer, with a cache-line-aligned price-level array and slab-allocated order pool achieving zero heap allocation on the hot path; validated against LOBSTER ground-truth snapshots across a full trading day with zero mismatches. Benchmarked end-to-end book-update latency at p50 XXX ns / p99 XXX ns and sustained throughput of X.X M msgs/sec; profiled with perf and flamegraph achieving L1D miss rate < 2% and branch-mispredict rate < 0.5%."*

---

*Last updated: May 2026*