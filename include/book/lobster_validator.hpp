#pragma once

#include "book/order_book.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace book {

// One snapshot row from a LOBSTER _orderbook_N.csv file.
// Prices are stored in ITCH fixed-point units (×10000 = dollars).
struct LobsterSnapshot {
    uint64_t timestamp_ns = 0;  // nanoseconds since midnight

    static constexpr int kDepth = 10;
    uint32_t ask_price[kDepth]{};
    uint32_t ask_qty  [kDepth]{};
    uint32_t bid_price[kDepth]{};
    uint32_t bid_qty  [kDepth]{};
};

// Mismatch at a single snapshot checkpoint.
struct Mismatch {
    uint64_t timestamp_ns;
    char     side;        // 'B' or 'S'
    int      level;       // 0-based depth level
    uint32_t our_price;
    uint32_t our_qty;
    uint32_t ref_price;   // from LOBSTER
    uint32_t ref_qty;     // from LOBSTER
};

// Replays an ITCH binary file through an OrderBook and compares the resulting
// book state against LOBSTER ground-truth snapshots at each checkpoint.
//
// LOBSTER files:
//   _orderbook_N.csv  — N-level depth snapshot after each event
//   _message_N.csv    — one row per event; column 0 is the event timestamp
//                       (fractional seconds since midnight)
//
// LOBSTER prices are floating-point dollars; we convert to ITCH ×10000.
class LobsterValidator {
public:
    // Load snapshots from the two CSV files.
    // Returns false if either file cannot be opened or is empty.
    bool load(const std::string& orderbook_csv,
              const std::string& message_csv) noexcept;

    // Replay itch_file through book; compare state at each loaded snapshot.
    // Returns total mismatch count (0 = perfect).
    // If out_mismatches is non-null, appends each mismatch detail.
    std::size_t validate(OrderBook& book,
                         const std::string& itch_file,
                         std::vector<Mismatch>* out_mismatches = nullptr) noexcept;

    const std::vector<LobsterSnapshot>& snapshots() const noexcept {
        return snapshots_;
    }

private:
    std::vector<LobsterSnapshot> snapshots_;
    std::vector<uint64_t>        snap_ts_;   // timestamps from message CSV

    bool load_messages (const std::string& path) noexcept;
    bool load_orderbook(const std::string& path) noexcept;

    std::size_t compare_snapshot(const OrderBook& book,
                                 const LobsterSnapshot& snap,
                                 std::vector<Mismatch>* out) const noexcept;
};

} // namespace book
