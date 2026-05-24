#pragma once

#include <cstdint>

// All ITCH fields are big-endian. Access via the be16/be32/be64 helpers below,
// never by direct field read on a little-endian host.

namespace itch {

// ---------------------------------------------------------------------------
// Byte-swap helpers (big-endian network order → host)
// ---------------------------------------------------------------------------
inline uint16_t be16(uint16_t v) noexcept { return __builtin_bswap16(v); }
inline uint32_t be32(uint32_t v) noexcept { return __builtin_bswap32(v); }
inline uint64_t be64(uint64_t v) noexcept { return __builtin_bswap64(v); }

// ITCH timestamps are 6-byte (48-bit) big-endian nanoseconds since midnight.
inline uint64_t read_ts(const uint8_t* ts) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | ts[i];
    return v;
}

// Fixed-point price: ITCH price / 10000 = dollars
inline double to_dollars(uint32_t price) noexcept {
    return static_cast<double>(price) / 10000.0;
}

// ---------------------------------------------------------------------------
// Packed structs — reinterpret_cast directly onto the raw buffer (zero-copy).
// All multi-byte fields are in network (big-endian) order.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

// 'S' — System Event
struct SystemEventMsg {
    char     msg_type;        // 'S'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    char     event_code;      // 'O'=market open, 'C'=market close, etc.
};

// 'A' — Add Order (no MPID)
struct AddOrderMsg {
    char     msg_type;        // 'A'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;   // unique order reference
    char     buy_sell;        // 'B' or 'S'
    uint32_t shares;
    char     stock[8];        // left-justified, space-padded
    uint32_t price;           // fixed-point ×10000
};

// 'F' — Add Order with MPID
struct AddOrderMPIDMsg {
    char     msg_type;        // 'F'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;
    char     buy_sell;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
    char     attribution[4];  // market participant ID
};

// 'E' — Order Executed
struct ExecuteOrderMsg {
    char     msg_type;        // 'E'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
};

// 'C' — Order Executed With Price (auction cross — different execution price)
struct ExecuteOrderWithPriceMsg {
    char     msg_type;        // 'C'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
    char     printable;       // 'Y' or 'N'
    uint32_t execution_price;
};

// 'X' — Order Cancel (partial cancel — reduces shares)
struct CancelOrderMsg {
    char     msg_type;        // 'X'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;
    uint32_t cancelled_shares;
};

// 'D' — Order Delete (full cancel)
struct DeleteOrderMsg {
    char     msg_type;        // 'D'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;
};

// 'U' — Order Replace (delete + new order in one message)
struct ReplaceOrderMsg {
    char     msg_type;        // 'U'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t original_order_ref_num;
    uint64_t new_order_ref_num;
    uint32_t shares;
    uint32_t price;
};

// 'P' — Trade Message (non-cross; informational only, not a resting order)
struct TradeMsg {
    char     msg_type;        // 'P'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref_num;
    char     buy_sell;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
    uint64_t match_number;
};

#pragma pack(pop)

} // namespace itch
