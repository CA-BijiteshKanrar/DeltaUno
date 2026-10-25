#pragma once

#include "tradeuno/core/types.hpp"
#include "tradeuno/market/order_book.hpp"
#include <optional>

namespace tradeuno::strategy {

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual void on_tick(const core::MarketTick& tick, const market::OrderBook& book) = 0;
    [[nodiscard]] virtual std::optional<core::TradingSignal> generate_signal(const market::OrderBook& book) = 0;
    virtual void on_trade_fill(const core::TradeAuditEntry& trade) = 0;
};

} // namespace tradeuno::strategy
