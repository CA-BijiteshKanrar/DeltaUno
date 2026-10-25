#pragma once

#include "deltauno/strategy/strategy_interface.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace deltauno::strategy {

// Independent indicator state tracked per symbol to prevent cross-symbol contamination
struct SymbolIndicatorState {
    uint32_t token{0};
    char symbol[16]{0};
    uint64_t prev_total_volume{0};
    std::deque<uint64_t> recent_volumes;
    double cumulative_turnover{0.0};
    uint64_t cumulative_volume{0};

    void reset_session() noexcept {
        prev_total_volume = 0;
        recent_volumes.clear();
        cumulative_turnover = 0.0;
        cumulative_volume = 0;
    }

    [[nodiscard]] core::Price get_vwap() const noexcept {
        if (cumulative_volume == 0) return 0;
        return static_cast<core::Price>(cumulative_turnover / static_cast<double>(cumulative_volume));
    }

    [[nodiscard]] double latest_volume_multiple() const noexcept {
        if (recent_volumes.size() < 2) return 0.0;
        double avg_vol = 0.0;
        for (size_t i = 0; i < recent_volumes.size() - 1; ++i) {
            avg_vol += static_cast<double>(recent_volumes[i]);
        }
        avg_vol /= static_cast<double>(recent_volumes.size() - 1);
        if (avg_vol <= 0.0) return 0.0;
        return static_cast<double>(recent_volumes.back()) / avg_vol;
    }
};

class MomentumBreakoutStrategy : public IStrategy {
public:
    explicit MomentumBreakoutStrategy(double volume_surge_multiple = 2.5,
                                      double min_composite_ofi = 0.70,
                                      std::unordered_set<std::string> whitelist = {},
                                      core::Price tick_size_paise = 5,
                                      double circuit_buffer_pct = 1.5,
                                      double take_profit_pct = 1.05,
                                      double stop_loss_pct = 1.00,
                                      double min_bid_depth_ratio = 1.80)
        : volume_surge_multiple_(volume_surge_multiple),
          min_composite_ofi_(min_composite_ofi),
          whitelist_(std::move(whitelist)),
          tick_size_paise_(tick_size_paise),
          circuit_buffer_pct_(circuit_buffer_pct),
          take_profit_pct_(take_profit_pct),
          stop_loss_pct_(stop_loss_pct),
          min_bid_depth_ratio_(min_bid_depth_ratio) {
        if (whitelist_.empty()) {
            whitelist_ = {"TATASTEEL", "SBIN", "ITC", "COALINDIA", "ZOMATO"};
        }
        symbol_states_.reserve(16);
    }

    void on_tick(const core::MarketTick& tick, const market::OrderBook& /* book */) override {
        SymbolIndicatorState& state = symbol_states_[tick.token];
        if (state.token == 0) {
            state.token = tick.token;
            core::copy_fixed_str(state.symbol, tick.symbol);
            if (state.symbol[0] != '\0') {
                symbol_to_token_[std::string(state.symbol)] = tick.token;
            }
        }

        // Track cumulative volume and turnover for session VWAP calculation (per symbol)
        if (tick.last_quantity > 0) {
            state.cumulative_turnover += static_cast<double>(tick.last_price) * static_cast<double>(tick.last_quantity);
            state.cumulative_volume += tick.last_quantity;
        }

        // Rolling single-tick volume delta tracking across 20 periods (per symbol)
        if (state.prev_total_volume > 0 && tick.total_volume >= state.prev_total_volume) {
            const uint64_t delta_vol = tick.total_volume - state.prev_total_volume;
            state.recent_volumes.push_back(delta_vol);
            if (state.recent_volumes.size() > 20) {
                state.recent_volumes.pop_front();
            }
        }
        state.prev_total_volume = tick.total_volume;
        last_active_token_ = tick.token;
    }

    [[nodiscard]] std::optional<core::TradingSignal> generate_signal(const market::OrderBook& book) override {
        if (whitelist_.find(book.symbol()) == whitelist_.end()) {
            return std::nullopt;
        }

        const uint32_t token = book.token();
        auto it = symbol_states_.find(token);
        if (it == symbol_states_.end()) {
            return std::nullopt;
        }

        const auto& state = it->second;
        if (state.recent_volumes.size() < 10) {
            return std::nullopt; // Insufficient warmup
        }

        // 1. Check Volume Surge Acceleration against prior window average (per symbol)
        const double vol_mult = state.latest_volume_multiple();
        const bool has_volume_surge = (vol_mult >= volume_surge_multiple_);

        // 2. Check Depth-Normalized Multi-Horizon Composite OFI (Cont 2014 & Kolm 2023)
        const double ofi = book.composite_ofi();
        const bool has_ofi_support = (ofi >= min_composite_ofi_);

        // 3. Check Order Book Depth Imbalance (institutional queue absorption)
        const double depth_ratio = book.top5_bid_to_ask_depth_ratio();
        const bool has_depth_support = (depth_ratio >= min_bid_depth_ratio_);

        // 4. Check VWAP Alignment (per symbol)
        const core::Price vwap = state.get_vwap();
        const bool is_above_vwap = (vwap == 0) || (book.best_bid() >= vwap);

        // 5. Circuit proximity veto
        if (book.is_near_circuit(circuit_buffer_pct_)) {
            return std::nullopt;
        }

        if (has_volume_surge && has_ofi_support && has_depth_support && is_above_vwap && book.best_ask() > 0) {
            core::TradingSignal sig{};
            sig.timestamp_ns = book.last_tick_time_ns();
            sig.token = token;
            core::copy_fixed_str(sig.symbol, book.symbol());
            sig.side = core::Side::BUY;
            sig.target_entry = book.best_ask();

            // Set Stop Loss and Take Profit aligned to NSE tick size
            const auto raw_sl = static_cast<core::Price>(
                static_cast<double>(sig.target_entry) * (1.0 - stop_loss_pct_ / 100.0));
            const auto raw_tp = static_cast<core::Price>(
                static_cast<double>(sig.target_entry) * (1.0 + take_profit_pct_ / 100.0));
            sig.stop_loss = core::floor_to_tick(raw_sl, tick_size_paise_);
            sig.take_profit = core::ceil_to_tick(raw_tp, tick_size_paise_);

            sig.confidence = std::clamp(ofi, 0.0, 1.0);
            sig.source = core::SignalSource::INTERNAL_MOMENTUM;
            sig.is_valid = true;
            return sig;
        }

        return std::nullopt;
    }

    void on_trade_fill(const core::TradeAuditEntry& /* trade */) override {}

    [[nodiscard]] core::Price get_vwap(uint32_t token) const noexcept {
        auto it = symbol_states_.find(token);
        if (it == symbol_states_.end()) return 0;
        return it->second.get_vwap();
    }

    [[nodiscard]] core::Price get_vwap(std::string_view symbol) const noexcept {
        auto it = symbol_to_token_.find(std::string(symbol));
        if (it == symbol_to_token_.end()) return 0;
        return get_vwap(it->second);
    }

    // Backwards-compatible overload for single-symbol callers
    [[nodiscard]] core::Price get_vwap() const noexcept {
        if (last_active_token_ != 0) {
            return get_vwap(last_active_token_);
        }
        if (symbol_states_.size() == 1) {
            return symbol_states_.begin()->second.get_vwap();
        }
        return 0;
    }

    [[nodiscard]] double latest_volume_multiple(uint32_t token) const noexcept {
        auto it = symbol_states_.find(token);
        if (it == symbol_states_.end()) return 0.0;
        return it->second.latest_volume_multiple();
    }

    [[nodiscard]] double latest_volume_multiple(std::string_view symbol) const noexcept {
        auto it = symbol_to_token_.find(std::string(symbol));
        if (it == symbol_to_token_.end()) return 0.0;
        return latest_volume_multiple(it->second);
    }

    // Backwards-compatible overload for single-symbol callers
    [[nodiscard]] double latest_volume_multiple() const noexcept {
        if (last_active_token_ != 0) {
            return latest_volume_multiple(last_active_token_);
        }
        if (symbol_states_.size() == 1) {
            return symbol_states_.begin()->second.latest_volume_multiple();
        }
        return 0.0;
    }

    void reset_daily_session() noexcept {
        for (auto& [token, state] : symbol_states_) {
            state.reset_session();
        }
    }

    [[nodiscard]] const SymbolIndicatorState* get_symbol_state(uint32_t token) const noexcept {
        auto it = symbol_states_.find(token);
        if (it == symbol_states_.end()) return nullptr;
        return &it->second;
    }

    [[nodiscard]] const std::unordered_map<uint32_t, SymbolIndicatorState>& symbol_states() const noexcept {
        return symbol_states_;
    }

    [[nodiscard]] double take_profit_pct() const noexcept { return take_profit_pct_; }
    [[nodiscard]] double stop_loss_pct() const noexcept { return stop_loss_pct_; }
    [[nodiscard]] double min_composite_ofi() const noexcept { return min_composite_ofi_; }
    [[nodiscard]] double min_bid_depth_ratio() const noexcept { return min_bid_depth_ratio_; }

private:
    double volume_surge_multiple_{2.5};
    double min_composite_ofi_{0.70};
    std::unordered_set<std::string> whitelist_;
    core::Price tick_size_paise_{5};
    double circuit_buffer_pct_{1.5};
    double take_profit_pct_{1.05};
    double stop_loss_pct_{1.00};
    double min_bid_depth_ratio_{1.80};

    std::unordered_map<uint32_t, SymbolIndicatorState> symbol_states_;
    std::unordered_map<std::string, uint32_t> symbol_to_token_;
    uint32_t last_active_token_{0};
};

} // namespace deltauno::strategy
