#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <chrono>
#include <cmath>

namespace tradeuno::core {

// Fixed-point price representation in Paise (1 INR = 100 Paise)
// Eliminates floating-point inaccuracies on the hot path
using Price = int64_t;
using Quantity = uint32_t;

[[nodiscard]] constexpr Price to_paise(double inr) noexcept {
    return static_cast<Price>(std::round(inr * 100.0));
}

[[nodiscard]] constexpr double to_inr(Price paise) noexcept {
    return static_cast<double>(paise) / 100.0;
}

enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

[[nodiscard]] constexpr const char* side_to_string(Side s) noexcept {
    return (s == Side::BUY) ? "BUY" : "SELL";
}

enum class OrderType : uint8_t {
    MARKET = 0,
    LIMIT = 1,
    STOP_LOSS = 2,
    STOP_LOSS_LIMIT = 3
};

enum class OrderStatus : uint8_t {
    PENDING = 0,
    ACCEPTED = 1,
    FILLED = 2,
    PARTIALLY_FILLED = 3,
    CANCELLED = 4,
    REJECTED = 5
};

[[nodiscard]] constexpr const char* order_status_to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::PENDING: return "PENDING";
        case OrderStatus::ACCEPTED: return "ACCEPTED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

enum class AssetClass : uint8_t {
    EQUITY_CASH = 0,
    EQUITY_INTRADAY_MARGIN = 1, // MIS (5x broker intraday margin)
    EQUITY_FUTURES = 2,         // Standard lot size futures
    CRYPTO_PERPETUAL = 3
};

struct DepthLevel {
    Price price{0};
    Quantity quantity{0};
    uint32_t orders_count{0};
};

struct MarketTick {
    uint32_t token{0};
    char symbol[16]{0};
    uint64_t timestamp_ns{0};
    Price last_price{0};
    Quantity last_quantity{0};
    uint64_t total_volume{0};
    Price upper_circuit{0};
    Price lower_circuit{0};
    std::array<DepthLevel, 5> bids{};
    std::array<DepthLevel, 5> asks{};
};

enum class SignalSource : uint8_t {
    INTERNAL_MOMENTUM = 0,
    EXTERNAL_WEBHOOK = 1
};

struct TradingSignal {
    uint64_t timestamp_ns{0};
    uint32_t token{0};
    char symbol[16]{0};
    Side side{Side::BUY};
    Price target_entry{0};
    Price stop_loss{0};
    Price take_profit{0};
    double confidence{0.0};
    SignalSource source{SignalSource::INTERNAL_MOMENTUM};
    bool is_valid{false};
};

struct Order {
    uint64_t client_order_id{0};
    uint32_t token{0};
    char symbol[16]{0};
    Side side{Side::BUY};
    OrderType type{OrderType::MARKET};
    Price price{0};
    Price trigger_price{0};
    Quantity quantity{0};
    Quantity filled_quantity{0};
    Price avg_fill_price{0};
    OrderStatus status{OrderStatus::PENDING};
    uint64_t created_time_ns{0};
    uint64_t updated_time_ns{0};
    char rejection_reason[64]{0};
};

struct Position {
    uint32_t token{0};
    char symbol[16]{0};
    Side side{Side::BUY};
    Quantity quantity{0};
    Price avg_entry_price{0};
    Price current_price{0};
    Price current_stop_loss{0};
    Price initial_stop_loss{0};
    Price take_profit{0};
    bool is_breakeven_set{false};
    Price peak_favorable_price{0};
    Price worst_adverse_price{0};
    uint64_t open_time_ns{0};
};

struct TradeAuditEntry {
    uint64_t trade_id{0};
    char symbol[16]{0};
    Side side{Side::BUY};
    Price entry_price{0};
    Price exit_price{0};
    Quantity quantity{0};
    uint64_t entry_time_ns{0};
    uint64_t exit_time_ns{0};
    Price max_adverse_excursion{0}; // MAE (paise)
    Price max_favorable_excursion{0}; // MFE (paise)
    double gross_pnl_inr{0.0};
    double stt_inr{0.0};
    double exchange_fee_inr{0.0};
    double gst_inr{0.0};
    double stamp_duty_inr{0.0};
    double sebi_turnover_inr{0.0};
    double net_pnl_inr{0.0};
    double friction_ratio{0.0}; // total taxes / gross gain
};

} // namespace tradeuno::core
