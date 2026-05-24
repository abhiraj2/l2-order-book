#include <gtest/gtest.h>
#include "book/lobster_validator.hpp"
#include "book/analytics.hpp"
#include "itch/messages.hpp"

#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Helpers to build synthetic ITCH binary data in a temp file
// ---------------------------------------------------------------------------
static uint16_t h2be16(uint16_t v) { return __builtin_bswap16(v); }
static uint32_t h2be32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t h2be64(uint64_t v) { return __builtin_bswap64(v); }

static void write_ts(uint8_t* ts, uint64_t ns) {
    for (int i = 5; i >= 0; --i) { ts[i] = ns & 0xFF; ns >>= 8; }
}

// Write a length-prefixed ITCH message to a buffer.
static void append_msg(std::vector<uint8_t>& buf, const void* msg, uint16_t len) {
    const uint16_t be_len = h2be16(len);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&be_len);
    buf.push_back(p[0]);
    buf.push_back(p[1]);
    const uint8_t* m = reinterpret_cast<const uint8_t*>(msg);
    buf.insert(buf.end(), m, m + len);
}

// Write buffer to a temp file; returns the path.
static std::string write_temp(const std::vector<uint8_t>& data) {
    char path[] = "/tmp/itch_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return "";
    write(fd, data.data(), data.size());
    close(fd);
    return path;
}

// Write a LOBSTER orderbook CSV line (10 levels) and message CSV line.
static void write_lobster_files(const std::string& ob_path,
                                const std::string& msg_path,
                                double ts_s,
                                const uint32_t* ask_px, const uint32_t* ask_sz,
                                const uint32_t* bid_px, const uint32_t* bid_sz,
                                int depth = 10) {
    // Message CSV: "time,type,id,size,price,direction\n"
    FILE* mf = fopen(msg_path.c_str(), "w");
    fprintf(mf, "%.9f,1,1,100,1500000,1\n", ts_s);
    fclose(mf);

    // Orderbook CSV: ask_px_1,ask_sz_1,bid_px_1,bid_sz_1,...\n
    FILE* of = fopen(ob_path.c_str(), "w");
    for (int i = 0; i < depth; ++i) {
        // LOBSTER prices are dollars; our stored values are ×10000
        fprintf(of, "%.4f,%.0f,%.4f,%.0f",
                ask_px[i] / 10000.0, (double)ask_sz[i],
                bid_px[i] / 10000.0, (double)bid_sz[i]);
        if (i + 1 < depth) fputc(',', of);
    }
    fputc('\n', of);
    fclose(of);
}

// ---------------------------------------------------------------------------
// Synthetic correctness test
// ---------------------------------------------------------------------------
// Builds a minimal ITCH file, a matching LOBSTER snapshot, and verifies 0
// mismatches.  This always runs — it requires no external data.
TEST(LobsterValidateTest, SyntheticPerfectMatch) {
    // Build ITCH stream:
    //   SystemEvent 'O' at t=0
    //   AddOrder bid  $150.00 (1500000), 500 shares, ref=1
    //   AddOrder ask  $150.01 (1500100), 300 shares, ref=2
    // Snapshot is taken after both adds.

    const uint64_t snap_ns = 100'000'000ULL; // 100 ms since midnight
    const double   snap_s  = snap_ns / 1e9;

    std::vector<uint8_t> itch_data;

    // SystemEvent 'O'
    itch::SystemEventMsg sys{};
    sys.msg_type = 'S';
    write_ts(sys.timestamp, 1'000'000ULL);
    sys.event_code = 'O';
    append_msg(itch_data, &sys, sizeof(sys));

    // Add bid: ref=1, price=1500000, shares=500
    itch::AddOrderMsg bid{};
    bid.msg_type       = 'A';
    bid.order_ref_num  = h2be64(1);
    bid.buy_sell       = 'B';
    bid.shares         = h2be32(500);
    bid.price          = h2be32(1'500'000);
    write_ts(bid.timestamp, snap_ns - 10);
    append_msg(itch_data, &bid, sizeof(bid));

    // Add ask: ref=2, price=1500100, shares=300
    itch::AddOrderMsg ask{};
    ask.msg_type       = 'A';
    ask.order_ref_num  = h2be64(2);
    ask.buy_sell       = 'S';
    ask.shares         = h2be32(300);
    ask.price          = h2be32(1'500'100);
    write_ts(ask.timestamp, snap_ns);
    append_msg(itch_data, &ask, sizeof(ask));

    const std::string itch_path = write_temp(itch_data);
    ASSERT_FALSE(itch_path.empty());

    // Build LOBSTER snapshot matching the expected book state.
    // Level 0: bid=1500000/500, ask=1500100/300. Levels 1-9: zeros.
    uint32_t bid_px[10]{}, bid_sz[10]{}, ask_px[10]{}, ask_sz[10]{};
    bid_px[0] = 1'500'000; bid_sz[0] = 500;
    ask_px[0] = 1'500'100; ask_sz[0] = 300;

    const std::string ob_path  = "/tmp/lobster_ob_XXXXXX";
    const std::string msg_path = "/tmp/lobster_msg_XXXXXX";
    // Use actual temp files
    char ob_buf[64], msg_buf[64];
    snprintf(ob_buf,  sizeof(ob_buf),  "/tmp/lobster_ob_%d.csv",  (int)getpid());
    snprintf(msg_buf, sizeof(msg_buf), "/tmp/lobster_msg_%d.csv", (int)getpid());

    write_lobster_files(ob_buf, msg_buf, snap_s,
                        ask_px, ask_sz, bid_px, bid_sz);

    book::LobsterValidator validator;
    ASSERT_TRUE(validator.load(ob_buf, msg_buf));
    EXPECT_EQ(validator.snapshots().size(), 1u);

    book::OrderBook book;
    std::vector<book::Mismatch> mismatches;
    const std::size_t n = validator.validate(book, itch_path, &mismatches);

    for (const auto& m : mismatches) {
        fprintf(stderr, "  mismatch side=%c level=%d our_qty=%u ref_qty=%u\n",
                m.side, m.level, m.our_qty, m.ref_qty);
    }

    EXPECT_EQ(n, 0u) << "Expected 0 mismatches against synthetic LOBSTER snapshot";

    remove(itch_path.c_str());
    remove(ob_buf);
    remove(msg_buf);
}

// ---------------------------------------------------------------------------
// Analytics test (does not need LOBSTER data)
// ---------------------------------------------------------------------------
TEST(AnalyticsTest, BasicMetrics) {
    book::OrderBook book{64};

    itch::SystemEventMsg open{};
    open.msg_type = 'S'; open.event_code = 'O';
    book.on_system_event(open);

    auto make_add = [](char side, uint32_t price, uint32_t shares, uint64_t ref) {
        itch::AddOrderMsg m{};
        m.msg_type      = 'A';
        m.order_ref_num = __builtin_bswap64(ref);
        m.buy_sell      = side;
        m.shares        = __builtin_bswap32(shares);
        m.price         = __builtin_bswap32(price);
        return m;
    };

    book.on_add(make_add('B', 1'500'000, 200, 1)); // bid $150.00
    book.on_add(make_add('S', 1'500'100, 100, 2)); // ask $150.01

    book::Analytics ana(book);

    EXPECT_DOUBLE_EQ(ana.spread(), 100.0);   // 1 tick = 100 units
    EXPECT_DOUBLE_EQ(ana.mid_price(), 1'500'050.0);

    // OFI: (bid_qty - ask_qty) / (bid_qty + ask_qty) = (200-100)/(200+100) = 1/3
    const double ofi = ana.order_flow_imbalance();
    EXPECT_NEAR(ofi, 1.0 / 3.0, 1e-9);

    // VWAP with 1 level on each side
    EXPECT_DOUBLE_EQ(ana.vwap('B', 1), 1'500'000.0);
    EXPECT_DOUBLE_EQ(ana.vwap('S', 1), 1'500'100.0);

    // Visible qty
    EXPECT_EQ(ana.visible_qty('B', 5), 200u);
    EXPECT_EQ(ana.visible_qty('S', 5), 100u);
}

// ---------------------------------------------------------------------------
// Real-data test: skips if LOBSTER + ITCH files are not in data/
// ---------------------------------------------------------------------------
TEST(LobsterValidateTest, RealData_DISABLED) {
    // To enable: place matching files in data/ and remove _DISABLED suffix.
    // Expected filenames (adjust symbol/date):
    //   data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
    //   data/AAPL_2012-06-21_34200000_57600000_message_10.csv
    //   data/01302012.NASDAQ_ITCH50   (decompressed)
    const std::string ob_file  = "data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv";
    const std::string msg_file = "data/AAPL_2012-06-21_34200000_57600000_message_10.csv";
    const std::string itch_bin = "data/01302012.NASDAQ_ITCH50";

    FILE* f = fopen(ob_file.c_str(), "r");
    if (!f) { GTEST_SKIP() << "LOBSTER data not present in data/ — skipping"; }
    fclose(f);

    book::LobsterValidator validator;
    ASSERT_TRUE(validator.load(ob_file, msg_file));

    book::OrderBook book;
    std::vector<book::Mismatch> mismatches;
    const std::size_t n = validator.validate(book, itch_bin, &mismatches);

    if (!mismatches.empty()) {
        fprintf(stderr, "First 5 mismatches:\n");
        for (std::size_t i = 0; i < mismatches.size() && i < 5; ++i) {
            const auto& m = mismatches[i];
            fprintf(stderr, "  ts=%llu side=%c lvl=%d our=%u ref=%u\n",
                    (unsigned long long)m.timestamp_ns,
                    m.side, m.level, m.our_qty, m.ref_qty);
        }
    }

    EXPECT_EQ(n, 0u) << n << " mismatches vs LOBSTER ground truth";
}
