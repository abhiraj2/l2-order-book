#pragma once

#include "book/order.hpp"

#include <cstdint>
#include <cassert>

namespace book {

// One price level in the order book.
//
// alignas(64): each PriceLevel occupies exactly one cache line.  With 8192
// levels per side the bid array is 512 KB and the ask array another 512 KB —
// the active ~500-tick window (32 KB per side) fits entirely in L1D.
//
// Orders within a level are stored as an intrusive doubly-linked FIFO list
// (head = oldest, tail = newest) so add/remove are both O(1) with no heap
// allocation.
struct alignas(64) PriceLevel {
    uint64_t total_qty   = 0;
    uint32_t order_count = 0;
    Order*   head        = nullptr;  // front of FIFO (first to execute)
    Order*   tail        = nullptr;  // back of FIFO  (last to arrive)

    bool empty() const noexcept { return order_count == 0; }

    // Append order to the back of the FIFO.
    void add_order(Order* o) noexcept {
        assert(o);
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head       = o;
        tail = o;
        total_qty   += o->shares;
        ++order_count;
    }

    // Unlink and remove a specific order (O(1) via doubly-linked list).
    void remove_order(Order* o) noexcept {
        assert(o);
        assert(order_count > 0);
        assert(total_qty >= o->shares);

        if (o->prev) o->prev->next = o->next;
        else         head          = o->next;

        if (o->next) o->next->prev = o->prev;
        else         tail          = o->prev;

        total_qty   -= o->shares;
        --order_count;
        o->next = o->prev = nullptr;
    }

    // Reduce shares of an existing order (partial cancel / partial execute).
    // Does NOT remove the order even if shares reach 0 — caller decides.
    void reduce_shares(Order* o, uint32_t by) noexcept {
        assert(o && by <= o->shares && by <= total_qty);
        o->shares  -= by;
        total_qty  -= by;
    }
};

static_assert(sizeof(PriceLevel) <= 64,
              "PriceLevel exceeds one cache line — add padding or shrink fields");

} // namespace book
