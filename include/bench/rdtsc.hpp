#pragma once

#include <cstdint>
#include <ctime>

// Platform-portable nanosecond timer.
//
// x86-64 Linux : rdtscp (serialising read of TSC).  Calibrated once against
//                CLOCK_MONOTONIC; converts cycles to nanoseconds at runtime.
// ARM / macOS  : clock_gettime(CLOCK_MONOTONIC_RAW) — already in nanoseconds,
//                no calibration needed.
//
// Usage:
//   const uint64_t t0 = now_ns();
//   do_work();
//   const uint64_t t1 = now_ns();
//   const uint64_t latency_ns = t1 - t0;

namespace bench {

#if defined(__x86_64__) || defined(_M_X64)

// ---------------------------------------------------------------------------
// x86-64: rdtscp with calibration
// ---------------------------------------------------------------------------

inline uint64_t rdtscp() noexcept {
    uint32_t lo, hi;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi) :: "%rcx");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Returns TSC ticks per nanosecond (calibrated once at first call).
inline double tsc_ghz() noexcept {
    static const double ghz = [] {
        // Measure TSC ticks over a 10 ms CLOCK_MONOTONIC sleep.
        struct timespec ts0, ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        const uint64_t tsc0 = rdtscp();

        struct timespec sleep_req = {0, 10'000'000}; // 10 ms
        nanosleep(&sleep_req, nullptr);

        const uint64_t tsc1 = rdtscp();
        clock_gettime(CLOCK_MONOTONIC, &ts1);

        const double ns = static_cast<double>(ts1.tv_sec  - ts0.tv_sec)  * 1e9
                        + static_cast<double>(ts1.tv_nsec - ts0.tv_nsec);
        return static_cast<double>(tsc1 - tsc0) / ns; // ticks per ns
    }();
    return ghz;
}

inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(static_cast<double>(rdtscp()) / tsc_ghz());
}

#else

// ---------------------------------------------------------------------------
// ARM / macOS: CLOCK_MONOTONIC_RAW (already nanoseconds, ~40 ns overhead)
// ---------------------------------------------------------------------------

inline uint64_t now_ns() noexcept {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

#endif // architecture

// ---------------------------------------------------------------------------
// Simple fixed-bucket histogram for latency recording.
// Buckets: [0,1) [1,2) ... [kBuckets-2, kBuckets-1) [kBuckets-1, ∞)
// Default range covers 0–10 µs in 1 ns steps (suitable for book operations).
// ---------------------------------------------------------------------------
template <std::size_t kBuckets = 10001>
class Histogram {
public:
    void record(uint64_t ns) noexcept {
        const std::size_t idx = ns < kBuckets - 1
                              ? static_cast<std::size_t>(ns)
                              : kBuckets - 1;
        ++buckets_[idx];
        ++total_;
    }

    // Returns the value at the given percentile (0.0–1.0).
    uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(total_));
        uint64_t cum = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            cum += buckets_[i];
            if (cum > target) return static_cast<uint64_t>(i);
        }
        return kBuckets - 1;
    }

    uint64_t p50()  const noexcept { return percentile(0.50); }
    uint64_t p99()  const noexcept { return percentile(0.99); }
    uint64_t p999() const noexcept { return percentile(0.999); }
    uint64_t count() const noexcept { return total_; }

    void reset() noexcept {
        for (auto& b : buckets_) b = 0;
        total_ = 0;
    }

private:
    uint64_t buckets_[kBuckets]{};
    uint64_t total_ = 0;
};

} // namespace bench
