#pragma once

#include "tradeuno/strategy/strategy_interface.hpp"
#include <deque>
#include <unordered_set>
#include <string>

namespace tradeuno::strategy {

class MomentumBreakoutStrategy : public IStrategy {
public:
    explicit MomentumBreakoutStrategy(double volume_surge_multiple = 2.5,
                                      double min_composite_ofi = 0.55)
        : volume_surge_multiple_(volume_surge_multiple),
          min_composite_ofi_(min_composite_ofi) {
        // Whitelist liquid NSE stocks under ₹1,000
        whitelist_.insert("TATASTEEL");
        whitelist_.insert("SBIN");
        whitelist_.insert("ITC");
        whitelist_.insert("COALINDIA");
        whitelist_.insert("ZOMATO");
    }

    void on_tick(const core::MarketTick& tick, const market::OrderBook& /* book */) override {
        // Track cumulative volume and price for VWAP calculation
        if (tick.last_quantity > 0) {
            cumulative_turnover_ += static_cast<double>(tick.last_price) * tick.last_quantity;
            cumulative_volume_ += tick.last_quantity;
        }

        // Rolling volume delta tracking
        if (prev_total_volume_ > 0 && tick.total_volume >= prev_total_volume_) {
            uint64_t delta_vol = tick.total_volume - prev_total_volume_;
            recent_volumes_.push_back(delta_vol);
            if (recent_volumes_.size() > 20) {
                recent_volumes_.pop_front();
            }
        }
        prev_total_volume_ = tick.total_volume;
    }

    [[nodiscard]] std::optional<core::TradingSignal> generate_signal(const market::OrderBook& book) override {
        if (whitelist_.find(book.symbol()) == whitelist_.end()) {
            return std::nullopt; // Not in liquid whitelist
        }

        if (recent_volumes_.size() < 10) {
            return std::nullopt; // Insufficient warmup
        }

        // 1. Check Volume Acceleration
        double avg_vol = 0.0;
        for (size_t i = 0; i < recent_volumes_.size() - 1; ++i) {
            avg_vol += recent_volumes_[i];
        }
        avg_vol /= static_cast<double>(recent_volumes_.size() - 1);

        uint64_t latest_vol = recent_volumes_.back();
        bool has_volume_surge = (avg_vol > 0) && (static_cast<double>(latest_vol) >= avg_vol * volume_surge_multiple_);

        // 2. Check Depth-Normalized Composite OFI (Cont 2014 & Kolm 2023)
        double ofi = book.composite_ofi();
        bool has_ofi_support = (ofi >= min_composite_ofi_);

        // 3. Check VWAP Alignment
        core::Price vwap = get_vwap();
        bool is_above_vwap = (vwap == 0) || (book.best_bid() >= vwap);

        // 4. Circuit proximity veto
        if (book.is_near_circuit(1.5)) {
            return std::nullopt;
        }

        if (has_volume_surge && has_ofi_support && is_above_vwap) {
            core::TradingSignal sig{};
            sig.timestamp_ns = book.last_tick_time_ns();
            sig.token = book.token();
#if defined(_MSC_VER)
            strncpy_s(sig.symbol, book.symbol().c_str(), sizeof(sig.symbol) - 1);
#else
            std::strncpy(sig.symbol, book.symbol().c_str(), sizeof(sig.symbol) - 1);
#endif
            sig.side = core::Side::BUY;
            sig.target_entry = book.best_ask();

            // Set Stop Loss at ~1.0% below entry
            sig.stop_loss = static_cast<core::Price>(sig.target_entry * 0.990);

            // Set Take Profit at ~1.8% above entry (1:1.8 Risk:Reward)
            sig.take_profit = static_cast<core::Price>(sig.target_entry * 1.018);

            sig.confidence = std::min(1.0, ofi);
            sig.source = core::SignalSource::INTERNAL_MOMENTUM;
            sig.is_valid = true;
            return sig;
        }

        return std::nullopt;
    }

    void on_trade_fill(const core::TradeAuditEntry& /* trade */) override {
        // Strategy feedback loop (can adapt thresholds based on win/loss)
    }

    [[nodiscard]] core::Price get_vwap() const noexcept {
        if (cumulative_volume_ == 0) return 0;
        return static_cast<core::Price>(cumulative_turnover_ / static_cast<double>(cumulative_volume_));
    }

private:
    double volume_surge_multiple_{2.5};
    double min_composite_ofi_{0.55};
    std::unordered_set<std::string> whitelist_;

    uint64_t prev_total_volume_{0};
    std::deque<uint64_t> recent_volumes_;

    double cumulative_turnover_{0.0};
    uint64_t cumulative_volume_{0};
};

} // namespace tradeuno::strategy
