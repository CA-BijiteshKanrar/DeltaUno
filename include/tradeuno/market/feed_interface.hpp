#pragma once

#include "tradeuno/core/types.hpp"
#include <functional>

namespace tradeuno::market {

using TickCallback = std::function<void(const core::MarketTick&)>;

class IMarketFeed {
public:
    virtual ~IMarketFeed() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void register_callback(TickCallback cb) = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;
};

} // namespace tradeuno::market
