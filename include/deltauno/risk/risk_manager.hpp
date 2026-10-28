#pragma once

#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/market/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace deltauno::risk {

enum class KillSwitchStatus : uint8_t {
    CLEAR = 0,
    TRIPPED = 1
};

[[nodiscard]] constexpr const char* kill_switch_status_to_string(KillSwitchStatus s) noexcept {
    return (s == KillSwitchStatus::TRIPPED) ? "TRIPPED" : "CLEAR";
}

struct DailyKillSwitchAudit {
    std::string date_ist;                  // Format: "YYYY-MM-DD"
    int64_t ist_day_index{0};              // Epoch day index
    double starting_capital_inr{10000.0};  // Equity at 09:15 IST opening bell
    double peak_capital_inr{10000.0};      // Highest equity recorded during session
    double trough_capital_inr{10000.0};    // Lowest equity recorded during session
    double max_drawdown_inr{0.0};          // Maximum peak-to-trough drop in INR
    double max_drawdown_pct{0.0};          // Maximum peak-to-trough drop as % of peak
    KillSwitchStatus status{KillSwitchStatus::CLEAR}; // CLEAR vs TRIPPED
    uint64_t tripped_time_ns{0};           // Timestamp of trip event (0 if CLEAR)
    size_t trade_count{0};                 // Completed trades in this session
};

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
        case RiskRejection::EXCEEDS_POSITION_VALUE_LIMIT: return "EXCEEDS_POSITION_VALUE_LIMIT (Max ₹5,000)";
        case RiskRejection::MAX_OPEN_POSITIONS_REACHED: return "MAX_OPEN_POSITIONS_REACHED (Max 1)";
        case RiskRejection::OUTSIDE_TRADING_WINDOW: return "OUTSIDE_TRADING_WINDOW (Chop/Opening Range)";
        case RiskRejection::AUTO_SQUAREOFF_TIME_REACHED: return "AUTO_SQUAREOFF_TIME_REACHED (Past 15:05 IST)";
        case RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO: return "INSUFFICIENT_PAYOFF_TO_FEE_RATIO (Fees eat profit)";
        case RiskRejection::ZERO_QUANTITY_AFTER_SIZING: return "ZERO_QUANTITY_AFTER_SIZING";
        case RiskRejection::CIRCUIT_LIMIT_PROXIMITY: return "CIRCUIT_LIMIT_PROXIMITY (Within 1.5% of Circuit)";
        case RiskRejection::MACRO_EVENT_FREEZE: return "MACRO_EVENT_FREEZE (RBI/Budget/FOMC Event)";
        default: return "NONE";
    }
}

class RiskManager {
public:
    explicit RiskManager(const core::RiskConfig& cfg)
        : cfg_(cfg),
          current_capital_inr_(cfg.initial_capital_inr),
          peak_capital_inr_(cfg.initial_capital_inr),
          day_starting_capital_inr_(cfg.initial_capital_inr),
          day_peak_capital_inr_(cfg.initial_capital_inr),
          day_trough_capital_inr_(cfg.initial_capital_inr),
          max_risk_per_trade_paise_(core::to_paise(cfg.max_risk_per_trade_inr)),
          max_daily_drawdown_paise_(core::to_paise(cfg.max_daily_drawdown_inr)),
          max_position_value_paise_(core::to_paise(cfg.max_position_value_inr)) {}

    explicit RiskManager(double initial_capital_inr = 10000.0,
                         double max_risk_per_trade_inr = 120.0,
                         double max_daily_drawdown_inr = 500.0,
                         double max_position_value_inr = 5000.0)
        : current_capital_inr_(initial_capital_inr),
          peak_capital_inr_(initial_capital_inr),
          day_starting_capital_inr_(initial_capital_inr),
          day_peak_capital_inr_(initial_capital_inr),
          day_trough_capital_inr_(initial_capital_inr),
          max_risk_per_trade_paise_(core::to_paise(max_risk_per_trade_inr)),
          max_daily_drawdown_paise_(core::to_paise(max_daily_drawdown_inr)),
          max_position_value_paise_(core::to_paise(max_position_value_inr)) {
        cfg_.initial_capital_inr = initial_capital_inr;
        cfg_.max_risk_per_trade_inr = max_risk_per_trade_inr;
        cfg_.max_daily_drawdown_inr = max_daily_drawdown_inr;
        cfg_.max_position_value_inr = max_position_value_inr;
    }

    // Pre-trade risk evaluation and dynamic position sizing across all pre-trade shields
    [[nodiscard]] std::pair<core::Quantity, RiskRejection>
    evaluate_and_size_order(const core::TradingSignal& signal,
                            uint64_t current_time_ns,
                            bool is_macro_freeze = false,
                            bool is_near_circuit = false) {
        check_day_rollover(current_time_ns);

        // 0. Permanent cumulative pilot stop-out latch (Charter §1 & §4: cumulative loss >= ₹1,500 / equity <= ₹8,500)
        if (is_pilot_stopout_active_ || current_capital_inr_ <= (cfg_.initial_capital_inr - 1500.0)) {
            is_pilot_stopout_active_ = true;
            return {0, RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT};
        }

        // 1. Daily drawdown kill-switch check (net daily / intraday peak-to-trough drawdown)
        if (is_kill_switch_active_ || daily_drawdown_paise() >= max_daily_drawdown_paise_) {
            is_kill_switch_active_ = true;
            return {0, RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT};
        }

        // 2. Open position limit check (Strictly 1 open/pending position for ₹10,000 pilot account)
        if (has_active_position_) {
            return {0, RiskRejection::MAX_OPEN_POSITIONS_REACHED};
        }

        // 3. Macro event freeze check (RBI / Budget / FOMC)
        if (is_macro_freeze) {
            return {0, RiskRejection::MACRO_EVENT_FREEZE};
        }

        // 4. Circuit band proximity shield
        if (is_near_circuit) {
            return {0, RiskRejection::CIRCUIT_LIMIT_PROXIMITY};
        }

        // 5. Time-of-day filter & 15:05 IST Auto-Squareoff Cutoff
        const int seconds_from_midnight_ist = get_seconds_from_midnight_ist(current_time_ns);
        if (seconds_from_midnight_ist >= cfg_.auto_squareoff_sec) {
            return {0, RiskRejection::AUTO_SQUAREOFF_TIME_REACHED};
        }

        const bool is_morning_window =
            (seconds_from_midnight_ist >= cfg_.morning_start_sec && seconds_from_midnight_ist <= cfg_.morning_end_sec);
        const bool is_afternoon_window =
            !cfg_.disable_afternoon_entries &&
            (seconds_from_midnight_ist >= cfg_.afternoon_start_sec && seconds_from_midnight_ist <= cfg_.afternoon_end_sec);

        if (!is_morning_window && !is_afternoon_window) {
            return {0, RiskRejection::OUTSIDE_TRADING_WINDOW};
        }

        // 6. Dynamic Fixed-Risk Sizing Formula (Bi-Directional: BUY Long & SELL Short):
        // Quantity = min( floor(Max_Risk / |Entry - SL|), floor(Max_Position_Value / Entry) )
        const bool is_short = (signal.side == core::Side::SELL);
        if (signal.target_entry <= 0) {
            return {0, RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO};
        }
        if (!is_short && (signal.target_entry <= signal.stop_loss || signal.take_profit <= signal.target_entry)) {
            return {0, RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO};
        }
        if (is_short && (signal.stop_loss <= signal.target_entry || signal.target_entry <= signal.take_profit)) {
            return {0, RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO};
        }

        const core::Price risk_per_share_paise = is_short
            ? (signal.stop_loss - signal.target_entry)
            : (signal.target_entry - signal.stop_loss);
        double comp_mult = 1.0;
        if (!cfg_.enable_ofi_harvest &&
            max_position_value_paise_ > core::to_paise(5000.0) &&
            cfg_.initial_capital_inr > 0.0 &&
            (current_capital_inr_ > cfg_.initial_capital_inr || enable_bidirectional_compounding_)) {
            comp_mult = current_capital_inr_ / cfg_.initial_capital_inr;
        }
        const auto dynamic_risk_paise = static_cast<core::Price>(static_cast<double>(max_risk_per_trade_paise_) * comp_mult);
        const auto dynamic_pos_paise = static_cast<core::Price>(static_cast<double>(max_position_value_paise_) * comp_mult);
        const auto qty_by_risk = static_cast<core::Quantity>(dynamic_risk_paise / risk_per_share_paise);
        const auto qty_by_capital = static_cast<core::Quantity>(dynamic_pos_paise / signal.target_entry);

        const core::Quantity final_qty = std::min(qty_by_risk, qty_by_capital);
        if (final_qty == 0) {
            return {0, RiskRejection::ZERO_QUANTITY_AFTER_SIZING};
        }

        // 7. Statutory Friction vs. Expected Payoff Filter (exact parity with PaperBroker taxes)
        const double trade_value_inr = core::to_inr(signal.target_entry * static_cast<core::Price>(final_qty));
        const double turnover_inr = trade_value_inr * 2.0;
        const double est_stt = trade_value_inr * 0.00025;            // 0.025% on intraday sell
        const double est_turnover = turnover_inr * 0.0000297;        // 0.00297% NSE turnover (revised)
        const double est_sebi = turnover_inr * 0.000001;             // ₹10 per crore
        const double est_gst = (est_turnover + est_sebi) * 0.18;     // 18% GST on (exchange + SEBI)
        const double est_stamp = trade_value_inr * 0.00003;          // 0.003% on buy
        const double total_est_fees_inr = est_stt + est_turnover + est_gst + est_stamp + est_sebi;

        const core::Price reward_per_share_paise = is_short
            ? (signal.target_entry - signal.take_profit)
            : (signal.take_profit - signal.target_entry);
        const double expected_gain_inr =
            core::to_inr(reward_per_share_paise * static_cast<core::Price>(final_qty));
        if (expected_gain_inr < total_est_fees_inr * cfg_.min_payoff_to_fee_ratio) {
            return {0, RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO};
        }

        return {final_qty, RiskRejection::NONE};
    }

    // Monitors open position for MAE/MFE, Zero-Loss Break-Even Trailing Stop,
    // Progressive Profit Ratcheting, Active L2 Order-Flow Exhaustion Profit Harvest & 15:05 Auto-Squareoff
    [[nodiscard]] std::pair<bool, std::string>
    check_position_rules(core::Position& pos,
                         core::Price current_market_price,
                         uint64_t current_time_ns,
                         core::Price tick_size_paise = 5,
                         const market::OrderBook* book = nullptr) {
        pos.current_price = current_market_price;

        if (pos.peak_favorable_price == 0) {
            pos.peak_favorable_price = pos.avg_entry_price;
        }
        if (pos.worst_adverse_price == 0) {
            pos.worst_adverse_price = pos.avg_entry_price;
        }

        const bool is_short = (pos.side == core::Side::SELL) && (pos.take_profit == 0 || pos.take_profit < pos.avg_entry_price);
        if (!is_short) {
            if (current_market_price > pos.peak_favorable_price) {
                pos.peak_favorable_price = current_market_price;
            }
            if (current_market_price < pos.worst_adverse_price) {
                pos.worst_adverse_price = current_market_price;
            }
        } else {
            if (current_market_price < pos.peak_favorable_price) {
                pos.peak_favorable_price = current_market_price;
            }
            if (current_market_price > pos.worst_adverse_price) {
                pos.worst_adverse_price = current_market_price;
            }
        }

        // 1. Auto-Squareoff Shield at 15:05 IST
        const int seconds_from_midnight_ist = get_seconds_from_midnight_ist(current_time_ns);
        if (seconds_from_midnight_ist >= cfg_.auto_squareoff_sec) {
            return {true, "AUTO_SQUAREOFF_SHIELD_1505"};
        }

        if (is_short) {
            // Symmetric Short Position Management (Downward Break-Even Ratchet & SL/TP)
            const double trigger_mult = 1.0 - (cfg_.breakeven_trigger_pct / 100.0);
            const auto trigger_price = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * trigger_mult);
            if (!pos.is_breakeven_set && current_market_price <= trigger_price) {
                const auto raw_be = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 0.9985);
                const core::Price breakeven_price = std::min(
                    core::floor_to_tick(raw_be, tick_size_paise),
                    pos.avg_entry_price - std::max<core::Price>(1, tick_size_paise));
                pos.current_stop_loss = std::min(pos.current_stop_loss, breakeven_price);
                pos.is_breakeven_set = true;
            }

            if (book != nullptr && book->best_ask() > 0 && cfg_.enable_ofi_harvest) {
                const core::Price max_net_win_ask = std::min(
                    core::floor_to_tick(static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 0.9985), tick_size_paise),
                    pos.avg_entry_price - 2 * std::max<core::Price>(1, tick_size_paise));
                const auto stage1_trigger = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 0.9960);
                const auto stage2_trigger = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 0.9930);
                if (current_market_price <= stage2_trigger) {
                    const core::Price stage2_lock = std::min(
                        max_net_win_ask,
                        core::floor_to_tick(static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 0.9965), tick_size_paise));
                    pos.current_stop_loss = std::min(pos.current_stop_loss, stage2_lock);
                } else if (current_market_price <= stage1_trigger) {
                    pos.current_stop_loss = std::min(pos.current_stop_loss, max_net_win_ask);
                }
                const bool is_bouncing_or_stalled =
                    (current_market_price > pos.peak_favorable_price) ||
                    (pos.open_time_ns > 0 && current_time_ns > pos.open_time_ns &&
                     (current_time_ns - pos.open_time_ns) >= 900'000'000'000ULL);
                if (is_bouncing_or_stalled && book->best_ask() <= max_net_win_ask) {
                    if (book->composite_ofi() > -0.35 || book->weighted_mid_price() > book->mid_price()) {
                        return {true, "OFI_EXHAUSTION_PROFIT_HARVEST"};
                    }
                }
            }

            if (current_market_price >= pos.current_stop_loss) {
                return {true, (pos.is_breakeven_set || pos.current_stop_loss < pos.avg_entry_price)
                                  ? "BREAKEVEN_STOP_TRIGGERED"
                                  : "STOP_LOSS_TRIGGERED"};
            }
            if (pos.take_profit > 0 && current_market_price <= pos.take_profit) {
                return {true, "TAKE_PROFIT_TRIGGERED"};
            }
            return {false, "HOLD"};
        }

        // 2. Zero-Loss Break-Even Trailing Stop
        // Once unrealized gain >= breakeven_trigger_pct (1.0%), ratchet SL to Entry + 0.15% (rounded up to valid tick)
        const double trigger_mult = 1.0 + (cfg_.breakeven_trigger_pct / 100.0);
        const auto trigger_price = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * trigger_mult);
        if (!pos.is_breakeven_set && current_market_price >= trigger_price) {
            const auto raw_be = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 1.0015);
            const core::Price breakeven_price = std::max(
                core::ceil_to_tick(raw_be, tick_size_paise),
                pos.avg_entry_price + std::max<core::Price>(1, tick_size_paise));
            pos.current_stop_loss = std::max(pos.current_stop_loss, breakeven_price);
            pos.is_breakeven_set = true;
        }

        // 2b. Progressive Multi-Stage Profit Ratchet & Active L2 Order-Flow Exhaustion Profit Harvest
        // (Active in Harvester mode when live L2 OrderBook depth is available)
        if (book != nullptr && book->best_bid() > 0 &&
            (cfg_.enable_ofi_harvest || max_position_value_paise_ <= core::to_paise(5000.0))) {
            const double min_win_mult = cfg_.enable_ofi_harvest ? 1.0055 : 1.0015;
            const core::Price min_net_win_bid = std::max(
                core::ceil_to_tick(static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * min_win_mult), tick_size_paise),
                pos.avg_entry_price + 2 * std::max<core::Price>(1, tick_size_paise));

            // Progressive profit floor ratcheting at +0.40% and +0.70% MFE
            const auto stage1_trigger = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 1.0040);
            const auto stage2_trigger = static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 1.0070);
            if (current_market_price >= stage2_trigger) {
                const core::Price stage2_lock = std::max(
                    min_net_win_bid,
                    core::ceil_to_tick(static_cast<core::Price>(static_cast<double>(pos.avg_entry_price) * 1.0035), tick_size_paise));
                pos.current_stop_loss = std::max(pos.current_stop_loss, stage2_lock);
            } else if (current_market_price >= stage1_trigger) {
                pos.current_stop_loss = std::max(pos.current_stop_loss, min_net_win_bid);
            }

            // Active L2 OFI Exhaustion / Pullback Profit Harvest:
            // Never interrupt a trade while it is actively making new highs (current_market_price >= pos.peak_favorable_price).
            // Once price pulls back from its peak MFE (or stalls > 15 minutes) while executable best_bid is still in
            // net profit (covering NSE statutory taxes + Tick[t+1] slippage) and order flow weakens, harvest profit
            // before the short-horizon impulse mean-reverts into a loss.
            const bool is_pulling_back_or_stalled =
                (current_market_price < pos.peak_favorable_price) ||
                (pos.open_time_ns > 0 && current_time_ns > pos.open_time_ns &&
                 (current_time_ns - pos.open_time_ns) >= 900'000'000'000ULL);
            if (is_pulling_back_or_stalled && book->best_bid() >= min_net_win_bid) {
                const bool ofi_exhausted = (book->composite_ofi() < 0.35);
                const bool microprice_reversed = (book->weighted_mid_price() < book->mid_price());
                if (ofi_exhausted || microprice_reversed) {
                    return {true, "OFI_EXHAUSTION_PROFIT_HARVEST"};
                }
            }
        }

        // 3. Stop-Loss Trigger Check
        if (current_market_price <= pos.current_stop_loss) {
            return {true, (pos.is_breakeven_set || pos.current_stop_loss > pos.avg_entry_price)
                              ? "BREAKEVEN_STOP_TRIGGERED"
                              : "STOP_LOSS_TRIGGERED"};
        }

        // 4. Take-Profit Trigger Check
        if (pos.take_profit > 0 && current_market_price >= pos.take_profit) {
            return {true, "TAKE_PROFIT_TRIGGERED"};
        }

        return {false, "HOLD"};
    }

    void on_trade_closed(const core::TradeAuditEntry& trade) {
        if (trade.exit_time_ns > 0) {
            check_day_rollover(trade.exit_time_ns);
        }

        has_active_position_ = false;
        ++day_trade_count_;
        current_capital_inr_ += trade.net_pnl_inr;

        if (current_capital_inr_ > peak_capital_inr_) {
            peak_capital_inr_ = current_capital_inr_;
        }

        day_peak_capital_inr_ = std::max(day_peak_capital_inr_, current_capital_inr_);
        day_trough_capital_inr_ = std::min(day_trough_capital_inr_, current_capital_inr_);

        if (trade.net_pnl_inr < -0.05) {
            ++daily_consecutive_losses_;
        } else if (trade.net_pnl_inr > 0.05) {
            daily_consecutive_losses_ = 0;
        }

        daily_net_pnl_paise_ += core::to_paise(trade.net_pnl_inr);
        intraday_peak_pnl_paise_ = std::max(intraday_peak_pnl_paise_, daily_net_pnl_paise_);

        const core::Price current_dd_paise = daily_drawdown_paise();
        max_intraday_drawdown_paise_ = std::max(max_intraday_drawdown_paise_, current_dd_paise);

        if (current_dd_paise >= max_daily_drawdown_paise_) {
            if (!is_kill_switch_active_) {
                is_kill_switch_active_ = true;
                kill_switch_tripped_time_ns_ = trade.exit_time_ns;
            }
        }

        if (current_capital_inr_ <= (cfg_.initial_capital_inr - 1500.0)) {
            is_pilot_stopout_active_ = true;
        }
    }

    void archive_current_day() noexcept {
        if (active_ist_day_ == -1) return;

        DailyKillSwitchAudit audit{};
        audit.ist_day_index = active_ist_day_;
        audit.date_ist = format_ist_date(active_ist_day_);
        audit.starting_capital_inr = day_starting_capital_inr_;
        audit.peak_capital_inr = day_peak_capital_inr_;
        audit.trough_capital_inr = day_trough_capital_inr_;
        audit.max_drawdown_inr = core::to_inr(max_intraday_drawdown_paise_);
        audit.max_drawdown_pct = (day_peak_capital_inr_ > 0.0)
            ? (audit.max_drawdown_inr / day_peak_capital_inr_ * 100.0) : 0.0;
        audit.status = is_kill_switch_active_ ? KillSwitchStatus::TRIPPED : KillSwitchStatus::CLEAR;
        audit.tripped_time_ns = kill_switch_tripped_time_ns_;
        audit.trade_count = day_trade_count_;

        kill_switch_history_.push_back(std::move(audit));
    }

    void reset_daily_limits() noexcept {
        daily_net_pnl_paise_ = 0;
        intraday_peak_pnl_paise_ = 0;
        max_intraday_drawdown_paise_ = 0;
        daily_consecutive_losses_ = 0;
        day_trade_count_ = 0;
        is_kill_switch_active_ = false;
        kill_switch_tripped_time_ns_ = 0;

        day_starting_capital_inr_ = current_capital_inr_;
        day_peak_capital_inr_ = current_capital_inr_;
        day_trough_capital_inr_ = current_capital_inr_;
    }

    void reset_daily_state() noexcept {
        reset_daily_limits();
    }

    bool check_day_rollover(uint64_t time_ns) noexcept {
        if (time_ns == 0) return false;
        const int64_t ist_day = static_cast<int64_t>((time_ns / 1'000'000'000ULL + 19800ULL) / 86400ULL);
        if (active_ist_day_ != -1 && ist_day != active_ist_day_) {
            archive_current_day();
            reset_daily_limits();
            active_ist_day_ = ist_day;
            return true;
        }
        if (active_ist_day_ == -1) {
            day_starting_capital_inr_ = current_capital_inr_;
            day_peak_capital_inr_ = current_capital_inr_;
            day_trough_capital_inr_ = current_capital_inr_;
            active_ist_day_ = ist_day;
        }
        return false;
    }

    void finalize_session() noexcept {
        if (active_ist_day_ != -1) {
            if (kill_switch_history_.empty() || kill_switch_history_.back().ist_day_index != active_ist_day_) {
                archive_current_day();
            }
        }
    }

    void set_has_active_position(bool active) noexcept {
        has_active_position_ = active;
    }

    [[nodiscard]] bool has_active_position() const noexcept {
        return has_active_position_;
    }

    [[nodiscard]] double current_capital_inr() const noexcept {
        return current_capital_inr_;
    }

    [[nodiscard]] double day_starting_capital_inr() const noexcept {
        return day_starting_capital_inr_;
    }

    [[nodiscard]] double day_peak_capital_inr() const noexcept {
        return day_peak_capital_inr_;
    }

    [[nodiscard]] double day_trough_capital_inr() const noexcept {
        return day_trough_capital_inr_;
    }

    [[nodiscard]] core::Price max_intraday_drawdown_paise() const noexcept {
        return max_intraday_drawdown_paise_;
    }

    [[nodiscard]] size_t day_trade_count() const noexcept {
        return day_trade_count_;
    }

    [[nodiscard]] const std::vector<DailyKillSwitchAudit>& kill_switch_history() const noexcept {
        return kill_switch_history_;
    }

    [[nodiscard]] uint32_t consecutive_losses() const noexcept {
        return daily_consecutive_losses_;
    }

    [[nodiscard]] bool is_kill_switch_active() const noexcept {
        return is_kill_switch_active_;
    }

    [[nodiscard]] bool is_pilot_stopout_active() const noexcept {
        return is_pilot_stopout_active_;
    }

    void set_starting_capital_override(double override_capital_inr) noexcept {
        if (override_capital_inr > 0.0) {
            current_capital_inr_ = override_capital_inr;
            peak_capital_inr_ = std::max(peak_capital_inr_, override_capital_inr);
            day_starting_capital_inr_ = override_capital_inr;
            day_peak_capital_inr_ = override_capital_inr;
            day_trough_capital_inr_ = override_capital_inr;
            enable_bidirectional_compounding_ = true;
        }
    }

    void sync_shared_capital(double shared_capital_inr) noexcept {
        current_capital_inr_ = shared_capital_inr;
        if (current_capital_inr_ > peak_capital_inr_) peak_capital_inr_ = current_capital_inr_;
        if (current_capital_inr_ > day_peak_capital_inr_) day_peak_capital_inr_ = current_capital_inr_;
        if (current_capital_inr_ < day_trough_capital_inr_) day_trough_capital_inr_ = current_capital_inr_;
        enable_bidirectional_compounding_ = true;
    }

    [[nodiscard]] core::Price daily_drawdown_paise() const noexcept {
        return std::max<core::Price>(0, intraday_peak_pnl_paise_ - daily_net_pnl_paise_);
    }

    [[nodiscard]] core::Price daily_loss_paise() const noexcept {
        return daily_drawdown_paise();
    }

    [[nodiscard]] static std::string format_ist_date(int64_t ist_day_index) {
        if (ist_day_index < 0) return "1970-01-01";
        const time_t sec = static_cast<time_t>(ist_day_index * 86400ULL);
        std::tm tm_val{};
#if defined(_WIN32) || defined(_WIN64)
        gmtime_s(&tm_val, &sec);
#else
        gmtime_r(&sec, &tm_val);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_val);
        return std::string(buf);
    }

private:
    [[nodiscard]] static int get_seconds_from_midnight_ist(uint64_t time_ns) noexcept {
        // IST is UTC + 05:30 (+19,800 seconds)
        const uint64_t seconds_utc = time_ns / 1'000'000'000ULL;
        const uint64_t seconds_ist = seconds_utc + 19800ULL;
        return static_cast<int>(seconds_ist % 86400ULL);
    }

    core::RiskConfig cfg_{};
    double current_capital_inr_{10000.0};
    double peak_capital_inr_{10000.0};
    double day_starting_capital_inr_{10000.0};
    double day_peak_capital_inr_{10000.0};
    double day_trough_capital_inr_{10000.0};
    core::Price max_risk_per_trade_paise_{12000};
    core::Price max_daily_drawdown_paise_{50000};
    core::Price max_position_value_paise_{500000};

    core::Price daily_net_pnl_paise_{0};
    core::Price intraday_peak_pnl_paise_{0};
    core::Price max_intraday_drawdown_paise_{0};
    size_t day_trade_count_{0};
    uint64_t kill_switch_tripped_time_ns_{0};
    int64_t active_ist_day_{-1};
    uint32_t daily_consecutive_losses_{0};
    bool is_kill_switch_active_{false};
    bool is_pilot_stopout_active_{false};
    bool has_active_position_{false};
    bool enable_bidirectional_compounding_{false};
    std::vector<DailyKillSwitchAudit> kill_switch_history_;
};

} // namespace deltauno::risk
