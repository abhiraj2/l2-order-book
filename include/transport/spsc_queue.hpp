#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>

namespace transport {

// Single-producer / single-consumer lock-free ring buffer.
//
// head_ and tail_ live on separate cache lines to eliminate false sharing:
// the producer writes tail_, the consumer writes head_. Without the
// padding, every producer push would invalidate the consumer's cache line.
//
// Capacity must be a power of 2; slots are addressed via bitwise AND mask.
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SPSCQueue: Capacity must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue: T must be trivially copyable");

public:
    SPSCQueue() = default;

    // Producer side — returns false if the queue is full.
    bool try_push(const T& item) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next_t = (t + 1) & kMask;
        if (next_t == head_.load(std::memory_order_acquire))
            return false;          // full
        slots_[t] = item;
        tail_.store(next_t, std::memory_order_release);
        return true;
    }

    // Consumer side — returns false if the queue is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire))
            return false;          // empty
        out = slots_[h];
        head_.store((h + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // Approximate size — exact only when called from a single thread.
    std::size_t size_approx() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return (t - h) & kMask;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity>              slots_;
};

} // namespace transport
