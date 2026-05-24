#include "book/lobster_validator.hpp"
#include "itch/parser.hpp"
#include "transport/pcap_replay.hpp"

#include <cstdio>
#include <variant>

namespace book {

// ---------------------------------------------------------------------------
// load_messages: read timestamps from LOBSTER _message_N.csv
// ---------------------------------------------------------------------------
// LOBSTER message CSV columns (comma-separated, no header):
//   time(s), type, order_id, size, price, direction
// We only need column 0 — fractional seconds since midnight.
bool LobsterValidator::load_messages(const std::string& path) noexcept {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "LobsterValidator: cannot open message file '%s'\n",
                path.c_str());
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        double time_s = 0.0;
        if (sscanf(line, "%lf,", &time_s) == 1)
            snap_ts_.push_back(static_cast<uint64_t>(time_s * 1e9 + 0.5));
    }
    fclose(f);
    return !snap_ts_.empty();
}

// ---------------------------------------------------------------------------
// load_orderbook: read snapshots from LOBSTER _orderbook_N.csv
// ---------------------------------------------------------------------------
// Each line has 4×N fields: ask_px_1,ask_sz_1,bid_px_1,bid_sz_1,...
// Prices are floating-point dollars; we convert to ITCH ×10000 integers.
bool LobsterValidator::load_orderbook(const std::string& path) noexcept {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "LobsterValidator: cannot open orderbook file '%s'\n",
                path.c_str());
        return false;
    }

    char line[4096];
    std::size_t row = 0;
    while (fgets(line, sizeof(line), f) && row < snap_ts_.size()) {
        LobsterSnapshot snap;
        snap.timestamp_ns = snap_ts_[row];

        char* p = line;
        bool  ok = true;
        for (int lvl = 0; lvl < LobsterSnapshot::kDepth && ok; ++lvl) {
            double   ask_px, ask_sz, bid_px, bid_sz;
            int      n = 0;
            if (sscanf(p, "%lf,%lf,%lf,%lf%n",
                       &ask_px, &ask_sz, &bid_px, &bid_sz, &n) != 4) {
                ok = false; break;
            }
            snap.ask_price[lvl] = static_cast<uint32_t>(ask_px * 10000.0 + 0.5);
            snap.ask_qty  [lvl] = static_cast<uint32_t>(ask_sz + 0.5);
            snap.bid_price[lvl] = static_cast<uint32_t>(bid_px * 10000.0 + 0.5);
            snap.bid_qty  [lvl] = static_cast<uint32_t>(bid_sz + 0.5);
            p += n;
            if (*p == ',') ++p;
        }
        if (ok) snapshots_.push_back(snap);
        ++row;
    }
    fclose(f);
    return !snapshots_.empty();
}

bool LobsterValidator::load(const std::string& orderbook_csv,
                            const std::string& message_csv) noexcept {
    return load_messages(message_csv) && load_orderbook(orderbook_csv);
}

// ---------------------------------------------------------------------------
// compare_snapshot
// ---------------------------------------------------------------------------
std::size_t LobsterValidator::compare_snapshot(
    const OrderBook& book,
    const LobsterSnapshot& snap,
    std::vector<Mismatch>* out) const noexcept
{
    std::size_t mismatches = 0;

    for (int lvl = 0; lvl < LobsterSnapshot::kDepth; ++lvl) {
        // --- Bid side ---
        const uint32_t ref_bid_price = snap.bid_price[lvl];
        const uint32_t ref_bid_qty   = snap.bid_qty[lvl];

        uint32_t our_bid_qty = 0;
        if (ref_bid_price > 0 && book.bid_ref() >= ref_bid_price) {
            const int idx = static_cast<int>(book.bid_ref() - ref_bid_price);
            const PriceLevel* pl = book.bid_level(idx);
            if (pl) our_bid_qty = static_cast<uint32_t>(pl->total_qty);
        }

        if (our_bid_qty != ref_bid_qty) {
            ++mismatches;
            if (out) out->push_back({snap.timestamp_ns, 'B', lvl,
                                     ref_bid_price, our_bid_qty,
                                     ref_bid_price, ref_bid_qty});
        }

        // --- Ask side ---
        const uint32_t ref_ask_price = snap.ask_price[lvl];
        const uint32_t ref_ask_qty   = snap.ask_qty[lvl];

        uint32_t our_ask_qty = 0;
        if (ref_ask_price > 0 && ref_ask_price >= book.ask_ref()) {
            const int idx = static_cast<int>(ref_ask_price - book.ask_ref());
            const PriceLevel* pl = book.ask_level(idx);
            if (pl) our_ask_qty = static_cast<uint32_t>(pl->total_qty);
        }

        if (our_ask_qty != ref_ask_qty) {
            ++mismatches;
            if (out) out->push_back({snap.timestamp_ns, 'S', lvl,
                                     ref_ask_price, our_ask_qty,
                                     ref_ask_price, ref_ask_qty});
        }
    }
    return mismatches;
}

// ---------------------------------------------------------------------------
// validate: replay ITCH file and compare at each snapshot timestamp
// ---------------------------------------------------------------------------
std::size_t LobsterValidator::validate(OrderBook& book,
                                       const std::string& itch_file,
                                       std::vector<Mismatch>* out) noexcept {
    if (snapshots_.empty()) return 0;

    transport::ITCHFileReader reader(itch_file);
    std::size_t total_mismatches = 0;
    std::size_t snap_idx = 0;

    uint16_t       len = 0;
    const uint8_t* raw = nullptr;

    while ((raw = reader.next_message(len)) != nullptr && snap_idx < snapshots_.size()) {
        const itch::ParsedMessage msg = itch::parse_message(raw, len);

        // Dispatch to book
        std::visit([&book](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, itch::SystemEventMsg>)
                book.on_system_event(m);
            else if constexpr (std::is_same_v<T, itch::AddOrderMsg>)
                book.on_add(m);
            else if constexpr (std::is_same_v<T, itch::AddOrderMPIDMsg>)
                book.on_add(m);
            else if constexpr (std::is_same_v<T, itch::ExecuteOrderMsg>)
                book.on_execute(m);
            else if constexpr (std::is_same_v<T, itch::ExecuteOrderWithPriceMsg>)
                book.on_execute(m);
            else if constexpr (std::is_same_v<T, itch::CancelOrderMsg>)
                book.on_cancel(m);
            else if constexpr (std::is_same_v<T, itch::DeleteOrderMsg>)
                book.on_delete(m);
            else if constexpr (std::is_same_v<T, itch::ReplaceOrderMsg>)
                book.on_replace(m);
        }, msg);

        // Check if we've reached the next snapshot timestamp
        const uint64_t msg_ts = std::visit([](const auto& m) -> uint64_t {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, std::monostate>) return 0;
            else return itch::read_ts(m.timestamp);
        }, msg);

        while (snap_idx < snapshots_.size() &&
               msg_ts >= snapshots_[snap_idx].timestamp_ns) {
            total_mismatches += compare_snapshot(book, snapshots_[snap_idx], out);
            ++snap_idx;
        }
    }

    // Compare any remaining snapshots after EOF
    while (snap_idx < snapshots_.size()) {
        total_mismatches += compare_snapshot(book, snapshots_[snap_idx], out);
        ++snap_idx;
    }

    return total_mismatches;
}

} // namespace book
