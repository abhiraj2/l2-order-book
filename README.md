# L2 Order Book Engine

Full-depth Level 2 order book engine in C++20 consuming **NASDAQ TotalView-ITCH 5.0** binary market data. Designed for sub-microsecond book-update latency with zero heap allocation on the hot path.

> **Status:** Weeks 1–2 complete (parser + book core). Week 3 (end-to-end integration + LOBSTER validation) in progress. Performance numbers will be added after Week 4 benchmarking.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                   Feed Ingestion Layer                    │
│  ITCHFileReader — length-prefixed binary stream          │
│  ITCHParser     — zero-copy reinterpret_cast onto buffer │
│  ParsedMessage  — std::variant tagged union              │
└───────────────────────┬──────────────────────────────────┘
                        │  lock-free SPSC ring buffer
┌───────────────────────▼──────────────────────────────────┐
│                   Order Book Core                         │
│  std::array<PriceLevel, 8192> × 2 (bid + ask sides)     │
│  OrderPool slab — zero heap alloc after warm-up          │
│  order_map: unordered_map<order_ref, Order*>  O(1)       │
│  Operations: Add · Cancel · Execute · Replace  all O(1)  │
└───────────────────────┬──────────────────────────────────┘
                        │  BBO snapshot
┌───────────────────────▼──────────────────────────────────┐
│               Analytics / Consumer Layer                  │
│  BBO stream (best bid/offer)                             │
│  Mid-price, spread, order-flow imbalance (planned)       │
└──────────────────────────────────────────────────────────┘
```

### Thread model

| Thread    | Core | Responsibility                                      |
|-----------|------|-----------------------------------------------------|
| Parser    | 0    | Read file/socket, parse ITCH, push to SPSC          |
| Book      | 1    | Pop SPSC, update book, publish BBO snapshot         |
| Analytics | 2    | Read-only snapshots, compute derived statistics     |

All inter-thread communication is via lock-free SPSC queues. No mutexes on the hot path.

---

## Key Design Decisions

### Price-level array instead of `std::map`

`std::map<price, PriceLevel>` means 3–5 pointer dereferences through a heap-allocated red-black tree per lookup. At millions of messages per second this is fatal.

Instead, each side of the book is a `std::array<PriceLevel, 8192>`. The bid side indexes prices as `bid_ref - price` (higher prices → lower indices), the ask side as `price - ask_ref` (lower prices → lower indices). The active ~500-tick window around the spread is ~32 KB per side and fits entirely in L1D cache. Lookup is a single array index — one addition, no pointer chasing.

```
bid_levels_[i]  ←→  price = bid_ref_ - i   (idx 0 = best bid)
ask_levels_[i]  ←→  price = ask_ref_ + i   (idx 0 = best ask)
```

Each side maintains its own reference price (`bid_ref_`, `ask_ref_`) and re-centres independently when a price moves outside the array range — a rare event on a normal trading day.

### `alignas(64)` on `PriceLevel`

```cpp
struct alignas(64) PriceLevel {
    uint64_t total_qty;
    uint32_t order_count;
    Order*   head;
    Order*   tail;
};
static_assert(sizeof(PriceLevel) <= 64);
```

Each level occupies exactly one cache line. Touching bid level 100 never evicts ask level 100 from cache. The active window of ~500 levels × 64 B = 32 KB fits in a typical 32 KB L1D.

### SPSC queue with separated cache lines

```cpp
alignas(64) std::atomic<std::size_t> head_{0};  // consumer writes this
alignas(64) std::atomic<std::size_t> tail_{0};  // producer writes this
```

Producer and consumer atomics each occupy their own 64-byte cache line. Without this separation, every producer push would invalidate the consumer's cache line (false sharing), adding a full cache-coherence round-trip — ~60 ns on a modern NUMA system.

### Slab allocator — zero hot-path heap allocation

`OrderPool` pre-allocates all `Order` objects at startup and maintains a free-list threaded through `Order::next`. `alloc()` and `free()` are a pointer swap — no `malloc`, no kernel call, no lock contention.

```
Before: free_head_ → [slot 3] → [slot 7] → [slot 1] → nullptr
alloc():  returns slot 3, free_head_ → [slot 7]
free(o):  o→next = free_head_, free_head_ = o
```

### Cached `level_idx` in `Order`

Every `Order` stores the index into its side's price-level array at insertion time. `remove_order` uses this directly rather than recomputing `ref - price` — this means removal is correct even if a re-centre changes `bid_ref_` or `ask_ref_` in between.

### Zero-copy ITCH parsing

ITCH messages are parsed by `reinterpret_cast` directly onto the raw read buffer — no per-field copies. All multi-byte fields are big-endian and accessed via `__builtin_bswap16/32/64`. The parsed `std::variant` holds a copy of the struct (≤ 40 bytes) pushed into the SPSC queue; the raw buffer is immediately reusable.

---

## Building

**Requirements:** CMake 3.20+, Clang 15+ / GCC 12+ (C++20), macOS or Linux.  
Dependencies (Google Benchmark, GoogleTest) are fetched via CMake `FetchContent` — no manual install needed.

```bash
# Configure (macOS)
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release

# Configure (Linux with Ninja)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build everything
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build -V

# Run a single test suite
./build/tests --gtest_filter=BookTest.*

# Run benchmarks
./build/bench_book --benchmark_format=json
```

---

## Tests

34 tests across 3 suites, all passing:

| Suite | Tests | What's covered |
|-------|-------|----------------|
| `ParserTest` | 11 | All 9 ITCH message types decoded from raw bytes; byte-swap correctness; 6-byte timestamp round-trip; truncated/null/unknown inputs |
| `SPSCQueueTest` | 6 | FIFO order; full-queue backpressure; wrap-around; 10M-message two-thread stress test (no lost or duplicated items) |
| `BookTest` | 17 | Add/cancel/execute/replace correctness; BBO tracking; best-bid/ask advancement; order-count invariant; 50k-operation random stress with no-crossed-book assertion after every op |

---

## ITCH 5.0 Message Types Handled

| Code | Name | Book effect |
|------|------|-------------|
| `S` | System Event | Session open/close markers |
| `A` | Add Order | New resting order |
| `F` | Add Order (MPID) | New resting order with market participant ID |
| `E` | Order Executed | Partial/full fill — reduces quantity |
| `C` | Execute with Price | Fill at non-reference price (auctions) |
| `X` | Order Cancel | Partial cancellation |
| `D` | Order Delete | Full removal |
| `U` | Order Replace | Atomic cancel + new order |
| `P` | Trade | Informational; not a resting order |

---

## Project Structure

```
include/
  itch/        messages.hpp     — packed ITCH structs, byte-swap helpers
               parser.hpp       — ParsedMessage variant, parse_message()
  book/        order.hpp        — Order struct + OrderPool slab allocator
               price_level.hpp  — alignas(64) PriceLevel, intrusive FIFO list
               order_book.hpp   — OrderBook, all O(1) operations
               bbo.hpp          — BBO snapshot struct
  transport/   spsc_queue.hpp   — lock-free SPSCQueue<T, Capacity>
               pcap_replay.hpp  — ITCHFileReader (length-prefixed binary)
  bench/       rdtsc.hpp        — rdtsc latency timer (planned)
src/           implementations
test/          unit + stress tests
bench/         Google Benchmark suite (planned)
```

---

## Roadmap

- [x] Week 1 — ITCH 5.0 parser, SPSC ring buffer, file reader
- [x] Week 2 — Order book core: price-level array, slab allocator, BBO tracking
- [ ] Week 3 — End-to-end integration, LOBSTER ground-truth validation
- [ ] Week 4 — Latency benchmarking (`rdtsc`), perf profiling, flamegraph
- [ ] Week 5 — README performance numbers, benchmark artifacts, optional live UDP feed
