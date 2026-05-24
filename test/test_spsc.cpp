#include <gtest/gtest.h>
#include "transport/spsc_queue.hpp"

#include <atomic>
#include <thread>
#include <vector>

using transport::SPSCQueue;

// ---------------------------------------------------------------------------
// Basic single-threaded correctness
// ---------------------------------------------------------------------------
TEST(SPSCQueueTest, PushPop_SingleThread) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.empty());

    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.empty());

    int v;
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 3);
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueTest, FullQueue_ReturnsFalse) {
    SPSCQueue<int, 4> q;
    // Capacity=4 means 3 usable slots (one slot reserved for full/empty distinction)
    EXPECT_TRUE(q.try_push(10));
    EXPECT_TRUE(q.try_push(20));
    EXPECT_TRUE(q.try_push(30));
    EXPECT_FALSE(q.try_push(40)); // queue is full

    int v;
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 10);
    EXPECT_TRUE(q.try_push(40)); // now there's room
}

TEST(SPSCQueueTest, FIFOOrder) {
    SPSCQueue<int, 64> q;
    for (int i = 0; i < 50; ++i) q.try_push(i);
    for (int i = 0; i < 50; ++i) {
        int v;
        EXPECT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
}

// ---------------------------------------------------------------------------
// Capacity is power of 2
// ---------------------------------------------------------------------------
TEST(SPSCQueueTest, CapacityConstant) {
    using Q256  = SPSCQueue<int, 256>;
    using Q1024 = SPSCQueue<int, 1024>;
    EXPECT_EQ(Q256::capacity(),  256u);
    EXPECT_EQ(Q1024::capacity(), 1024u);
}

// ---------------------------------------------------------------------------
// Stress test: 10M messages, two threads, no lost or duplicated items
// ---------------------------------------------------------------------------
TEST(SPSCQueueTest, StressTest_10M_Messages) {
    constexpr int kMessages = 10'000'000;
    constexpr std::size_t kCapacity = 65536;

    SPSCQueue<uint64_t, kCapacity> q;

    std::atomic<bool> done{false};
    uint64_t          consumed_sum = 0;

    std::thread consumer([&] {
        uint64_t count = 0;
        uint64_t sum   = 0;
        while (count < kMessages) {
            uint64_t v;
            if (q.try_pop(v)) {
                sum += v;
                ++count;
            }
        }
        consumed_sum = sum;
        done.store(true, std::memory_order_release);
    });

    uint64_t produced_sum = 0;
    for (int i = 1; i <= kMessages; ++i) {
        uint64_t v = static_cast<uint64_t>(i);
        while (!q.try_push(v)) {} // spin on full — shouldn't happen often
        produced_sum += v;
    }

    consumer.join();

    EXPECT_TRUE(done.load());
    EXPECT_EQ(produced_sum, consumed_sum)
        << "Sum mismatch: lost or duplicated messages";
}

// ---------------------------------------------------------------------------
// Wrap-around: push/pop across the power-of-2 boundary many times
// ---------------------------------------------------------------------------
TEST(SPSCQueueTest, WrapAround) {
    SPSCQueue<int, 8> q;

    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 7; ++i) EXPECT_TRUE(q.try_push(round * 100 + i));
        for (int i = 0; i < 7; ++i) {
            int v;
            EXPECT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, round * 100 + i);
        }
    }
}
