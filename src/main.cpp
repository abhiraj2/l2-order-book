#include "book/order_book.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "transport/pcap_replay.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <variant>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// ---------------------------------------------------------------------------
// Thread affinity (Linux only; silently skipped on macOS)
// ---------------------------------------------------------------------------
static void pin_thread(std::thread& t, int core) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(t.native_handle(), sizeof(cpuset), &cpuset);
#else
    (void)t; (void)core;
#endif
}

// ---------------------------------------------------------------------------
// Book dispatcher — routes a ParsedMessage variant to the right handler
// ---------------------------------------------------------------------------
static void dispatch(book::OrderBook& book, const itch::ParsedMessage& msg) {
    std::visit([&book](const auto& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, itch::SystemEventMsg>)
            book.on_system_event(m);
        else if constexpr (std::is_same_v<T, itch::AddOrderMsg>)
            book.on_add(m);
        else if constexpr (std::is_same_v<T, itch::AddOrderMPIDMsg>)
            book.on_add(m);
        else if constexpr (std::is_same_v<T, itch::ExecuteOrderMsg>)
            book.on_execute(m);
        else if constexpr (std::is_same_v<T, itch::ExecuteOrderWithPriceMsg>)
            book.on_execute(m);
        else if constexpr (std::is_same_v<T, itch::CancelOrderMsg>)
            book.on_cancel(m);
        else if constexpr (std::is_same_v<T, itch::DeleteOrderMsg>)
            book.on_delete(m);
        else if constexpr (std::is_same_v<T, itch::ReplaceOrderMsg>)
            book.on_replace(m);
        // SystemEventMsg and TradeMsg are ignored at this layer for now.
    }, msg);
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void usage(const char* argv0) {
    fprintf(stderr, "Usage: %s --replay <itch-binary-file>\n", argv0);
    fprintf(stderr, "  The file must be a raw (decompressed) Nasdaq ITCH 5.0\n");
    fprintf(stderr, "  length-prefixed binary stream.\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* replay_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
            replay_path = argv[++i];
    }

    if (!replay_path) { usage(argv[0]); return 1; }

    // --- Shared state -------------------------------------------------------
    itch::MessageQueue queue;

    std::atomic<bool>     parse_done{false};
    std::atomic<uint64_t> msgs_parsed{0};
    std::atomic<uint64_t> msgs_booked{0};

    // --- Parser thread ------------------------------------------------------
    std::thread parser_thread([&] {
        transport::ITCHFileReader reader(replay_path);
        uint64_t count = 0;
        uint16_t len   = 0;

        const uint8_t* raw;
        while ((raw = reader.next_message(len)) != nullptr) {
            itch::parse_and_push(raw, len, queue);
            ++count;
        }

        msgs_parsed.store(count, std::memory_order_release);
        parse_done.store(true, std::memory_order_release);
    });

    // --- Book thread --------------------------------------------------------
    book::OrderBook book;

    const auto t_start = std::chrono::steady_clock::now();

    std::thread book_thread([&] {
        uint64_t count = 0;
        itch::ParsedMessage msg;

        while (true) {
            if (queue.try_pop(msg)) {
                dispatch(book, msg);
                ++count;
            } else if (parse_done.load(std::memory_order_acquire) && queue.empty()) {
                break;
            }
        }

        msgs_booked.store(count, std::memory_order_release);
    });

    pin_thread(parser_thread, 0);
    pin_thread(book_thread,   1);

    parser_thread.join();
    book_thread.join();

    const auto t_end = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration<double>(t_end - t_start).count();
    const double throughput = static_cast<double>(msgs_booked.load()) / elapsed_s / 1e6;

    // --- Results ------------------------------------------------------------
    const book::BBO bbo = book.best_bid_offer();

    printf("\n=== Replay complete ===\n");
    printf("  Messages parsed : %llu\n",  (unsigned long long)msgs_parsed.load());
    printf("  Messages booked : %llu\n",  (unsigned long long)msgs_booked.load());
    printf("  Elapsed         : %.3f s\n", elapsed_s);
    printf("  Throughput      : %.2f M msgs/s\n", throughput);
    printf("  Live orders     : %zu\n",    book.order_count());
    if (bbo.bid_price || bbo.ask_price) {
        printf("  Best bid        : $%.4f  qty %u\n",
               bbo.bid_price / 10000.0, bbo.bid_qty);
        printf("  Best ask        : $%.4f  qty %u\n",
               bbo.ask_price / 10000.0, bbo.ask_qty);
        if (bbo.valid())
            printf("  Spread          : $%.4f\n", (bbo.ask_price - bbo.bid_price) / 10000.0);
    } else {
        printf("  BBO             : (empty)\n");
    }

    return 0;
}
