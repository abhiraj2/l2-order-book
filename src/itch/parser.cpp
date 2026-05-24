#include "itch/parser.hpp"

#include <cstring>

namespace itch {

// Safely copy a packed struct out of the raw buffer.
template <typename T>
static T copy_msg(const uint8_t* data) noexcept {
    T msg;
    std::memcpy(&msg, data, sizeof(T));
    return msg;
}

ParsedMessage parse_message(const uint8_t* data, uint16_t len) noexcept {
    if (!data || len == 0) return std::monostate{};

    switch (static_cast<char>(data[0])) {
    case 'S':
        if (len >= sizeof(SystemEventMsg))
            return copy_msg<SystemEventMsg>(data);
        break;
    case 'A':
        if (len >= sizeof(AddOrderMsg))
            return copy_msg<AddOrderMsg>(data);
        break;
    case 'F':
        if (len >= sizeof(AddOrderMPIDMsg))
            return copy_msg<AddOrderMPIDMsg>(data);
        break;
    case 'E':
        if (len >= sizeof(ExecuteOrderMsg))
            return copy_msg<ExecuteOrderMsg>(data);
        break;
    case 'C':
        if (len >= sizeof(ExecuteOrderWithPriceMsg))
            return copy_msg<ExecuteOrderWithPriceMsg>(data);
        break;
    case 'X':
        if (len >= sizeof(CancelOrderMsg))
            return copy_msg<CancelOrderMsg>(data);
        break;
    case 'D':
        if (len >= sizeof(DeleteOrderMsg))
            return copy_msg<DeleteOrderMsg>(data);
        break;
    case 'U':
        if (len >= sizeof(ReplaceOrderMsg))
            return copy_msg<ReplaceOrderMsg>(data);
        break;
    case 'P':
        if (len >= sizeof(TradeMsg))
            return copy_msg<TradeMsg>(data);
        break;
    default:
        break;
    }
    return std::monostate{};
}

void parse_and_push(const uint8_t* data, uint16_t len,
                    MessageQueue& queue) noexcept {
    const ParsedMessage msg = parse_message(data, len);
    // Spin until space is available (parser should not outrun the book thread).
    while (!queue.try_push(msg)) {
        // tight spin — in production pin threads and size the queue
        // large enough that this never fires
    }
}

} // namespace itch
