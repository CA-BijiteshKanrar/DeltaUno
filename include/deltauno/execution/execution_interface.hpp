#pragma once

#include "deltauno/core/types.hpp"
#include <functional>

namespace deltauno::execution {

using OrderFillCallback = std::function<void(const core::Order&, const core::TradeAuditEntry&)>;

class IExecutionGateway {
public:
    virtual ~IExecutionGateway() = default;
    virtual bool place_order(const core::Order& order) = 0;
    virtual bool cancel_order(uint64_t client_order_id) = 0;
    virtual void register_fill_callback(OrderFillCallback cb) = 0;
    virtual void restore_open_position(const core::Position& /* pos */) {}
};

} // namespace deltauno::execution
