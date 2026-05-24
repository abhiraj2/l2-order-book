#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <vector>

namespace book {

// A single resting order in the book.
// Uses an intrusive doubly-linked list so splice/remove are O(1) with no
// extra heap allocation.  next/prev are only valid while the order is live
// inside a PriceLevel; they are zeroed on alloc and on free.
struct Order {
    uint64_t order_ref  = 0;
    uint32_t shares     = 0;
    uint32_t price      = 0;   // raw ITCH fixed-point
    char     side       = 0;   // 'B' or 'S'
    int      level_idx  = -1;  // index into bid_levels_ or ask_levels_; cached to
                               // avoid recomputing after a recentre
    Order*   next       = nullptr;
    Order*   prev       = nullptr;
};

// Pre-allocated slab of Order objects.  alloc()/free() are O(1) via an
// intrusive free-list threaded through Order::next.  No syscall or heap
// interaction after construction.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : pool_(capacity) {
        // Build free list: pool_[0] → pool_[1] → ... → pool_[n-1] → nullptr
        for (std::size_t i = 0; i + 1 < capacity; ++i)
            pool_[i].next = &pool_[i + 1];
        free_head_ = &pool_[0];
    }

    Order* alloc() noexcept {
        assert(free_head_ && "OrderPool exhausted");
        Order* o    = free_head_;
        free_head_  = o->next;
        o->next     = nullptr;
        o->prev     = nullptr;
        return o;
    }

    void free(Order* o) noexcept {
        assert(o);
        o->order_ref  = 0;
        o->shares     = 0;
        o->price      = 0;
        o->side       = 0;
        o->level_idx  = -1;
        o->prev       = nullptr;
        o->next       = free_head_;
        free_head_   = o;
    }

    std::size_t capacity() const noexcept { return pool_.size(); }

private:
    std::vector<Order> pool_;
    Order*             free_head_ = nullptr;
};

} // namespace book
