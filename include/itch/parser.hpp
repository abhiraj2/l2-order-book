#pragma once

#include "itch/messages.hpp"
#include "transport/spsc_queue.hpp"

#include <cstdint>
#include <variant>

namespace itch {

// Tagged union of all message types the book cares about.
// std::monostate represents an unknown / ignored message type.
using ParsedMessage = std::variant<
    std::monostate,
    SystemEventMsg,
    AddOrderMsg,
    AddOrderMPIDMsg,
    ExecuteOrderMsg,
    ExecuteOrderWithPriceMsg,
    CancelOrderMsg,
    DeleteOrderMsg,
    ReplaceOrderMsg,
    TradeMsg
>;

// Queue type shared between parser thread and book thread.
// 65536 slots × sizeof(ParsedMessage) ≈ 65536 × ~88 B ≈ 5.5 MB
static constexpr std::size_t kQueueCapacity = 65536;
using MessageQueue = transport::SPSCQueue<ParsedMessage, kQueueCapacity>;

// Parse one raw ITCH message (pointer into the raw buffer, big-endian) into a
// ParsedMessage.  The caller is responsible for keeping the raw buffer alive
// for the duration of the call.  The resulting ParsedMessage owns a copy of
// all fields (no pointer into the raw buffer is retained).
ParsedMessage parse_message(const uint8_t* data, uint16_t len) noexcept;

// Convenience: parse and push into a queue.  Spins on a full queue.
void parse_and_push(const uint8_t* data, uint16_t len,
                    MessageQueue& queue) noexcept;

} // namespace itch
