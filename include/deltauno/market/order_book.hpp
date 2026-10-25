#pragma once

#include "deltauno/core/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string>

namespace deltauno::market {

class OrderBook {
public:
    explicit OrderBook(uint32_t token = 0, std::string symbol = "")
        : token_(token), symbol_(std::move(symbol)) {}

    void on_tick(const core::MarketTick& tick) noexcept {
        last_tick_time_ns_ = tick.timestamp_ns;
        last_price_ = tick.last_price;
        upper_circuit_ = tick.upper_circuit;
        lower_circuit_ = tick.lower_circuit;
        total_volume_ = tick.total_volume;
        if (symbol_.empty() && tick.symbol[0] != '\0') {
            symbol_ = tick.symbol;
        }
        if (token_ == 0 && tick.token != 0) {
            token_ = tick.token;
        }

        // Multi-level Depth-Normalized Order Flow Imbalance (Cont et al. 2014; Kolm et al. 2023)
        // Exponential decay across L2 levels m = 0..4 emphasizes top-of-book while capturing queue support
        static constexpr std::array<double, 5> kLevelWeights = {1.00, 0.70, 0.50, 0.35, 0.25};

        double weighted_ofi = 0.0;
        double weighted_depth = 0.0;

        for (size_t m = 0; m < 5; ++m) {
            if (has_previous_tick_) {
                int64_t delta_bid = 0;
                if (tick.bids[m].price > prev_bids_[m].price) {
                    delta_bid = static_cast<int64_t>(tick.bids[m].quantity);
                } else if (tick.bids[m].price == prev_bids_[m].price) {
                    delta_bid = static_cast<int64_t>(tick.bids[m].quantity) -
                                static_cast<int64_t>(prev_bids_[m].quantity);
                } else {
                    delta_bid = -static_cast<int64_t>(prev_bids_[m].quantity);
                }

                int64_t delta_ask = 0;
                if (tick.asks[m].price < prev_asks_[m].price && tick.asks[m].price > 0) {
                    delta_ask = static_cast<int64_t>(tick.asks[m].quantity);
                } else if (tick.asks[m].price == prev_asks_[m].price) {
                    delta_ask = static_cast<int64_t>(tick.asks[m].quantity) -
                                static_cast<int64_t>(prev_asks_[m].quantity);
                } else {
                    delta_ask = -static_cast<int64_t>(prev_asks_[m].quantity);
                }

                weighted_ofi += kLevelWeights[m] * static_cast<double>(delta_bid - delta_ask);
            }

            bids_[m] = tick.bids[m];
            asks_[m] = tick.asks[m];

            const uint64_t level_qty = static_cast<uint64_t>(tick.bids[m].quantity) +
                                       static_cast<uint64_t>(tick.asks[m].quantity);
            weighted_depth += kLevelWeights[m] * (static_cast<double>(level_qty) * 0.5);
        }

        double normalized_ofi = 0.0;
        if (has_previous_tick_ && weighted_depth > 0.0) {
            normalized_ofi = std::clamp(weighted_ofi / weighted_depth, -1.0, 1.0);
        }

        // Maintain multi-horizon history (Kolm et al. 2023)
        ofi_history_.push_back(normalized_ofi);
        if (ofi_history_.size() > 20) {
            ofi_history_.pop_front();
        }

        prev_bids_ = bids_;
        prev_asks_ = asks_;
        has_previous_tick_ = true;
    }

    [[nodiscard]] core::Price best_bid() const noexcept {
        return bids_[0].price;
    }

    [[nodiscard]] core::Price best_ask() const noexcept {
        return asks_[0].price;
    }

    [[nodiscard]] core::Quantity best_bid_qty() const noexcept {
        return bids_[0].quantity;
    }

    [[nodiscard]] core::Quantity best_ask_qty() const noexcept {
        return asks_[0].quantity;
    }

    [[nodiscard]] core::Price mid_price() const noexcept {
        if (bids_[0].price == 0 || asks_[0].price == 0) return last_price_;
        return (bids_[0].price + asks_[0].price) / 2;
    }

    // Volume-imbalance weighted microprice: (P_bid * Q_ask + P_ask * Q_bid) / (Q_bid + Q_ask)
    [[nodiscard]] core::Price weighted_mid_price() const noexcept {
        const uint64_t q_sum = static_cast<uint64_t>(bids_[0].quantity) + static_cast<uint64_t>(asks_[0].quantity);
        if (bids_[0].price == 0 || asks_[0].price == 0 || q_sum == 0) {
            return mid_price();
        }
        const int64_t num = bids_[0].price * static_cast<int64_t>(asks_[0].quantity) +
                            asks_[0].price * static_cast<int64_t>(bids_[0].quantity);
        return static_cast<core::Price>(num / static_cast<int64_t>(q_sum));
    }

    [[nodiscard]] core::Price spread() const noexcept {
        if (bids_[0].price == 0 || asks_[0].price == 0) return 0;
        return asks_[0].price - bids_[0].price;
    }

    [[nodiscard]] double composite_ofi() const noexcept {
        if (ofi_history_.empty()) return 0.0;

        // Multi-horizon blend: 0.5 * OFI_1 + 0.3 * OFI_5_avg + 0.2 * OFI_20_avg
        const double ofi_1 = ofi_history_.back();

        double ofi_5 = 0.0;
        const size_t count_5 = std::min<size_t>(5, ofi_history_.size());
        for (size_t i = 0; i < count_5; ++i) {
            ofi_5 += ofi_history_[ofi_history_.size() - 1 - i];
        }
        ofi_5 /= static_cast<double>(count_5);

        double ofi_20 = 0.0;
        for (double v : ofi_history_) {
            ofi_20 += v;
        }
        ofi_20 /= static_cast<double>(ofi_history_.size());

        return (0.5 * ofi_1) + (0.3 * ofi_5) + (0.2 * ofi_20);
    }

    [[nodiscard]] bool is_near_circuit(double buffer_pct = 1.5) const noexcept {
        if (last_price_ <= 0) return false;

        if (upper_circuit_ > 0) {
            const double dist_to_upper =
                static_cast<double>(upper_circuit_ - last_price_) / static_cast<double>(last_price_) * 100.0;
            if (dist_to_upper <= buffer_pct) return true;
        }

        if (lower_circuit_ > 0) {
            const double dist_to_lower =
                static_cast<double>(last_price_ - lower_circuit_) / static_cast<double>(last_price_) * 100.0;
            if (dist_to_lower <= buffer_pct) return true;
        }

        return false;
    }

    [[nodiscard]] uint64_t last_tick_time_ns() const noexcept { return last_tick_time_ns_; }
    [[nodiscard]] core::Price last_price() const noexcept { return last_price_; }
    [[nodiscard]] uint64_t total_volume() const noexcept { return total_volume_; }
    [[nodiscard]] uint32_t token() const noexcept { return token_; }
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }

    /// Flushes overnight depth states and OFI history to prevent overnight price gap corruption.
    void reset_session() noexcept {
        has_previous_tick_ = false;
        bids_.fill(core::DepthLevel{});
        asks_.fill(core::DepthLevel{});
        prev_bids_.fill(core::DepthLevel{});
        prev_asks_.fill(core::DepthLevel{});
        ofi_history_.clear();
        total_volume_ = 0;
        last_price_ = 0;
        upper_circuit_ = 0;
        lower_circuit_ = 0;
        last_tick_time_ns_ = 0;
    }

    /// Alias conforming to PROJECT.md Contract 3
    void reset_daily_session() noexcept {
        reset_session();
    }

    [[nodiscard]] bool has_previous_tick() const noexcept { return has_previous_tick_; }
    [[nodiscard]] size_t ofi_history_size() const noexcept { return ofi_history_.size(); }

private:
    uint32_t token_{0};
    std::string symbol_;
    core::Price last_price_{0};
    core::Price upper_circuit_{0};
    core::Price lower_circuit_{0};
    uint64_t total_volume_{0};
    uint64_t last_tick_time_ns_{0};

    std::array<core::DepthLevel, 5> bids_{};
    std::array<core::DepthLevel, 5> asks_{};
    std::array<core::DepthLevel, 5> prev_bids_{};
    std::array<core::DepthLevel, 5> prev_asks_{};
    bool has_previous_tick_{false};

    std::deque<double> ofi_history_;
};

} // namespace deltauno::market
