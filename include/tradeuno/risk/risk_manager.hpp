#pragma once

#include "tradeuno/core/types.hpp"
#include <string>
#include <cmath>
#include <algorithm>
#include <optional>
#include <chrono>

namespace tradeuno::risk {

enum class RiskRejection : uint8_t {
    NONE = 0,
    EXCEEDS_DAILY_DRAWDOWN_LIMIT = 1,
    EXCEEDS_POSITION_VALUE_LIMIT = 2,
    MAX_OPEN_POSITIONS_REACHED = 3,
    OUTSIDE_TRADING_WINDOW = 4,
    AUTO_SQUAREOFF_TIME_REACHED = 5,
    INSUFFICIENT_PAYOFF_TO_FEE_RATIO = 6,
    ZERO_QUANTITY_AFTER_SIZING = 7,
    CIRCUIT_LIMIT_PROXIMITY = 8,
    MACRO_EVENT_FREEZE = 9
};

[[nodiscard]] constexpr const char* risk_rejection_to_string(RiskRejection r) noexcept {
    switch (r) {
        case RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT: return "EXCEEDS_DAILY_DRAWDOWN_LIMIT (Kill-switch triggered!)";
        case RiskRejection::EXCEEDS_POSITION_VALUE_LIMIT: return "EXCEEDS_POSITION_VALUE_LIMIT (Max ₹500)";
        case RiskRejection::MAX_OPEN_POSITIONS_REACHED: return "MAX_OPEN_POSITIONS_REACHED (Max 1)";
        case RiskRejection::OUTSIDE_TRADING_WINDOW: return "OUTSIDE_TRADING_WINDOW (Chop/Opening Range)";
        case RiskRejection::AUTO_SQUAREOFF_TIME_REACHED: return "AUTO_SQUAREOFF_TIME_REACHED (Past 15:05 IST)";
        case RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO: return "INSUFFICIENT_PAYOFF_TO_FEE_RATIO (Fees eat profit)";
        case RiskRejection::ZERO_QUANTITY_AFTER_SIZING: return "ZERO_QUANTITY_AFTER_SIZING";
        case RiskRejection::CIRCUIT_LIMIT_PROXIMITY: return "CIRCUIT_LIMIT_PROXIMITY";
        case RiskRejection::MACRO_EVENT_FREEZE: return "MACRO_EVENT_FREEZE (RBI/Budget Event)";
        default: return "NONE";
    }
}

class RiskManager {
public:
    explicit RiskManager(double initial_capital_inr = 1000.0,
                         double max_risk_per_trade_inr = 12.0,
                         double max_daily_drawdown_inr = 50.0,
                         double max_position_value_inr = 500.0)
        : current_capital_inr_(initial_capital_inr),
          peak_capital_inr_(initial_capital_inr),
          max_risk_per_trade_paise_(core::to_paise(max_risk_per_trade_inr)),
          max_daily_drawdown_paise_(core::to_paise(max_daily_drawdown_inr)),
          max_position_value_paise_(core::to_paise(max_position_value_inr)) {}

    // Pre-trade risk evaluation and dynamic position sizing
    [[nodiscard]] std::pair<core::Quantity, RiskRejection>
    evaluate_and_size_order(const core::TradingSignal& signal,
                            uint64_t current_time_ns,
                            bool is_macro_freeze = false) {
        // 1. Daily drawdown kill-switch check
        if (is_kill_switch_active_ || daily_loss_paise_ >= max_daily_drawdown_paise_) {
            is_kill_switch_active_ = true;
            return {0, RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT};
        }

        // 2. Open position limit check (Strictly 1 open position for ₹1,000 account)
        if (has_active_position_) {
            return {0, RiskRejection::MAX_OPEN_POSITIONS_REACHED};
        }

        // 3. Macro event freeze check
        if (is_macro_freeze) {
            return {0, RiskRejection::MACRO_EVENT_FREEZE};
        }

        // 4. Time-of-day filter (IST: 09:30-11:00 and 13:30-14:45)
        int seconds_from_midnight_ist = get_seconds_from_midnight_ist(current_time_ns);
        constexpr int t_0930 = 9 * 3600 + 30 * 60;
        constexpr int t_1100 = 11 * 3600;
        constexpr int t_1330 = 13 * 3600 + 30 * 60;
        constexpr int t_1445 = 14 * 3600 + 45 * 60;
        constexpr int t_1505 = 15 * 3600 + 5 * 60;

        if (seconds_from_midnight_ist >= t_1505) {
            return {0, RiskRejection::AUTO_SQUAREOFF_TIME_REACHED};
        }

        bool is_morning_window = (seconds_from_midnight_ist >= t_0930 && seconds_from_midnight_ist <= t_1100);
        bool is_afternoon_window = (seconds_from_midnight_ist >= t_1330 && seconds_from_midnight_ist <= t_1445);

        if (!is_morning_window && !is_afternoon_window) {
            return {0, RiskRejection::OUTSIDE_TRADING_WINDOW};
        }

        // 5. Dynamic Fixed-Risk Sizing Formula:
        // Quantity = min( floor(Max_Risk / (Entry - SL)), floor(Max_Position_Value / Entry) )
        if (signal.target_entry <= signal.stop_loss) {
            return {0, RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO};
        }

        core::Price risk_per_share_paise = signal.target_entry - signal.stop_loss;
        core::Quantity qty_by_risk = static_cast<core::Quantity>(max_risk_per_trade_paise_ / risk_per_share_paise);
        core::Quantity qty_by_capital = static_cast<core::Quantity>(max_position_value_paise_ / signal.target_entry);

        core::Quantity final_qty = std::min(qty_by_risk, qty_by_capital);
        if (final_qty == 0) {
            return {0, RiskRejection::ZERO_QUANTITY_AFTER_SIZING};
        }

        // 6. Statutory Friction vs. Expected Payoff Filter
        double trade_value_inr = core::to_inr(signal.target_entry * final_qty);
        double est_stt = trade_value_inr * 0.00025; // 0.025% on intraday sell
        double est_turnover = (trade_value_inr * 2) * 0.0000345; // 0.00345%
        double est_gst = est_turnover * 0.18;
        double est_stamp = trade_value_inr * 0.00003; // 0.003% on buy
        double total_est_fees_inr = est_stt + est_turnover + est_gst + est_stamp;

        double expected_gain_inr = core::to_inr((signal.take_profit - signal.target_entry) * final_qty);
        if (expected_gain_inr < total_est_fees_inr * 2.5) {
            return {0, RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO};
        }

        return {final_qty, RiskRejection::NONE};
    }

    // Monitors open position for Zero-Loss Trailing Stop & Auto-Squareoff
    [[nodiscard]] std::pair<bool, std::string>
    check_position_rules(core::Position& pos,
                         core::Price current_market_price,
                         uint64_t current_time_ns) {
        pos.current_price = current_market_price;

        // Track MAE & MFE
        if (pos.peak_favorable_price == 0 || current_market_price > pos.peak_favorable_price) {
            pos.peak_favorable_price = current_market_price;
        }
        if (pos.worst_adverse_price == 0 || current_market_price < pos.worst_adverse_price) {
            pos.worst_adverse_price = current_market_price;
        }

        // 1. Auto-Squareoff Shield at 15:05 IST
        int seconds_from_midnight_ist = get_seconds_from_midnight_ist(current_time_ns);
        constexpr int t_1505 = 15 * 3600 + 5 * 60;
        if (seconds_from_midnight_ist >= t_1505) {
            return {true, "AUTO_SQUAREOFF_SHIELD_1505"};
        }

        // 2. Zero-Loss Break-Even Trailing Stop
        // If profit >= 1.0%, move Stop Loss to Entry + 0.15% (covers all taxes/fees)
        if (!pos.is_breakeven_set && current_market_price >= static_cast<core::Price>(pos.avg_entry_price * 1.010)) {
            core::Price breakeven_price = static_cast<core::Price>(pos.avg_entry_price * 1.0015);
            pos.current_stop_loss = std::max(pos.current_stop_loss, breakeven_price);
            pos.is_breakeven_set = true;
        }

        // 3. Stop-Loss Trigger Check
        if (current_market_price <= pos.current_stop_loss) {
            return {true, pos.is_breakeven_set ? "BREAKEVEN_STOP_TRIGGERED" : "STOP_LOSS_TRIGGERED"};
        }

        // 4. Take-Profit Trigger Check
        if (pos.take_profit > 0 && current_market_price >= pos.take_profit) {
            return {true, "TAKE_PROFIT_TRIGGERED"};
        }

        return {false, "HOLD"};
    }

    void on_trade_closed(const core::TradeAuditEntry& trade) {
        has_active_position_ = false;
        current_capital_inr_ += trade.net_pnl_inr;

        if (current_capital_inr_ > peak_capital_inr_) {
            peak_capital_inr_ = current_capital_inr_;
        }

        if (trade.net_pnl_inr < 0) {
            daily_loss_paise_ += core::to_paise(-trade.net_pnl_inr);
            if (daily_loss_paise_ >= max_daily_drawdown_paise_) {
                is_kill_switch_active_ = true;
            }
        }
    }

    void set_has_active_position(bool active) noexcept {
        has_active_position_ = active;
    }

    [[nodiscard]] double current_capital_inr() const noexcept {
        return current_capital_inr_;
    }

    [[nodiscard]] bool is_kill_switch_active() const noexcept {
        return is_kill_switch_active_;
    }

    [[nodiscard]] core::Price daily_loss_paise() const noexcept {
        return daily_loss_paise_;
    }

private:
    [[nodiscard]] int get_seconds_from_midnight_ist(uint64_t time_ns) const noexcept {
        // IST is UTC + 5:30 = +19800 seconds
        uint64_t seconds_utc = time_ns / 1'000'000'000ULL;
        uint64_t seconds_ist = seconds_utc + 19800;
        return static_cast<int>(seconds_ist % 86400);
    }

    double current_capital_inr_{1000.0};
    double peak_capital_inr_{1000.0};
    core::Price max_risk_per_trade_paise_;
    core::Price max_daily_drawdown_paise_;
    core::Price max_position_value_paise_;

    core::Price daily_loss_paise_{0};
    bool is_kill_switch_active_{false};
    bool has_active_position_{false};
};

} // namespace tradeuno::risk
