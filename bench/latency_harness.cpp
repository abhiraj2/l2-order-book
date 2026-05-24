//
// Latency harness — measures per-operation tail latency using the rdtsc.hpp
// Histogram.  Run with no arguments; prints p50/p99/p999 for each operation.
//
// This deliberately avoids Google Benchmark's aggregation model so we can
// inspect tail latency independently of the mean.
//
#include "book/order_book.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "bench/rdtsc.hpp"

#include <cstdio>
#include <cstring>

using namespace book;
using namespace itch;

static constexpr uint32_t kBid = 1'500'000;
static constexpr uint32_t kAsk = 1'500'100;
static constexpr int      kIter = 1'000'000; // iterations per benchmark

// ---------------------------------------------------------------------------
// Message builders
// ---------------------------------------------------------------------------
static SystemEventMsg make_sys(char code) noexcept {
    SystemEventMsg m{};
    m.msg_type = 'S'; m.event_code = code;
    return m;
}

static AddOrderMsg make_add(char side, uint32_t price, uint32_t shares,
                            uint64_t ref) noexcept {
    AddOrderMsg m{};
    m.msg_type      = 'A';
    m.buy_sell      = side;
    m.price         = __builtin_bswap32(price);
    m.shares        = __builtin_bswap32(shares);
    m.order_ref_num = __builtin_bswap64(ref);
    return m;
}

static DeleteOrderMsg make_delete(uint64_t ref) noexcept {
    DeleteOrderMsg m{};
    m.msg_type      = 'D';
    m.order_ref_num = __builtin_bswap64(ref);
    return m;
}

static CancelOrderMsg make_cancel(uint64_t ref, uint32_t qty) noexcept {
    CancelOrderMsg m{};
    m.msg_type         = 'X';
    m.order_ref_num    = __builtin_bswap64(ref);
    m.cancelled_shares = __builtin_bswap32(qty);
    return m;
}

static ReplaceOrderMsg make_replace(uint64_t old_ref, uint64_t new_ref,
                                    uint32_t price, uint32_t shares) noexcept {
    ReplaceOrderMsg m{};
    m.msg_type               = 'U';
    m.original_order_ref_num = __builtin_bswap64(old_ref);
    m.new_order_ref_num      = __builtin_bswap64(new_ref);
    m.price                  = __builtin_bswap32(price);
    m.shares                 = __builtin_bswap32(shares);
    return m;
}

static void warm_book(OrderBook& book) {
    book.on_system_event(make_sys('O'));
    for (uint64_t i = 1; i <= 100; ++i) {
        book.on_add(make_add('B', kBid - (i % 5) * 100, 200, i));
        book.on_add(make_add('S', kAsk + (i % 5) * 100, 200, i + 1000));
    }
}

static void print_row(const char* name, const bench::Histogram<>& h) {
    printf("  %-28s  p50=%4llu ns  p99=%5llu ns  p999=%6llu ns  n=%llu\n",
           name,
           (unsigned long long)h.p50(),
           (unsigned long long)h.p99(),
           (unsigned long long)h.p999(),
           (unsigned long long)h.count());
}

// ---------------------------------------------------------------------------
// Individual harnesses
// ---------------------------------------------------------------------------

static void bench_add_bid() {
    OrderBook book;
    warm_book(book);
    bench::Histogram<> h;
    uint64_t ref = 1'000'001;
    for (int i = 0; i < kIter; ++i) {
        const auto msg = make_add('B', kBid, 100, ref);
        const uint64_t t0 = bench::now_ns();
        book.on_add(msg);
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
        book.on_delete(make_delete(ref));
        ++ref;
    }
    print_row("AddOrder (bid, best level)", h);
}

static void bench_add_ask() {
    OrderBook book;
    warm_book(book);
    bench::Histogram<> h;
    uint64_t ref = 1'000'001;
    for (int i = 0; i < kIter; ++i) {
        const auto msg = make_add('S', kAsk, 100, ref);
        const uint64_t t0 = bench::now_ns();
        book.on_add(msg);
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
        book.on_delete(make_delete(ref));
        ++ref;
    }
    print_row("AddOrder (ask, best level)", h);
}

static void bench_bbo() {
    OrderBook book;
    warm_book(book);
    bench::Histogram<> h;
    for (int i = 0; i < kIter; ++i) {
        const uint64_t t0 = bench::now_ns();
        volatile auto bbo = book.best_bid_offer();
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
        (void)bbo;
    }
    print_row("BBO query", h);
}

static void bench_delete() {
    static constexpr int kN = 512;
    OrderBook book;
    warm_book(book);
    for (uint64_t i = 200'001; i <= 200'000 + kN; ++i)
        book.on_add(make_add('B', kBid, 100, i));

    bench::Histogram<> h;
    for (int i = 0; i < kIter; ++i) {
        const uint64_t ref = 200'001 + static_cast<uint64_t>(i % kN);
        const auto msg = make_delete(ref);
        const uint64_t t0 = bench::now_ns();
        book.on_delete(msg);
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
        book.on_add(make_add('B', kBid, 100, ref));
    }
    print_row("DeleteOrder", h);
}

static void bench_cancel() {
    static constexpr int kN = 512;
    OrderBook book;
    warm_book(book);
    for (uint64_t i = 200'001; i <= 200'000 + kN; ++i)
        book.on_add(make_add('B', kBid, 1'000'000, i));

    bench::Histogram<> h;
    for (int i = 0; i < kIter; ++i) {
        const uint64_t ref = 200'001 + static_cast<uint64_t>(i % kN);
        const auto msg = make_cancel(ref, 1);
        const uint64_t t0 = bench::now_ns();
        book.on_cancel(msg);
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
    }
    print_row("CancelOrder (partial, -1 share)", h);
}

static void bench_replace() {
    OrderBook book;
    warm_book(book);
    book.on_add(make_add('B', kBid, 100, 300'001));

    bench::Histogram<> h;
    uint64_t old_ref = 300'001;
    uint64_t new_ref = 300'002;
    for (int i = 0; i < kIter; ++i) {
        const auto msg = make_replace(old_ref, new_ref, kBid, 100);
        const uint64_t t0 = bench::now_ns();
        book.on_replace(msg);
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
        old_ref = new_ref;
        ++new_ref;
    }
    print_row("ReplaceOrder (same level)", h);
}

static void bench_parse_add() {
    const auto add = make_add('B', kBid, 100, 42);
    const auto* raw = reinterpret_cast<const uint8_t*>(&add);
    bench::Histogram<> h;
    for (int i = 0; i < kIter; ++i) {
        const uint64_t t0 = bench::now_ns();
        volatile auto parsed = parse_message(raw, sizeof(add));
        const uint64_t t1 = bench::now_ns();
        h.record(t1 - t0);
        (void)parsed;
    }
    print_row("parse_message (AddOrder)", h);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("Latency harness — %d iterations per benchmark\n", kIter);
    printf("Platform: ");
#if defined(__x86_64__) || defined(_M_X64)
    printf("x86-64 (rdtscp + TSC calibration)\n");
#else
    printf("ARM/other (clock_gettime CLOCK_MONOTONIC_RAW)\n");
#endif
    printf("\n");

    // Warm up the timer
    for (int i = 0; i < 10000; ++i) {
        volatile auto _ = bench::now_ns();
        (void)_;
    }

    printf("%-30s  %13s  %14s  %15s  %s\n",
           "Operation", "p50", "p99", "p999", "n");
    printf("%s\n", std::string(82, '-').c_str());

    bench_add_bid();
    bench_add_ask();
    bench_bbo();
    bench_delete();
    bench_cancel();
    bench_replace();
    bench_parse_add();

    return 0;
}
