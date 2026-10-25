#pragma once

#include "tradeuno/core/types.hpp"
#include <array>
#include <deque>
#include <numeric>
#include <algorithm>
#include <cmath>

namespace tradeuno::market {

using namespace tradeuno::core;

class OrderBook {
public:
    explicit OrderBook(uint32_t token = 0, std::string symbol = "")
        : token_(token), symbol_(std::move(symbol)) {}

    void on_tick(const MarketTick& tick) noexcept {
        last_tick_time_ns_ = tick.timestamp_ns;
        last_price_ = tick.last_price;
        upper_circuit_ = tick.upper_circuit;
        lower_circuit_ = tick.lower_circuit;
        total_volume_ = tick.total_volume;

        // Calculate single-tick Order Flow Imbalance (OFI)
        // OFI = delta(BidSize) - delta(AskSize) at best levels
        int64_t delta_bid = 0;
        int64_t delta_ask = 0;

        if (has_previous_tick_) {
            // Bid side comparison
            if (tick.bids[0].price > prev_bids_[0].price) {
                delta_bid = static_cast<int64_t>(tick.bids[0].quantity);
            } else if (tick.bids[0].price == prev_bids_[0].price) {
                delta_bid = static_cast<int64_t>(tick.bids[0].quantity) - static_cast<int64_t>(prev_bids_[0].quantity);
            } else {
                delta_bid = -static_cast<int64_t>(prev_bids_[0].quantity);
            }

            // Ask side comparison
            if (tick.asks[0].price < prev_asks_[0].price) {
                delta_ask = static_cast<int64_t>(tick.asks[0].quantity);
            } else if (tick.asks[0].price == prev_asks_[0].price) {
                delta_ask = static_cast<int64_t>(tick.asks[0].quantity) - static_cast<int64_t>(prev_asks_[0].quantity);
            } else {
                delta_ask = -static_cast<int64_t>(prev_asks_[0].quantity);
            }
        }

        int64_t instant_ofi = delta_bid - delta_ask;

        // Total top-5 depth for normalization (Cont, Kukanov, Stoikov 2014)
        uint64_t total_depth = 0;
        for (size_t i = 0; i < 5; ++i) {
            bids_[i] = tick.bids[i];
            asks_[i] = tick.asks[i];
            total_depth += tick.bids[i].quantity + tick.asks[i].quantity;
        }

        double normalized_ofi = 0.0;
        if (total_depth > 0) {
            normalized_ofi = static_cast<double>(instant_ofi) / (static_cast<double>(total_depth) * 0.5);
            normalized_ofi = std::clamp(normalized_ofi, -1.0, 1.0);
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

    [[nodiscard]] Price best_bid() const noexcept {
        return bids_[0].price;
    }

    [[nodiscard]] Price best_ask() const noexcept {
        return asks_[0].price;
    }

    [[nodiscard]] Quantity best_bid_qty() const noexcept {
        return bids_[0].quantity;
    }

    [[nodiscard]] Quantity best_ask_qty() const noexcept {
        return asks_[0].quantity;
    }

    [[nodiscard]] Price mid_price() const noexcept {
        if (bids_[0].price == 0 || asks_[0].price == 0) return last_price_;
        return (bids_[0].price + asks_[0].price) / 2;
    }

    [[nodiscard]] Price spread() const noexcept {
        if (bids_[0].price == 0 || asks_[0].price == 0) return 0;
        return asks_[0].price - bids_[0].price;
    }

    [[nodiscard]] double composite_ofi() const noexcept {
        if (ofi_history_.empty()) return 0.0;

        // Weight: 0.5 * OFI_1 + 0.3 * OFI_5_avg + 0.2 * OFI_20_avg
        double ofi_1 = ofi_history_.back();

        double ofi_5 = 0.0;
        size_t count_5 = std::min<size_t>(5, ofi_history_.size());
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
        if (last_price_ == 0) return false;

        if (upper_circuit_ > 0) {
            double dist_to_upper = static_cast<double>(upper_circuit_ - last_price_) / static_cast<double>(last_price_) * 100.0;
            if (dist_to_upper <= buffer_pct) return true;
        }

        if (lower_circuit_ > 0) {
            double dist_to_lower = static_cast<double>(last_price_ - lower_circuit_) / static_cast<double>(last_price_) * 100.0;
            if (dist_to_lower <= buffer_pct) return true;
        }

        return false;
    }

    [[nodiscard]] uint64_t last_tick_time_ns() const noexcept {
        return last_tick_time_ns_;
    }

    [[nodiscard]] Price last_price() const noexcept {
        return last_price_;
    }

    [[nodiscard]] uint64_t total_volume() const noexcept {
        return total_volume_;
    }

    [[nodiscard]] uint32_t token() const noexcept {
        return token_;
    }

    [[nodiscard]] const std::string& symbol() const noexcept {
        return symbol_;
    }

private:
    uint32_t token_{0};
    std::string symbol_;
    Price last_price_{0};
    Price upper_circuit_{0};
    Price lower_circuit_{0};
    uint64_t total_volume_{0};
    uint64_t last_tick_time_ns_{0};

    std::array<DepthLevel, 5> bids_{};
    std::array<DepthLevel, 5> asks_{};
    std::array<DepthLevel, 5> prev_bids_{};
    std::array<DepthLevel, 5> prev_asks_{};
    bool has_previous_tick_{false};

    std::deque<double> ofi_history_;
};

} // namespace tradeuno::market
