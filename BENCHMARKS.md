# BENCHMARKS.md

Baseline performance measurements for the L2 order book engine.
All results captured on the development machine; record updated with each
significant change.

---

## Environment

| Field              | Value                                        |
|--------------------|----------------------------------------------|
| Date               | 2026-05-24                                   |
| Platform           | macOS (ARM64, Apple M-series)                |
| Cores              | 10 × 24 MHz (Google Benchmark sysctl)        |
| L1D cache          | 64 KiB                                       |
| L2 cache           | 4 MiB × 10                                   |
| Build flags        | `-O3 -march=native -fno-exceptions -fno-rtti`|
| Timing (harness)   | `clock_gettime(CLOCK_MONOTONIC_RAW)` ~42 ns res |
| Timing (benchmark) | Google Benchmark v1.8.3, 3 repetitions       |

> **Note:** `CLOCK_MONOTONIC_RAW` on macOS quantises at ~42 ns, so the
> latency harness p50 values below show 0 ns for operations faster than one
> timer tick. The Google Benchmark mean (which amortises many iterations) is
> the authoritative latency figure on this platform.

---

## Google Benchmark results (`bench_book`)

Run command:
```
./build/bench_book --benchmark_format=json \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    > bench/results/$(date +%Y-%m-%d).json
```

| Benchmark                   | Mean (ns) | CV    | Notes                              |
|-----------------------------|----------:|------:|------------------------------------|
| `BM_Book_BBO`               |      0.98 | 0.22% | Single pointer dereference         |
| `BM_Parse_AddOrder`         |      1.23 | 0.70% | `memcpy` + `switch` + variant      |
| `BM_Parse_DeleteOrder`      |      1.44 | 0.46% |                                    |
| `BM_SPSC_RoundTrip`         |      2.44 | 1.07% | `try_push` + `try_pop` (1 thread)  |
| `BM_Book_CancelPartial`     |      2.99 | 0.36% | Reduce shares, no removal          |
| `BM_Book_AddBid`            |     25.9  | 0.57% | Hash lookup + level insert + BBO   |
| `BM_Book_AddAsk`            |     26.5  | 0.71% |                                    |
| `BM_Book_Delete`            |     25.9  | 0.82% | Hash erase + level splice          |
| `BM_Book_Replace_SameLevel` |     25.6  | 1.65% | Delete + add at same price         |
| `BM_Book_Replace_NewLevel`  |     26.0  | 0.15% | Delete + add at different price    |
| `BM_MixedWorkload`          |     20.2  | 0.49% | 70% add, 10% BBO, 10% cancel, 10% replace |

### Key observations

- **BBO is 0.98 ns** — `best_bid_offer()` is a direct array index read with no
  scan; the active price levels stay in L1D across the warm-up orders.
- **Parsing is 1.2–1.5 ns** — the `switch + memcpy + variant` path is
  dominated by `memcpy` into the struct; no heap allocation.
- **SPSC round-trip is 2.4 ns** — the power-of-2 ring buffer with separate
  cache-line atomics avoids false sharing entirely at this scale.
- **Add/Delete/Replace cluster at 25–26 ns** — the bottleneck is the
  `unordered_map` hash lookup (`order_map_`). This is the expected cost on ARM.
- **Mixed workload is 20.2 ns** — lower than raw add/delete because 30% of
  the loop is the cheaper BBO + cancel operations.

---

## Latency harness results (`latency_bench`)

Run command:
```
./build/latency_bench
```

1 000 000 iterations per operation. Values in nanoseconds.

| Operation                      | p50 (ns) | p99 (ns) | p999 (ns) |
|--------------------------------|---------:|---------:|----------:|
| AddOrder (bid, best level)     |       41 |       83 |        84 |
| AddOrder (ask, best level)     |        0 |       42 |        42 |
| BBO query                      |        0 |       42 |        42 |
| DeleteOrder                    |        0 |       42 |        83 |
| CancelOrder (partial, -1 share)|        0 |       42 |        42 |
| ReplaceOrder (same level)      |       41 |       42 |        83 |
| parse_message (AddOrder)       |        0 |       42 |        42 |

> p50 = 0 ns means the operation completed within a single 42 ns timer tick.
> On Linux/x86-64, rdtscp resolution is < 1 ns and these columns will be
> meaningful.

### p99 summary

All operations hit p99 ≤ 83 ns, well inside the 500 ns target.

---

## Throughput estimate

From the mixed-workload benchmark (20.2 ns/op mean):

```
1 / 20.2 ns ≈ 49.5 M ops/s (single-threaded, book thread only)
```

The two-threaded replay pipeline throughput is limited by the SPSC queue
consumer; at 2.44 ns/round-trip the queue alone supports >400 M msgs/s,
so the bottleneck is the book update at ~26 ns for order-mutating messages.

---

## Performance targets status

| Metric                      | Target      | Measured          | Status |
|-----------------------------|-------------|-------------------|--------|
| p99 latency (book ops)      | < 500 ns    | ≤ 83 ns           | ✓      |
| BBO query latency           | < 5 ns      | 0.98 ns           | ✓      |
| Parse throughput (single)   | > 5M msg/s  | ~800M msg/s       | ✓      |
| SPSC queue round-trip       | < 10 ns     | 2.44 ns           | ✓      |
| Hot-path heap allocations   | 0           | 0 (OrderPool)     | ✓      |

---

## How to track regressions

Save a dated JSON snapshot after any significant change:

```bash
./build/bench_book --benchmark_format=json \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    > bench/results/$(date +%Y-%m-%d).json
```

Compare two snapshots:

```bash
# Using google benchmark's compare.py (if available)
python3 $(find ~/.conan -name compare.py 2>/dev/null | head -1) \
    bench/results/baseline.json bench/results/new.json

# Quick manual diff
jq '[.benchmarks[] | {name:.name, mean:.cpu_time}]' bench/results/old.json
jq '[.benchmarks[] | {name:.name, mean:.cpu_time}]' bench/results/new.json
```

Any benchmark regressing > 10% from the baseline warrants investigation
before merging.

---

## Profiling notes (Linux/x86-64)

```bash
# CPU cycles and cache misses
sudo perf stat -e cycles,instructions,L1-dcache-load-misses,branch-misses \
    ./build/bench_book --benchmark_filter=BM_MixedWorkload

# Flamegraph
sudo perf record -g ./build/bench_book --benchmark_filter=BM_MixedWorkload
sudo perf script | stackcollapse-perf.pl | flamegraph.pl > bench/results/flamegraph.svg
```

Target L1D miss rate < 2%, branch mispredict < 0.5%.
