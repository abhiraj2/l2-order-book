#include <gtest/gtest.h>
#include "itch/parser.hpp"
#include "itch/messages.hpp"

#include <cstring>

using namespace itch;

// Build a raw big-endian ITCH byte buffer from a packed struct.
template <typename T>
static std::vector<uint8_t> to_raw(const T& msg) {
    std::vector<uint8_t> buf(sizeof(T));
    std::memcpy(buf.data(), &msg, sizeof(T));
    return buf;
}

// ---------------------------------------------------------------------------
// Helpers to construct big-endian test messages
// ---------------------------------------------------------------------------
static uint16_t h2be16(uint16_t v) { return __builtin_bswap16(v); }
static uint32_t h2be32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t h2be64(uint64_t v) { return __builtin_bswap64(v); }

static void write_ts(uint8_t* ts, uint64_t ns) {
    for (int i = 5; i >= 0; --i) { ts[i] = ns & 0xFF; ns >>= 8; }
}

// ---------------------------------------------------------------------------
// SystemEvent
// ---------------------------------------------------------------------------
TEST(ParserTest, SystemEvent_Open) {
    SystemEventMsg raw{};
    raw.msg_type = 'S';
    raw.stock_locate     = h2be16(1);
    raw.tracking_number  = h2be16(0);
    write_ts(raw.timestamp, 34'200'000'000'000ULL); // 09:30:00 in ns
    raw.event_code = 'O';

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<SystemEventMsg>(msg));
    const auto& s = std::get<SystemEventMsg>(msg);
    EXPECT_EQ(s.msg_type, 'S');
    EXPECT_EQ(s.event_code, 'O');
    EXPECT_EQ(be16(s.stock_locate), 1u);
    EXPECT_EQ(read_ts(s.timestamp), 34'200'000'000'000ULL);
}

// ---------------------------------------------------------------------------
// AddOrder
// ---------------------------------------------------------------------------
TEST(ParserTest, AddOrder_Buy) {
    AddOrderMsg raw{};
    raw.msg_type        = 'A';
    raw.stock_locate    = h2be16(42);
    raw.tracking_number = h2be16(7);
    write_ts(raw.timestamp, 100'000'000ULL);
    raw.order_ref_num   = h2be64(999'001);
    raw.buy_sell        = 'B';
    raw.shares          = h2be32(500);
    std::memcpy(raw.stock, "AAPL    ", 8);
    raw.price           = h2be32(1'505'000); // $150.50 × 10000

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<AddOrderMsg>(msg));
    const auto& a = std::get<AddOrderMsg>(msg);
    EXPECT_EQ(a.msg_type, 'A');
    EXPECT_EQ(a.buy_sell, 'B');
    EXPECT_EQ(be32(a.shares), 500u);
    EXPECT_EQ(be64(a.order_ref_num), 999'001u);
    EXPECT_EQ(be32(a.price), 1'505'000u);
    EXPECT_EQ(be16(a.stock_locate), 42u);
    EXPECT_EQ(read_ts(a.timestamp), 100'000'000ULL);
}

TEST(ParserTest, AddOrder_Sell) {
    AddOrderMsg raw{};
    raw.msg_type        = 'A';
    raw.stock_locate    = h2be16(1);
    raw.tracking_number = 0;
    write_ts(raw.timestamp, 200'000'000ULL);
    raw.order_ref_num   = h2be64(12345);
    raw.buy_sell        = 'S';
    raw.shares          = h2be32(100);
    std::memcpy(raw.stock, "MSFT    ", 8);
    raw.price           = h2be32(3'000'000); // $300.00

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<AddOrderMsg>(msg));
    EXPECT_EQ(std::get<AddOrderMsg>(msg).buy_sell, 'S');
    EXPECT_EQ(be32(std::get<AddOrderMsg>(msg).shares), 100u);
}

// ---------------------------------------------------------------------------
// DeleteOrder
// ---------------------------------------------------------------------------
TEST(ParserTest, DeleteOrder) {
    DeleteOrderMsg raw{};
    raw.msg_type        = 'D';
    raw.stock_locate    = h2be16(1);
    raw.tracking_number = 0;
    write_ts(raw.timestamp, 300'000'000ULL);
    raw.order_ref_num   = h2be64(999'001);

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<DeleteOrderMsg>(msg));
    EXPECT_EQ(be64(std::get<DeleteOrderMsg>(msg).order_ref_num), 999'001u);
}

// ---------------------------------------------------------------------------
// CancelOrder
// ---------------------------------------------------------------------------
TEST(ParserTest, CancelOrder) {
    CancelOrderMsg raw{};
    raw.msg_type         = 'X';
    raw.stock_locate     = h2be16(1);
    raw.tracking_number  = 0;
    write_ts(raw.timestamp, 400'000'000ULL);
    raw.order_ref_num    = h2be64(555);
    raw.cancelled_shares = h2be32(200);

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<CancelOrderMsg>(msg));
    const auto& c = std::get<CancelOrderMsg>(msg);
    EXPECT_EQ(be64(c.order_ref_num), 555u);
    EXPECT_EQ(be32(c.cancelled_shares), 200u);
}

// ---------------------------------------------------------------------------
// ReplaceOrder
// ---------------------------------------------------------------------------
TEST(ParserTest, ReplaceOrder) {
    ReplaceOrderMsg raw{};
    raw.msg_type               = 'U';
    raw.stock_locate           = h2be16(1);
    raw.tracking_number        = 0;
    write_ts(raw.timestamp, 500'000'000ULL);
    raw.original_order_ref_num = h2be64(100);
    raw.new_order_ref_num      = h2be64(200);
    raw.shares                 = h2be32(300);
    raw.price                  = h2be32(2'000'000);

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<ReplaceOrderMsg>(msg));
    const auto& r = std::get<ReplaceOrderMsg>(msg);
    EXPECT_EQ(be64(r.original_order_ref_num), 100u);
    EXPECT_EQ(be64(r.new_order_ref_num), 200u);
    EXPECT_EQ(be32(r.shares), 300u);
    EXPECT_EQ(be32(r.price), 2'000'000u);
}

// ---------------------------------------------------------------------------
// ExecuteOrder
// ---------------------------------------------------------------------------
TEST(ParserTest, ExecuteOrder) {
    ExecuteOrderMsg raw{};
    raw.msg_type        = 'E';
    raw.stock_locate    = h2be16(1);
    raw.tracking_number = 0;
    write_ts(raw.timestamp, 600'000'000ULL);
    raw.order_ref_num   = h2be64(999'001);
    raw.executed_shares = h2be32(100);
    raw.match_number    = h2be64(1234567890ULL);

    auto buf = to_raw(raw);
    auto msg = parse_message(buf.data(), static_cast<uint16_t>(buf.size()));

    ASSERT_TRUE(std::holds_alternative<ExecuteOrderMsg>(msg));
    const auto& e = std::get<ExecuteOrderMsg>(msg);
    EXPECT_EQ(be64(e.order_ref_num), 999'001u);
    EXPECT_EQ(be32(e.executed_shares), 100u);
}

// ---------------------------------------------------------------------------
// Unknown message type → monostate
// ---------------------------------------------------------------------------
TEST(ParserTest, UnknownMsgType_IsMonostate) {
    uint8_t buf[1] = { 'Z' };
    auto msg = parse_message(buf, 1);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(msg));
}

TEST(ParserTest, NullBuffer_IsMonostate) {
    auto msg = parse_message(nullptr, 0);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(msg));
}

TEST(ParserTest, TruncatedMessage_IsMonostate) {
    // Only 3 bytes — too short for any message struct
    uint8_t buf[3] = { 'A', 0x00, 0x01 };
    auto msg = parse_message(buf, 3);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(msg));
}

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------
TEST(ParserTest, ReadTimestamp_RoundTrip) {
    const uint64_t expected = 34'200'000'000'001ULL;
    uint8_t ts[6];
    write_ts(ts, expected);
    EXPECT_EQ(read_ts(ts), expected);
}
