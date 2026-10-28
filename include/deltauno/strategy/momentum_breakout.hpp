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
    core::Price prev_last_price{0};
    uint32_t consecutive_bullish_ticks{0};
    uint32_t consecutive_bearish_ticks{0};
    uint64_t last_exit_time_ns{0};
    uint32_t consecutive_losses{0};

    void reset_session() noexcept {
        prev_total_volume = 0;
        recent_volumes.clear();
        cumulative_turnover = 0.0;
        cumulative_volume = 0;
        prev_last_price = 0;
        consecutive_bullish_ticks = 0;
        consecutive_bearish_ticks = 0;
        last_exit_time_ns = 0;
        consecutive_losses = 0;
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

enum class StrategyMode : uint8_t {
    COMPOUNDER = 0, // Track A: Long-Only High-Yield Equity Compounder (Control)
    HARVESTER  = 1, // Track B: Long-Only L2 Microstructure Order-Flow Harvester (Control)
    OMNI       = 2  // Track C: Bi-Directional (Long + Short) + Continuous 09:35-14:45 Window + Midday Tier
};

class MomentumBreakoutStrategy : public IStrategy {
public:
    explicit MomentumBreakoutStrategy(double volume_surge_multiple = 2.5,
                                      double min_composite_ofi = 0.55,
                                      std::unordered_set<std::string> whitelist = {},
                                      core::Price tick_size_paise = 5,
                                      double circuit_buffer_pct = 1.5,
                                      double take_profit_pct = 1.75,
                                      double stop_loss_pct = 0.85,
                                      double min_bid_depth_ratio = 1.25,
                                      StrategyMode mode = StrategyMode::COMPOUNDER)
        : volume_surge_multiple_(volume_surge_multiple),
          min_composite_ofi_(min_composite_ofi),
          whitelist_(std::move(whitelist)),
          tick_size_paise_(tick_size_paise),
          circuit_buffer_pct_(circuit_buffer_pct),
          take_profit_pct_(take_profit_pct),
          stop_loss_pct_(stop_loss_pct),
          min_bid_depth_ratio_(min_bid_depth_ratio),
          mode_(mode) {
        if (whitelist_.empty()) {
            whitelist_ = {"TATASTEEL", "SBIN", "ITC", "COALINDIA", "ZOMATO"};
        }
        symbol_states_.reserve(16);
    }

    void on_tick(const core::MarketTick& tick, const market::OrderBook& book) override {
        // Intercept Macro Benchmark Tick (NIFTY 50: token 999999 or symbol "NIFTY")
        if (tick.token == 999999 || std::string_view(tick.symbol) == "NIFTY") {
            if (nifty_open_paise_ == 0 && tick.last_price > 0) {
                nifty_open_paise_ = tick.last_price;
            }
            nifty_last_paise_ = tick.last_price;
            if (nifty_open_paise_ > 0) {
                nifty_return_pct_ = (static_cast<double>(nifty_last_paise_ - nifty_open_paise_) /
                                     static_cast<double>(nifty_open_paise_)) * 100.0;
                // If NIFTY is down > 0.35% from open, mark regime as macro bearish
                is_macro_bearish_ = (nifty_return_pct_ <= -0.35);
            }
            return;
        }

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

        // Track price-flow agreement (multi-tick non-divergence vs. passive absorption)
        const double ofi = book.composite_ofi();
        if (state.prev_last_price > 0) {
            if (tick.last_price >= state.prev_last_price && ofi > 0.15) {
                ++state.consecutive_bullish_ticks;
                state.consecutive_bearish_ticks = 0;
            } else if (tick.last_price <= state.prev_last_price && ofi < -0.15) {
                ++state.consecutive_bearish_ticks;
                state.consecutive_bullish_ticks = 0;
            } else if (tick.last_price < state.prev_last_price && ofi < 0.0) {
                state.consecutive_bullish_ticks = 0;
            } else if (tick.last_price > state.prev_last_price && ofi > 0.0) {
                state.consecutive_bearish_ticks = 0;
            }
        } else if (ofi > 0.15) {
            state.consecutive_bullish_ticks = 1;
        } else if (ofi < -0.15) {
            state.consecutive_bearish_ticks = 1;
        }
        state.prev_last_price = tick.last_price;
        last_active_token_ = tick.token;
    }

    [[nodiscard]] std::optional<core::TradingSignal> generate_signal(const market::OrderBook& book) override {
        // 0. Macro Market Regime Gate: For Long-Only modes (Track A & B), veto if NIFTY is in heavy sell-off
        if (mode_ != StrategyMode::OMNI && is_macro_bearish_) {
            return std::nullopt;
        }

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

        // 3. Circuit proximity veto
        if (book.is_near_circuit(circuit_buffer_pct_)) {
            return std::nullopt;
        }

        const core::Price vwap = state.get_vwap();

        if (mode_ == StrategyMode::HARVESTER) {
            // Harvester Mode: Session overtrading & per-symbol post-trade cooldown
            if (session_wins_ >= 2 && (ofi < 0.85 || vol_mult < 3.5)) {
                return std::nullopt;
            }
            if (state.last_exit_time_ns > 0 && book.last_tick_time_ns() > state.last_exit_time_ns) {
                const uint64_t elapsed_ns = book.last_tick_time_ns() - state.last_exit_time_ns;
                if (state.consecutive_losses >= 1 || elapsed_ns < 3600'000'000'000ULL) {
                    return std::nullopt;
                }
            }

            const bool has_microprice_support = (book.weighted_mid_price() >= book.mid_price());
            const bool has_price_flow_agreement = (state.consecutive_bullish_ticks >= 1);
            const core::Price max_allowed_spread = std::max<core::Price>(
                tick_size_paise_ * 4,
                static_cast<core::Price>(static_cast<double>(book.best_ask()) * 0.0020));
            const bool has_tight_spread = (book.spread() >= 0 && book.spread() <= max_allowed_spread);
            // Statistically Valid VWAP Shield: Require established volume before honoring VWAP
            const bool has_established_vwap = (state.cumulative_volume >= 10000 && vwap > 0);
            if (has_established_vwap && book.best_bid() < vwap) {
                return std::nullopt; // Strictly veto buying below established institutional VWAP
            }
            const bool is_above_vwap =
                !has_established_vwap ||
                (book.best_bid() >= vwap && static_cast<double>(book.best_ask()) <= static_cast<double>(vwap) * 1.025);

            if (has_volume_surge && has_ofi_support && has_microprice_support &&
                has_price_flow_agreement && has_tight_spread && is_above_vwap && book.best_ask() > 0) {
                core::TradingSignal sig{};
                sig.timestamp_ns = book.last_tick_time_ns();
                sig.token = token;
                core::copy_fixed_str(sig.symbol, book.symbol());
                sig.side = core::Side::BUY;
                sig.target_entry = book.best_ask();

                const auto raw_sl = static_cast<core::Price>(static_cast<double>(sig.target_entry) * 0.990);
                const auto raw_tp = static_cast<core::Price>(static_cast<double>(sig.target_entry) * 1.018);
                sig.stop_loss = core::floor_to_tick(raw_sl, tick_size_paise_);
                sig.take_profit = core::ceil_to_tick(raw_tp, tick_size_paise_);

                sig.confidence = std::clamp(ofi, 0.0, 1.0);
                sig.source = core::SignalSource::INTERNAL_MOMENTUM;
                sig.is_valid = true;
                return sig;
            }
            return std::nullopt;
        }

        if (mode_ == StrategyMode::OMNI) {
            // Track C (Bi-Directional Omni): Continuous 09:35-14:45 Window + High-Conviction Midday Tier + Long/Short
            if (state.last_exit_time_ns > 0 && book.last_tick_time_ns() > state.last_exit_time_ns) {
                const uint64_t elapsed_ns = book.last_tick_time_ns() - state.last_exit_time_ns;
                if (state.consecutive_losses >= 1 || elapsed_ns < 2700'000'000'000ULL) {
                    return std::nullopt; // 45-minute post-exit cooldown per symbol & stop re-entering a loser
                }
            }

            const uint64_t sec_ist = ((book.last_tick_time_ns() / 1'000'000'000ULL) + 19800ULL) % 86400ULL;
            const bool is_midday_window = (sec_ist >= (11 * 3600 + 15 * 60) && sec_ist <= (13 * 3600 + 15 * 60));
            const double req_vol_mult = is_midday_window ? std::max(3.2, volume_surge_multiple_ * 1.25) : volume_surge_multiple_;
            const double req_ofi = is_midday_window ? std::max(0.70, min_composite_ofi_ + 0.15) : min_composite_ofi_;
            const double req_depth = is_midday_window ? std::max(1.40, min_bid_depth_ratio_ + 0.15) : min_bid_depth_ratio_;

            if (vol_mult < req_vol_mult) {
                return std::nullopt;
            }

            const bool has_established_vwap = (state.cumulative_volume >= 10000 && vwap > 0);

            // Path 1: LONG Breakout (Allowed when NIFTY is not in bearish sell-off)
            if (!is_macro_bearish_ && ofi >= req_ofi && book.best_ask() > 0) {
                const bool above_vwap = !has_established_vwap || (book.best_bid() >= vwap);
                const bool bid_depth_ok = (book.top5_bid_to_ask_depth_ratio() >= req_depth);
                const bool micro_ok = (book.weighted_mid_price() >= book.mid_price());
                if (above_vwap && bid_depth_ok && micro_ok) {
                    core::TradingSignal sig{};
                    sig.timestamp_ns = book.last_tick_time_ns();
                    sig.token = token;
                    core::copy_fixed_str(sig.symbol, book.symbol());
                    sig.side = core::Side::BUY;
                    sig.target_entry = book.best_ask();
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
            }

            // Path 2: SHORT Breakdown (Allowed when NIFTY is bearish OR stock shows severe L2 distribution below VWAP)
            const double short_req_ofi = is_macro_bearish_ ? req_ofi : std::max(0.65, req_ofi);
            if (has_established_vwap && ofi <= -short_req_ofi && book.best_bid() > 0 && book.best_ask() <= vwap) {
                const bool ask_depth_ok = (book.top5_ask_to_bid_depth_ratio() >= req_depth);
                const bool micro_bearish = (book.weighted_mid_price() <= book.mid_price());
                if (ask_depth_ok && micro_bearish && state.consecutive_bearish_ticks >= 1) {
                    core::TradingSignal sig{};
                    sig.timestamp_ns = book.last_tick_time_ns();
                    sig.token = token;
                    core::copy_fixed_str(sig.symbol, book.symbol());
                    sig.side = core::Side::SELL;
                    sig.target_entry = book.best_bid();
                    const auto raw_sl = static_cast<core::Price>(
                        static_cast<double>(sig.target_entry) * (1.0 + stop_loss_pct_ / 100.0));
                    const auto raw_tp = static_cast<core::Price>(
                        static_cast<double>(sig.target_entry) * (1.0 - take_profit_pct_ / 100.0));
                    sig.stop_loss = core::ceil_to_tick(raw_sl, tick_size_paise_);
                    sig.take_profit = core::floor_to_tick(raw_tp, tick_size_paise_);
                    sig.confidence = std::clamp(-ofi, 0.0, 1.0);
                    sig.source = core::SignalSource::INTERNAL_MOMENTUM;
                    sig.is_valid = true;
                    return sig;
                }
            }
            return std::nullopt;
        }

        // Compounder Mode (Default): Top-5 Order Book Depth Imbalance + Configurable TP/SL
        const double depth_ratio = book.top5_bid_to_ask_depth_ratio();
        const bool has_depth_support = (depth_ratio >= min_bid_depth_ratio_);

        // Statistically Valid VWAP Shield: Require established volume before honoring VWAP
        const bool has_established_vwap = (state.cumulative_volume >= 10000 && vwap > 0);
        if (has_established_vwap && book.best_bid() < vwap) {
            return std::nullopt; // Strictly veto buying below established institutional VWAP
        }
        const bool is_above_vwap = !has_established_vwap || (book.best_bid() >= vwap);

        if (has_volume_surge && has_ofi_support && has_depth_support && is_above_vwap && book.best_ask() > 0) {
            core::TradingSignal sig{};
            sig.timestamp_ns = book.last_tick_time_ns();
            sig.token = token;
            core::copy_fixed_str(sig.symbol, book.symbol());
            sig.side = core::Side::BUY;
            sig.target_entry = book.best_ask();

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

    void on_trade_fill(const core::TradeAuditEntry& trade) override {
        if (trade.net_pnl_inr > 0.05) {
            ++session_wins_;
        }
        auto it = symbol_to_token_.find(std::string(trade.symbol));
        if (it == symbol_to_token_.end()) return;
        auto st_it = symbol_states_.find(it->second);
        if (st_it == symbol_states_.end()) return;

        SymbolIndicatorState& state = st_it->second;
        state.last_exit_time_ns = trade.exit_time_ns;
        if (trade.net_pnl_inr < -0.05) {
            ++state.consecutive_losses;
        } else if (trade.net_pnl_inr > 0.05) {
            state.consecutive_losses = 0;
        }
    }

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
        session_wins_ = 0;
        for (auto& [token, state] : symbol_states_) {
            state.reset_session();
        }
        nifty_open_paise_ = 0;
        nifty_last_paise_ = 0;
        nifty_return_pct_ = 0.0;
        is_macro_bearish_ = false;
    }

    [[nodiscard]] double nifty_return_pct() const noexcept { return nifty_return_pct_; }
    [[nodiscard]] bool is_macro_bearish() const noexcept { return is_macro_bearish_; }
    void set_macro_benchmark(core::Price open_paise, core::Price last_paise) noexcept {
        nifty_open_paise_ = open_paise;
        nifty_last_paise_ = last_paise;
        if (nifty_open_paise_ > 0) {
            nifty_return_pct_ = (static_cast<double>(nifty_last_paise_ - nifty_open_paise_) /
                                 static_cast<double>(nifty_open_paise_)) * 100.0;
            is_macro_bearish_ = (nifty_return_pct_ <= -0.35);
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
    [[nodiscard]] StrategyMode mode() const noexcept { return mode_; }

private:
    double volume_surge_multiple_{2.5};
    double min_composite_ofi_{0.55};
    std::unordered_set<std::string> whitelist_;
    core::Price tick_size_paise_{5};
    double circuit_buffer_pct_{1.5};
    double take_profit_pct_{1.75};
    double stop_loss_pct_{0.85};
    double min_bid_depth_ratio_{1.25};
    StrategyMode mode_{StrategyMode::COMPOUNDER};

    std::unordered_map<uint32_t, SymbolIndicatorState> symbol_states_;
    std::unordered_map<std::string, uint32_t> symbol_to_token_;
    uint32_t last_active_token_{0};
    uint32_t session_wins_{0};

    core::Price nifty_open_paise_{0};
    core::Price nifty_last_paise_{0};
    double nifty_return_pct_{0.0};
    bool is_macro_bearish_{false};
};

} // namespace deltauno::strategy
