#include "book/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace book {

OrderBook::OrderBook(std::size_t pool_size) : pool_(pool_size) {
    order_map_.reserve(pool_size);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void OrderBook::insert_at(Order* o, int idx) noexcept {
    o->level_idx = idx;
    if (o->side == 'B') {
        bid_levels_[idx].add_order(o);
        if (best_bid_idx_ < 0 || idx < best_bid_idx_)
            best_bid_idx_ = idx;
    } else {
        ask_levels_[idx].add_order(o);
        if (best_ask_idx_ < 0 || idx < best_ask_idx_)
            best_ask_idx_ = idx;
    }
}

void OrderBook::insert_order(Order* o) noexcept {
    if (o->side == 'B') {
        if (bid_ref_ == 0) bid_ref_ = o->price;
        int idx = bid_idx(o->price);
        if (idx < 0 || idx >= kLevels) {
            recentre_bids(o->price);
            idx = bid_idx(o->price); // == 0 after recentre on o->price
        }
        assert(idx >= 0 && idx < kLevels);
        insert_at(o, idx);
    } else {
        if (ask_ref_ == 0) ask_ref_ = o->price;
        int idx = ask_idx(o->price);
        if (idx < 0 || idx >= kLevels) {
            recentre_asks(o->price);
            idx = ask_idx(o->price); // == 0 after recentre on o->price
        }
        assert(idx >= 0 && idx < kLevels);
        insert_at(o, idx);
    }
}

void OrderBook::remove_order(Order* o) noexcept {
    assert(o->level_idx >= 0);
    const int idx = o->level_idx;

    if (o->side == 'B') {
        bid_levels_[idx].remove_order(o);
        if (idx == best_bid_idx_ && bid_levels_[idx].empty())
            advance_best_bid();
    } else {
        ask_levels_[idx].remove_order(o);
        if (idx == best_ask_idx_ && ask_levels_[idx].empty())
            advance_best_ask();
    }
    o->level_idx = -1;
}

void OrderBook::reduce_shares(Order* o, uint32_t qty) noexcept {
    assert(o->level_idx >= 0);
    const int idx = o->level_idx;
    if (o->side == 'B') bid_levels_[idx].reduce_shares(o, qty);
    else                ask_levels_[idx].reduce_shares(o, qty);
}

void OrderBook::advance_best_bid() noexcept {
    for (int i = best_bid_idx_ + 1; i < kLevels; ++i) {
        if (!bid_levels_[i].empty()) { best_bid_idx_ = i; return; }
    }
    best_bid_idx_ = -1;
}

void OrderBook::advance_best_ask() noexcept {
    for (int i = best_ask_idx_ + 1; i < kLevels; ++i) {
        if (!ask_levels_[i].empty()) { best_ask_idx_ = i; return; }
    }
    best_ask_idx_ = -1;
}

// Re-centre the bid side so that new_ref becomes the new ceiling (idx=0).
// All existing bids are re-inserted at their new indices.
// Bids whose prices would now be out of range are silently dropped — this only
// happens if new_ref < some_existing_bid_price, which can't happen because
// we only recentre when new_ref is a bid that was above the current ceiling.
void OrderBook::recentre_bids(uint32_t new_ref) noexcept {
    for (auto& lvl : bid_levels_) lvl = PriceLevel{};
    best_bid_idx_ = -1;
    bid_ref_      = new_ref;

    for (auto& [ref, o] : order_map_) {
        if (o->side != 'B') continue;
        o->next      = nullptr;
        o->prev      = nullptr;
        o->level_idx = -1;
        const int idx = bid_idx(o->price);
        if (idx >= 0 && idx < kLevels)
            insert_at(o, idx);
        // else: price now below the new floor (new_ref - kLevels) — extremely
        // rare; order remains in order_map_ but not in any level.
        // A production system would handle this with a wider array or a
        // hash-indexed overflow map.
    }
}

void OrderBook::recentre_asks(uint32_t new_ref) noexcept {
    for (auto& lvl : ask_levels_) lvl = PriceLevel{};
    best_ask_idx_ = -1;
    ask_ref_      = new_ref;

    for (auto& [ref, o] : order_map_) {
        if (o->side != 'S') continue;
        o->next      = nullptr;
        o->prev      = nullptr;
        o->level_idx = -1;
        const int idx = ask_idx(o->price);
        if (idx >= 0 && idx < kLevels)
            insert_at(o, idx);
    }
}

// ---------------------------------------------------------------------------
// Public message handlers
// ---------------------------------------------------------------------------

void OrderBook::on_system_event(const itch::SystemEventMsg& msg) noexcept {
    switch (msg.event_code) {
    case 'O':  // Market open — begin accepting orders
        is_open_ = true;
        break;
    case 'C':  // Market close — stop accepting new orders
        is_open_ = false;
        break;
    case 'H':  // Trading halt — drain all live orders from the book
        is_open_ = false;
        for (auto& [ref, o] : order_map_) {
            o->next = o->prev = nullptr;
            o->level_idx = -1;
        }
        for (auto& lvl : bid_levels_) lvl = PriceLevel{};
        for (auto& lvl : ask_levels_) lvl = PriceLevel{};
        best_bid_idx_ = -1;
        best_ask_idx_ = -1;
        order_map_.clear();
        // Note: pool_ slots are not freed individually — just reset the free list
        // by reconstructing. In production use a generation counter instead.
        pool_ = OrderPool(pool_.capacity());
        break;
    default:
        break;
    }
}

static Order* make_order(OrderPool& pool,
                         uint64_t ref, uint32_t shares, uint32_t price,
                         char side) noexcept {
    Order* o     = pool.alloc();
    o->order_ref = ref;
    o->shares    = shares;
    o->price     = price;
    o->side      = side;
    return o;
}

void OrderBook::on_add(const itch::AddOrderMsg& msg) noexcept {
    if (!is_open_) return;  // ignore pre/post-market orders

    const uint64_t ref    = itch::be64(msg.order_ref_num);
    const uint32_t shares = itch::be32(msg.shares);
    const uint32_t price  = itch::be32(msg.price);

    Order* o = make_order(pool_, ref, shares, price, msg.buy_sell);
    // Insert before adding to order_map_ so recentre (which iterates order_map_)
    // does not see this order and double-insert it.
    insert_order(o);
    order_map_[ref] = o;
}

void OrderBook::on_add(const itch::AddOrderMPIDMsg& msg) noexcept {
    if (!is_open_) return;

    const uint64_t ref    = itch::be64(msg.order_ref_num);
    const uint32_t shares = itch::be32(msg.shares);
    const uint32_t price  = itch::be32(msg.price);

    Order* o = make_order(pool_, ref, shares, price, msg.buy_sell);
    insert_order(o);
    order_map_[ref] = o;
}

void OrderBook::on_execute(const itch::ExecuteOrderMsg& msg) noexcept {
    const uint64_t ref      = itch::be64(msg.order_ref_num);
    const uint32_t exec_qty = itch::be32(msg.executed_shares);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;
    Order* o = it->second;

    reduce_shares(o, exec_qty);
    if (o->shares == 0) {
        remove_order(o);
        order_map_.erase(it);
        pool_.free(o);
    }
}

void OrderBook::on_execute(const itch::ExecuteOrderWithPriceMsg& msg) noexcept {
    const uint64_t ref      = itch::be64(msg.order_ref_num);
    const uint32_t exec_qty = itch::be32(msg.executed_shares);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;
    Order* o = it->second;

    reduce_shares(o, exec_qty);
    if (o->shares == 0) {
        remove_order(o);
        order_map_.erase(it);
        pool_.free(o);
    }
}

void OrderBook::on_cancel(const itch::CancelOrderMsg& msg) noexcept {
    const uint64_t ref        = itch::be64(msg.order_ref_num);
    const uint32_t cancel_qty = itch::be32(msg.cancelled_shares);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;
    Order* o = it->second;

    reduce_shares(o, cancel_qty);
    if (o->shares == 0) {
        remove_order(o);
        order_map_.erase(it);
        pool_.free(o);
    }
}

void OrderBook::on_delete(const itch::DeleteOrderMsg& msg) noexcept {
    const uint64_t ref = itch::be64(msg.order_ref_num);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;

    Order* o = it->second;
    remove_order(o);
    order_map_.erase(it);
    pool_.free(o);
}

void OrderBook::on_replace(const itch::ReplaceOrderMsg& msg) noexcept {
    const uint64_t old_ref = itch::be64(msg.original_order_ref_num);
    const uint64_t new_ref = itch::be64(msg.new_order_ref_num);
    const uint32_t shares  = itch::be32(msg.shares);
    const uint32_t price   = itch::be32(msg.price);

    auto it = order_map_.find(old_ref);
    if (it == order_map_.end()) return;

    Order*     old_o = it->second;
    const char side  = old_o->side;

    remove_order(old_o);
    order_map_.erase(it);
    pool_.free(old_o);

    Order* new_o = make_order(pool_, new_ref, shares, price, side);
    insert_order(new_o);
    order_map_[new_ref] = new_o;
}

// ---------------------------------------------------------------------------
// BBO
// ---------------------------------------------------------------------------

BBO OrderBook::best_bid_offer() const noexcept {
    BBO bbo;
    if (best_bid_idx_ >= 0) {
        bbo.bid_price = price_from_bid_idx(best_bid_idx_);
        bbo.bid_qty   = static_cast<uint32_t>(bid_levels_[best_bid_idx_].total_qty);
    }
    if (best_ask_idx_ >= 0) {
        bbo.ask_price = price_from_ask_idx(best_ask_idx_);
        bbo.ask_qty   = static_cast<uint32_t>(ask_levels_[best_ask_idx_].total_qty);
    }
    return bbo;
}

const PriceLevel* OrderBook::bid_level(int idx) const noexcept {
    if (idx < 0 || idx >= kLevels) return nullptr;
    return &bid_levels_[idx];
}

const PriceLevel* OrderBook::ask_level(int idx) const noexcept {
    if (idx < 0 || idx >= kLevels) return nullptr;
    return &ask_levels_[idx];
}

} // namespace book
