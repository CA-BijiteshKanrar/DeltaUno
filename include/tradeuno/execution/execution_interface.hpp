#pragma once

#include "tradeuno/core/types.hpp"
#include <functional>

namespace tradeuno::execution {

using OrderFillCallback = std::function<void(const core::Order&, const core::TradeAuditEntry&)>;

class IExecutionGateway {
public:
    virtual ~IExecutionGateway() = default;
    virtual bool place_order(const core::Order& order) = 0;
    virtual bool cancel_order(uint64_t client_order_id) = 0;
    virtual void register_fill_callback(OrderFillCallback cb) = 0;
};

} // namespace tradeuno::execution
