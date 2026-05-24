#pragma once

#include <cstdint>

namespace book {

// Best Bid/Offer snapshot — a read-only copy published by the book thread.
// Prices are in raw ITCH fixed-point units (divide by 10000 for dollars).
struct BBO {
    uint32_t bid_price = 0;
    uint32_t bid_qty   = 0;
    uint32_t ask_price = 0;
    uint32_t ask_qty   = 0;

    bool valid() const noexcept {
        return bid_price > 0 && ask_price > 0 && bid_price < ask_price;
    }

    double spread_ticks() const noexcept {
        return valid() ? static_cast<double>(ask_price - bid_price) : 0.0;
    }

    double mid_price() const noexcept {
        return valid() ? static_cast<double>(bid_price + ask_price) / 2.0 : 0.0;
    }
};

} // namespace book
