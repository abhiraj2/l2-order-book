#pragma once

#include "book/bbo.hpp"
#include "book/order_book.hpp"

#include <cstdint>
#include <cmath>

namespace book {

// Read-only analytics computed from the live book state.
// All methods are O(N levels) at most; call from the analytics thread only —
// never from the book thread's hot path.
class Analytics {
public:
    explicit Analytics(const OrderBook& book) : book_(book) {}

    // (best_ask + best_bid) / 2  in raw ITCH price units.
    double mid_price() const noexcept {
        const BBO bbo = book_.best_bid_offer();
        if (!bbo.valid()) return 0.0;
        return static_cast<double>(bbo.bid_price + bbo.ask_price) / 2.0;
    }

    // best_ask - best_bid  in raw ITCH price units (divide by 10000 for dollars).
    double spread() const noexcept {
        const BBO bbo = book_.best_bid_offer();
        if (!bbo.valid()) return 0.0;
        return static_cast<double>(bbo.ask_price - bbo.bid_price);
    }

    // (bid_qty - ask_qty) / (bid_qty + ask_qty)  ∈ [-1, 1].
    // Positive = more bid-side pressure; negative = more ask-side pressure.
    double order_flow_imbalance() const noexcept {
        const BBO bbo = book_.best_bid_offer();
        const double bid = static_cast<double>(bbo.bid_qty);
        const double ask = static_cast<double>(bbo.ask_qty);
        const double total = bid + ask;
        if (total == 0.0) return 0.0;
        return (bid - ask) / total;
    }

    // Volume-weighted average price across the top N levels on one side.
    // Returns 0 if the book has no levels on that side.
    double vwap(char side, int levels) const noexcept {
        double price_qty_sum = 0.0;
        double qty_sum       = 0.0;
        const int n = (levels > book::kLevels) ? book::kLevels : levels;

        if (side == 'B') {
            for (int i = 0; i < n; ++i) {
                const PriceLevel* lvl = book_.bid_level(i);
                if (!lvl || lvl->empty()) continue;
                const double price = static_cast<double>(book_.bid_ref() - static_cast<uint32_t>(i));
                const double qty   = static_cast<double>(lvl->total_qty);
                price_qty_sum += price * qty;
                qty_sum       += qty;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const PriceLevel* lvl = book_.ask_level(i);
                if (!lvl || lvl->empty()) continue;
                const double price = static_cast<double>(book_.ask_ref() + static_cast<uint32_t>(i));
                const double qty   = static_cast<double>(lvl->total_qty);
                price_qty_sum += price * qty;
                qty_sum       += qty;
            }
        }

        return qty_sum > 0.0 ? price_qty_sum / qty_sum : 0.0;
    }

    // Depth snapshot: total visible quantity on one side across N levels.
    uint64_t visible_qty(char side, int levels) const noexcept {
        uint64_t total = 0;
        const int n = (levels > book::kLevels) ? book::kLevels : levels;
        for (int i = 0; i < n; ++i) {
            const PriceLevel* lvl = (side == 'B')
                                  ? book_.bid_level(i)
                                  : book_.ask_level(i);
            if (lvl) total += lvl->total_qty;
        }
        return total;
    }

private:
    const OrderBook& book_;
};

} // namespace book
