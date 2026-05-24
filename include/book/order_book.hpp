#pragma once

#include "book/bbo.hpp"
#include "book/order.hpp"
#include "book/price_level.hpp"
#include "itch/messages.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace book {

// Maximum price ticks tracked per side.
// Each PriceLevel is 64 B (one cache line); 8192 levels × 64 B = 512 KB per side.
// The active ~500-tick window (32 KB) fits entirely in L1D.
static constexpr int kLevels = 8192;

// Pre-allocated order pool size — one slot per possible live order.
static constexpr std::size_t kPoolSize = 1 << 20; // 1M orders

// Index semantics (separate reference per side):
//
//   bid_ref_  — ceiling of the bid array.
//   bid_levels_[i] represents price = bid_ref_ - i
//   idx=0 is the highest possible bid (at bid_ref_); higher idx = lower price.
//
//   ask_ref_  — floor of the ask array.
//   ask_levels_[i] represents price = ask_ref_ + i
//   idx=0 is the lowest possible ask (at ask_ref_); higher idx = higher price.
//
// Valid bid prices:  [bid_ref_ - kLevels + 1, bid_ref_]
// Valid ask prices:  [ask_ref_,               ask_ref_ + kLevels - 1]
//
// When a new order falls outside its side's range, that side is re-centred on
// the new price (all live orders on that side are re-inserted at new indices).
class OrderBook {
public:
    explicit OrderBook(std::size_t pool_size = kPoolSize);

    // System Event ('S') — drives session state.
    // 'O' = market open (start accepting orders)
    // 'C' = market close (stop accepting orders)
    // 'H' = trading halt (drain book, stop accepting orders)
    // 'Q' = quote-only period; 'R' = resume
    void on_system_event(const itch::SystemEventMsg& msg) noexcept;

    void on_add    (const itch::AddOrderMsg&              msg) noexcept;
    void on_add    (const itch::AddOrderMPIDMsg&          msg) noexcept;
    void on_execute(const itch::ExecuteOrderMsg&          msg) noexcept;
    void on_execute(const itch::ExecuteOrderWithPriceMsg& msg) noexcept;
    void on_cancel (const itch::CancelOrderMsg&           msg) noexcept;
    void on_delete (const itch::DeleteOrderMsg&           msg) noexcept;
    void on_replace(const itch::ReplaceOrderMsg&          msg) noexcept;

    // O(1) — reads best_bid_idx_ / best_ask_idx_ directly.
    BBO  best_bid_offer() const noexcept;

    std::size_t order_count() const noexcept { return order_map_.size(); }
    bool        has_order(uint64_t ref) const noexcept {
        return order_map_.count(ref) > 0;
    }
    bool is_open() const noexcept { return is_open_; }

    const PriceLevel* bid_level(int idx) const noexcept;
    const PriceLevel* ask_level(int idx) const noexcept;

    uint32_t bid_ref() const noexcept { return bid_ref_; }
    uint32_t ask_ref() const noexcept { return ask_ref_; }

private:
    std::array<PriceLevel, kLevels>          bid_levels_{};
    std::array<PriceLevel, kLevels>          ask_levels_{};
    std::unordered_map<uint64_t, Order*>     order_map_;
    OrderPool                                pool_;

    uint32_t bid_ref_      = 0;   // 0 = not yet initialised
    uint32_t ask_ref_      = 0;
    int      best_bid_idx_ = -1;  // -1 = no bids
    int      best_ask_idx_ = -1;  // -1 = no asks
    bool     is_open_      = false;

    int bid_idx(uint32_t price) const noexcept {
        return static_cast<int>(bid_ref_) - static_cast<int>(price);
    }
    int ask_idx(uint32_t price) const noexcept {
        return static_cast<int>(price) - static_cast<int>(ask_ref_);
    }
    uint32_t price_from_bid_idx(int i) const noexcept {
        return bid_ref_ - static_cast<uint32_t>(i);
    }
    uint32_t price_from_ask_idx(int i) const noexcept {
        return ask_ref_ + static_cast<uint32_t>(i);
    }

    // Insert at a validated index (no range check).
    void insert_at(Order* o, int idx) noexcept;

    // Insert with automatic recentre if out of range.
    void insert_order(Order* o) noexcept;

    // Remove using o->level_idx (O(1), no ref recomputation).
    void remove_order(Order* o) noexcept;

    // Partially execute (reduce shares) without removing; caller removes if zero.
    void reduce_shares(Order* o, uint32_t qty) noexcept;

    // Re-centre one side on a new reference price and rebuild all its orders.
    void recentre_bids(uint32_t new_ref) noexcept;
    void recentre_asks(uint32_t new_ref) noexcept;

    void advance_best_bid() noexcept;
    void advance_best_ask() noexcept;
};

} // namespace book
