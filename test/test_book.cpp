#include <gtest/gtest.h>
#include "book/order_book.hpp"
#include "itch/messages.hpp"

#include <cstring>
#include <vector>
#include <random>

using namespace book;
using namespace itch;

// ---------------------------------------------------------------------------
// Test fixture and message builders
// ---------------------------------------------------------------------------
class BookTest : public ::testing::Test {
protected:
    OrderBook book{512};
    uint64_t  next_ref_ = 1;

    void SetUp() override {
        // Simulate market open so on_add accepts orders.
        itch::SystemEventMsg open{};
        open.msg_type   = 'S';
        open.event_code = 'O';
        book.on_system_event(open);
    }

    AddOrderMsg make_add(char side, uint32_t price_raw, uint32_t shares) {
        AddOrderMsg m{};
        m.msg_type       = 'A';
        m.stock_locate   = __builtin_bswap16(1);
        m.order_ref_num  = __builtin_bswap64(next_ref_++);
        m.buy_sell       = side;
        m.shares         = __builtin_bswap32(shares);
        m.price          = __builtin_bswap32(price_raw);
        return m;
    }

    DeleteOrderMsg make_delete(uint64_t ref) {
        DeleteOrderMsg m{};
        m.msg_type      = 'D';
        m.order_ref_num = __builtin_bswap64(ref);
        return m;
    }

    CancelOrderMsg make_cancel(uint64_t ref, uint32_t qty) {
        CancelOrderMsg m{};
        m.msg_type         = 'X';
        m.order_ref_num    = __builtin_bswap64(ref);
        m.cancelled_shares = __builtin_bswap32(qty);
        return m;
    }

    ExecuteOrderMsg make_execute(uint64_t ref, uint32_t qty) {
        ExecuteOrderMsg m{};
        m.msg_type        = 'E';
        m.order_ref_num   = __builtin_bswap64(ref);
        m.executed_shares = __builtin_bswap32(qty);
        m.match_number    = 0;
        return m;
    }

    ReplaceOrderMsg make_replace(uint64_t old_ref, uint32_t price_raw,
                                 uint32_t shares) {
        ReplaceOrderMsg m{};
        m.msg_type               = 'U';
        m.original_order_ref_num = __builtin_bswap64(old_ref);
        m.new_order_ref_num      = __builtin_bswap64(next_ref_++);
        m.price                  = __builtin_bswap32(price_raw);
        m.shares                 = __builtin_bswap32(shares);
        return m;
    }

    // Assert invariants after every operation.
    void check_invariants() {
        BBO bbo = book.best_bid_offer();

        if (bbo.bid_price > 0 && bbo.ask_price > 0)
            EXPECT_LT(bbo.bid_price, bbo.ask_price) << "crossed book";
    }
};

// Use prices within kLevels (8192) of each other.
// For a $150 stock: 1 tick = $0.01 = 100 price units.
// We keep spreads to ≤100 ticks (= 10,000 units) but use tight realistic values.
// bid=$150.00=1,500,000  ask=$150.01=1,500,100  (1 tick / $0.01 spread)
static constexpr uint32_t kBid = 1'500'000;
static constexpr uint32_t kAsk = 1'500'100;

// ---------------------------------------------------------------------------
// Session state (SystemEvent)
// ---------------------------------------------------------------------------
TEST_F(BookTest, SystemEvent_PreOpen_OrdersIgnored) {
    OrderBook closed_book{16}; // never opened
    itch::SystemEventMsg open{};
    open.msg_type = 'S'; open.event_code = 'O';
    // do NOT call on_system_event — book stays closed
    closed_book.on_add(make_add('B', kBid, 100));
    EXPECT_EQ(closed_book.order_count(), 0u);
    EXPECT_EQ(closed_book.best_bid_offer().bid_price, 0u);
}

TEST_F(BookTest, SystemEvent_Close_StopsNewOrders) {
    book.on_add(make_add('B', kBid, 100)); // accepted (book is open from SetUp)
    EXPECT_EQ(book.order_count(), 1u);

    itch::SystemEventMsg close{};
    close.msg_type = 'S'; close.event_code = 'C';
    book.on_system_event(close);

    book.on_add(make_add('B', kBid, 200)); // rejected
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, SystemEvent_Halt_DrainsBook) {
    book.on_add(make_add('B', kBid, 100));
    book.on_add(make_add('S', kAsk, 200));
    EXPECT_EQ(book.order_count(), 2u);

    itch::SystemEventMsg halt{};
    halt.msg_type = 'S'; halt.event_code = 'H';
    book.on_system_event(halt);

    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.is_open());
    EXPECT_EQ(book.best_bid_offer().bid_price, 0u);
}

// ---------------------------------------------------------------------------
// Basic add
// ---------------------------------------------------------------------------
TEST_F(BookTest, AddBid_BBO_Updated) {
    book.on_add(make_add('B', kBid, 100));
    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.bid_price, kBid);
    EXPECT_EQ(bbo.bid_qty,   100u);
    EXPECT_EQ(bbo.ask_price, 0u);
    check_invariants();
}

TEST_F(BookTest, AddAsk_BBO_Updated) {
    book.on_add(make_add('S', kAsk, 200));
    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.ask_price, kAsk);
    EXPECT_EQ(bbo.ask_qty,   200u);
    EXPECT_EQ(bbo.bid_price, 0u);
    check_invariants();
}

TEST_F(BookTest, AddBidAndAsk_BBO_Valid) {
    book.on_add(make_add('B', kBid, 100));
    book.on_add(make_add('S', kAsk, 200));
    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.bid_price, kBid);
    EXPECT_EQ(bbo.ask_price, kAsk);
    EXPECT_TRUE(bbo.valid());
    check_invariants();
}

// ---------------------------------------------------------------------------
// Best bid tracks the highest bid price
// ---------------------------------------------------------------------------
TEST_F(BookTest, BestBid_IsHighestPrice) {
    book.on_add(make_add('B', kBid - 200, 50));
    book.on_add(make_add('B', kBid,       100)); // best
    book.on_add(make_add('B', kBid - 100, 75));
    EXPECT_EQ(book.best_bid_offer().bid_price, kBid);
    check_invariants();
}

TEST_F(BookTest, BestAsk_IsLowestPrice) {
    book.on_add(make_add('S', kAsk + 200, 50));
    book.on_add(make_add('S', kAsk,       100)); // best
    book.on_add(make_add('S', kAsk + 100, 75));
    EXPECT_EQ(book.best_bid_offer().ask_price, kAsk);
    check_invariants();
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------
TEST_F(BookTest, DeleteBestBid_AdvancesToNext) {
    book.on_add(make_add('B', kBid,       100)); // best, ref=1
    book.on_add(make_add('B', kBid - 100, 50));  // second, ref=2

    book.on_delete(make_delete(1));
    EXPECT_EQ(book.best_bid_offer().bid_price, kBid - 100);
    EXPECT_EQ(book.order_count(), 1u);
    check_invariants();
}

TEST_F(BookTest, DeleteLastOrder_BBOEmpty) {
    book.on_add(make_add('B', kBid, 100)); // ref=1
    book.on_delete(make_delete(1));
    EXPECT_EQ(book.best_bid_offer().bid_price, 0u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, DeleteUnknownRef_NoOp) {
    book.on_add(make_add('B', kBid, 100));
    book.on_delete(make_delete(9999));
    EXPECT_EQ(book.order_count(), 1u);
}

// ---------------------------------------------------------------------------
// Cancel (partial)
// ---------------------------------------------------------------------------
TEST_F(BookTest, PartialCancel_ReducesQty) {
    book.on_add(make_add('B', kBid, 100)); // ref=1
    book.on_cancel(make_cancel(1, 40));
    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.bid_qty, 60u);
    EXPECT_EQ(book.order_count(), 1u);
    check_invariants();
}

TEST_F(BookTest, FullCancel_RemovesOrder) {
    book.on_add(make_add('B', kBid, 100)); // ref=1
    book.on_cancel(make_cancel(1, 100));
    EXPECT_EQ(book.best_bid_offer().bid_price, 0u);
    EXPECT_EQ(book.order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------
TEST_F(BookTest, PartialExecute_ReducesQty) {
    book.on_add(make_add('B', kBid, 100)); // ref=1
    book.on_execute(make_execute(1, 30));
    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.bid_qty, 70u);
    EXPECT_EQ(book.order_count(), 1u);
    check_invariants();
}

TEST_F(BookTest, FullExecute_RemovesOrder) {
    book.on_add(make_add('B', kBid, 100)); // ref=1
    book.on_execute(make_execute(1, 100));
    EXPECT_EQ(book.best_bid_offer().bid_price, 0u);
    EXPECT_EQ(book.order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------
TEST_F(BookTest, Replace_UpdatesPriceAndQty) {
    book.on_add(make_add('B', kBid, 100)); // ref=1
    book.on_replace(make_replace(1, kBid + 50, 50)); // new ref=2, price +0.5 ticks

    EXPECT_FALSE(book.has_order(1));
    EXPECT_TRUE(book.has_order(2));
    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.bid_price, kBid + 50);
    EXPECT_EQ(bbo.bid_qty, 50u);
    EXPECT_EQ(book.order_count(), 1u);
    check_invariants();
}

TEST_F(BookTest, Replace_PriceDowngrade_BestBidUpdates) {
    book.on_add(make_add('B', kBid,       100)); // ref=1, best
    book.on_add(make_add('B', kBid - 100, 50));  // ref=2, second

    // Replace best bid with a price below the second bid
    book.on_replace(make_replace(1, kBid - 200, 100)); // new ref=3

    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.bid_price, kBid - 100); // ref=2 is now best
    check_invariants();
}

// ---------------------------------------------------------------------------
// Order count invariant
// ---------------------------------------------------------------------------
TEST_F(BookTest, OrderCount_TracksAddAndRemove) {
    for (int i = 0; i < 10; ++i)
        book.on_add(make_add('B', kBid - static_cast<uint32_t>(i) * 10, 10));
    EXPECT_EQ(book.order_count(), 10u);

    for (uint64_t ref = 1; ref <= 5; ++ref) book.on_delete(make_delete(ref));
    EXPECT_EQ(book.order_count(), 5u);
}

// ---------------------------------------------------------------------------
// Multiple orders at the same level — qty accumulates
// ---------------------------------------------------------------------------
TEST_F(BookTest, SameLevel_QtyAccumulates) {
    book.on_add(make_add('S', kAsk, 100));
    book.on_add(make_add('S', kAsk, 200));
    book.on_add(make_add('S', kAsk, 300));

    BBO bbo = book.best_bid_offer();
    EXPECT_EQ(bbo.ask_price, kAsk);
    EXPECT_EQ(bbo.ask_qty, 600u);
    EXPECT_EQ(book.order_count(), 3u);
    check_invariants();
}

// ---------------------------------------------------------------------------
// Stress: random add/cancel/delete sequence — invariants hold throughout
// ---------------------------------------------------------------------------
TEST_F(BookTest, Stress_RandomSequence) {
    constexpr int kOps = 50'000;
    std::mt19937  rng(42);

    // Bids are always below kBid, asks always above kBid, so the book is
    // never artificially crossed and the no-crossed-book invariant holds.
    std::uniform_int_distribution<int>      bid_delta(-40, -1);  // negative → below kBid
    std::uniform_int_distribution<int>      ask_delta(1,   40);  // positive → above kBid
    std::uniform_int_distribution<uint32_t> qty_dist(1, 1000);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      op_dist(0, 2);

    std::vector<uint64_t> live_refs;
    live_refs.reserve(kOps);

    for (int i = 0; i < kOps; ++i) {
        const int op = live_refs.empty() ? 0 : op_dist(rng);

        if (op == 0) {
            const char     side  = side_dist(rng) ? 'B' : 'S';
            const int      delta = (side == 'B') ? bid_delta(rng) : ask_delta(rng);
            const uint32_t price = static_cast<uint32_t>(
                static_cast<int>(kBid) + delta * 100);
            const uint32_t qty = qty_dist(rng);
            book.on_add(make_add(side, price, qty));
            live_refs.push_back(next_ref_ - 1);
        } else if (op == 1 && !live_refs.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_refs.size() - 1);
            book.on_cancel(make_cancel(live_refs[pick(rng)], 1));
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, live_refs.size() - 1);
            const std::size_t idx = pick(rng);
            book.on_delete(make_delete(live_refs[idx]));
            live_refs.erase(live_refs.begin() + static_cast<long>(idx));
        }

        check_invariants();
    }
}
