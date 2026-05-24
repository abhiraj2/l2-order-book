#include <benchmark/benchmark.h>

#include "book/order_book.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "transport/spsc_queue.hpp"

using namespace book;
using namespace itch;

static constexpr uint32_t kBid = 1'500'000; // $150.00
static constexpr uint32_t kAsk = 1'500'100; // $150.01

// ---------------------------------------------------------------------------
// Message builders (matching ITCH big-endian encoding)
// ---------------------------------------------------------------------------
static SystemEventMsg make_sys(char code) noexcept {
    SystemEventMsg m{};
    m.msg_type   = 'S';
    m.event_code = code;
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

// Warm a fresh book: open session, seed 100 bids + 100 asks so ref prices
// are locked in and the L1D window is hot.
static void warm_book(OrderBook& book) {
    book.on_system_event(make_sys('O'));
    for (uint64_t i = 1; i <= 100; ++i) {
        book.on_add(make_add('B', kBid - (i % 5) * 100, 200, i));
        book.on_add(make_add('S', kAsk + (i % 5) * 100, 200, i + 1000));
    }
}

// ---------------------------------------------------------------------------
// Order-book benchmarks
// ---------------------------------------------------------------------------

// Add bid at best price, immediately delete to keep steady state.
static void BM_Book_AddBid(benchmark::State& state) {
    OrderBook book;
    warm_book(book);
    uint64_t ref = 100'001;
    for (auto _ : state) {
        book.on_add(make_add('B', kBid, 100, ref));
        book.on_delete(make_delete(ref));
        ++ref;
    }
}
BENCHMARK(BM_Book_AddBid);

// Add ask at best price, immediately delete.
static void BM_Book_AddAsk(benchmark::State& state) {
    OrderBook book;
    warm_book(book);
    uint64_t ref = 100'001;
    for (auto _ : state) {
        book.on_add(make_add('S', kAsk, 100, ref));
        book.on_delete(make_delete(ref));
        ++ref;
    }
}
BENCHMARK(BM_Book_AddAsk);

// BBO query — the single hottest path: one pointer dereference.
static void BM_Book_BBO(benchmark::State& state) {
    OrderBook book;
    warm_book(book);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.best_bid_offer());
    }
}
BENCHMARK(BM_Book_BBO);

// Delete an order, immediately re-add it to maintain book depth.
static void BM_Book_Delete(benchmark::State& state) {
    static constexpr int kN = 512;
    OrderBook book;
    warm_book(book);
    for (uint64_t i = 200'001; i <= 200'000 + kN; ++i)
        book.on_add(make_add('B', kBid, 100, i));

    int i = 0;
    for (auto _ : state) {
        const uint64_t ref = 200'001 + static_cast<uint64_t>(i % kN);
        book.on_delete(make_delete(ref));
        book.on_add(make_add('B', kBid, 100, ref));
        ++i;
    }
}
BENCHMARK(BM_Book_Delete);

// Partial cancel — trim 1 share; orders have 1M shares so they never expire.
static void BM_Book_CancelPartial(benchmark::State& state) {
    static constexpr int kN = 512;
    OrderBook book;
    warm_book(book);
    for (uint64_t i = 200'001; i <= 200'000 + kN; ++i)
        book.on_add(make_add('B', kBid, 1'000'000, i));

    int i = 0;
    for (auto _ : state) {
        const uint64_t ref = 200'001 + static_cast<uint64_t>(i % kN);
        book.on_cancel(make_cancel(ref, 1));
        ++i;
    }
}
BENCHMARK(BM_Book_CancelPartial);

// Replace at same price level (ref changes, price unchanged).
static void BM_Book_Replace_SameLevel(benchmark::State& state) {
    OrderBook book;
    warm_book(book);
    book.on_add(make_add('B', kBid, 100, 300'001));

    uint64_t old_ref = 300'001;
    uint64_t new_ref = 300'002;
    for (auto _ : state) {
        book.on_replace(make_replace(old_ref, new_ref, kBid, 100));
        old_ref = new_ref;
        ++new_ref;
    }
}
BENCHMARK(BM_Book_Replace_SameLevel);

// Replace at a different price level (triggers best-price update).
static void BM_Book_Replace_NewLevel(benchmark::State& state) {
    OrderBook book;
    warm_book(book);
    book.on_add(make_add('B', kBid - 200, 100, 300'001));

    uint64_t old_ref = 300'001;
    uint64_t new_ref = 300'002;
    int      tick    = 0;
    for (auto _ : state) {
        // Alternate between kBid-200 and kBid to exercise level transitions.
        const uint32_t price = (tick % 2 == 0) ? kBid : kBid - 200;
        book.on_replace(make_replace(old_ref, new_ref, price, 100));
        old_ref = new_ref;
        ++new_ref;
        ++tick;
    }
}
BENCHMARK(BM_Book_Replace_NewLevel);

// ---------------------------------------------------------------------------
// Parser benchmark
// ---------------------------------------------------------------------------
static void BM_Parse_AddOrder(benchmark::State& state) {
    const auto msg = make_add('B', kBid, 100, 42);
    const auto* raw = reinterpret_cast<const uint8_t*>(&msg);
    for (auto _ : state) {
        benchmark::DoNotOptimize(parse_message(raw, sizeof(msg)));
    }
}
BENCHMARK(BM_Parse_AddOrder);

static void BM_Parse_DeleteOrder(benchmark::State& state) {
    const auto msg = make_delete(42);
    const auto* raw = reinterpret_cast<const uint8_t*>(&msg);
    for (auto _ : state) {
        benchmark::DoNotOptimize(parse_message(raw, sizeof(msg)));
    }
}
BENCHMARK(BM_Parse_DeleteOrder);

// ---------------------------------------------------------------------------
// SPSC queue — single-threaded push+pop round-trip (measures queue overhead).
// ---------------------------------------------------------------------------
static void BM_SPSC_RoundTrip(benchmark::State& state) {
    MessageQueue q;
    const ParsedMessage msg = make_add('B', kBid, 100, 1);
    ParsedMessage out;
    for (auto _ : state) {
        q.try_push(msg);
        benchmark::DoNotOptimize(q.try_pop(out));
    }
}
BENCHMARK(BM_SPSC_RoundTrip);

// ---------------------------------------------------------------------------
// Mixed workload — realistic message distribution:
//   70% add (immediately deleted to hold steady state)
//   10% BBO query
//   10% partial cancel
//   10% replace
// ---------------------------------------------------------------------------
static void BM_MixedWorkload(benchmark::State& state) {
    static constexpr int kN = 256;
    OrderBook book;
    warm_book(book);
    // Extra pool of cycling orders
    for (uint64_t i = 400'001; i <= 400'000 + kN; ++i)
        book.on_add(make_add('B', kBid, 1'000'000, i));

    uint64_t add_ref     = 500'001;
    uint64_t replace_ref = 600'001;  // current live replace target
    book.on_add(make_add('B', kBid, 100, replace_ref));

    int i = 0;
    for (auto _ : state) {
        const int op = i % 10;
        if (op < 7) {
            // 70%: add + immediate delete
            book.on_add(make_add('B', kBid, 100, add_ref));
            book.on_delete(make_delete(add_ref));
            ++add_ref;
        } else if (op == 7) {
            // 10%: BBO query
            benchmark::DoNotOptimize(book.best_bid_offer());
        } else if (op == 8) {
            // 10%: partial cancel from cycling pool
            const uint64_t ref = 400'001 + static_cast<uint64_t>(i % kN);
            book.on_cancel(make_cancel(ref, 1));
        } else {
            // 10%: replace (same level)
            const uint64_t next = replace_ref + 1;
            book.on_replace(make_replace(replace_ref, next, kBid, 100));
            replace_ref = next;
        }
        ++i;
    }
}
BENCHMARK(BM_MixedWorkload);

BENCHMARK_MAIN();
