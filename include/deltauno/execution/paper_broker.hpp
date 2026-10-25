#pragma once

#include "deltauno/execution/execution_interface.hpp"
#include "deltauno/market/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace deltauno::execution {

class PaperBroker : public IExecutionGateway {
public:
    explicit PaperBroker(double initial_cash_inr = 10000.0)
        : cash_balance_inr_(initial_cash_inr) {}

    bool place_order(const core::Order& order) override {
        if (order.quantity == 0) return false;

        // Guard single-position lifecycle against duplicate pending orders
        const bool has_same_side_pending = std::any_of(
            pending_orders_.begin(), pending_orders_.end(),
            [&order](const core::Order& p) { return p.side == order.side && p.token == order.token; });
        if (has_same_side_pending) {
            return false;
        }
        if (order.side == core::Side::BUY && has_long_) {
            return false;
        }
        if (order.side == core::Side::SELL && !has_long_) {
            return false;
        }

        // Enforce Zero Look-Ahead Bias: order enters pending queue to fill strictly on Tick[t+1]
        pending_orders_.push_back(order);
        return true;
    }

    bool cancel_order(uint64_t client_order_id) override {
        const auto it = std::remove_if(
            pending_orders_.begin(), pending_orders_.end(),
            [client_order_id](const core::Order& o) { return o.client_order_id == client_order_id; });
        if (it != pending_orders_.end()) {
            pending_orders_.erase(it, pending_orders_.end());
            return true;
        }
        return false;
    }

    void register_fill_callback(OrderFillCallback cb) override {
        fill_callback_ = std::move(cb);
    }

    void restore_open_position(const core::Position& pos) override {
        if (pos.quantity == 0 || pos.avg_entry_price <= 0) return;

        core::Order restored{};
        restored.token = pos.token;
        core::copy_fixed_str(restored.symbol, pos.symbol);
        restored.side = pos.side;
        restored.type = core::OrderType::MARKET;
        restored.quantity = pos.quantity;
        restored.filled_quantity = pos.quantity;
        restored.avg_fill_price = pos.avg_entry_price;
        restored.status = core::OrderStatus::FILLED;
        restored.created_time_ns = pos.open_time_ns;
        restored.updated_time_ns = pos.open_time_ns;

        active_long_ = restored;
        has_long_ = true;
        peak_favorable_price_ = (pos.peak_favorable_price > 0) ? pos.peak_favorable_price : pos.avg_entry_price;
        worst_adverse_price_ = (pos.worst_adverse_price > 0) ? pos.worst_adverse_price : pos.avg_entry_price;
    }

    // Called on every subsequent tick (Tick[t+1]) to track excursion and simulate order matching
    void on_next_tick(const core::MarketTick& tick, const market::OrderBook& book) {
        if (has_long_ && tick.token == active_long_.token) {
            update_excursion(tick.last_price);
            update_excursion(book.best_bid());
        }

        if (pending_orders_.empty()) return;

        std::deque<core::Order> remaining;
        for (auto& order : pending_orders_) {
            if (order.token != tick.token) {
                remaining.push_back(order);
                continue;
            }

            // Strict Zero Look-Ahead Bias: must match on a strictly newer timestamp
            if (tick.timestamp_ns <= order.created_time_ns) {
                remaining.push_back(order);
                continue;
            }

            match_order(order, book, tick.timestamp_ns);
        }
        pending_orders_ = std::move(remaining);
    }

    [[nodiscard]] bool has_open_long() const noexcept {
        return has_long_;
    }

    [[nodiscard]] size_t pending_orders_count() const noexcept {
        return pending_orders_.size();
    }

    [[nodiscard]] double cash_balance_inr() const noexcept {
        return cash_balance_inr_;
    }

    [[nodiscard]] size_t total_trades_count() const noexcept {
        return trade_history_.size();
    }

    [[nodiscard]] const std::vector<core::TradeAuditEntry>& trade_history() const noexcept {
        return trade_history_;
    }

private:
    void update_excursion(core::Price observed_price) noexcept {
        if (observed_price <= 0) return;
        if (peak_favorable_price_ == 0 || observed_price > peak_favorable_price_) {
            peak_favorable_price_ = observed_price;
        }
        if (worst_adverse_price_ == 0 || observed_price < worst_adverse_price_) {
            worst_adverse_price_ = observed_price;
        }
    }

    void match_order(core::Order& order, const market::OrderBook& book, uint64_t fill_time_ns) {
        core::Price base_price = 0;
        core::Quantity available_qty = 0;

        if (order.side == core::Side::BUY) {
            base_price = book.best_ask();
            available_qty = book.best_ask_qty();
        } else {
            base_price = book.best_bid();
            available_qty = book.best_bid_qty();
        }

        if (base_price <= 0) {
            base_price = book.last_price();
            available_qty = 1000;
        }

        // Almgren-Chriss Non-Linear Slippage Impact Model (ceil on BUY, floor on SELL so impact is never truncated away)
        double impact_factor = 0.0001; // 1 bps base market impact
        if (available_qty > 0) {
            impact_factor += 0.0001 * (static_cast<double>(order.quantity) / static_cast<double>(available_qty));
        }

        core::Price final_fill_price = base_price;
        if (order.side == core::Side::BUY) {
            final_fill_price = static_cast<core::Price>(
                std::ceil(static_cast<double>(base_price) * (1.0 + impact_factor)));
        } else {
            final_fill_price = static_cast<core::Price>(
                std::floor(static_cast<double>(base_price) * (1.0 - impact_factor)));
        }

        order.status = core::OrderStatus::FILLED;
        order.filled_quantity = order.quantity;
        order.avg_fill_price = final_fill_price;
        order.updated_time_ns = fill_time_ns;

        process_fill(order, fill_time_ns);
    }

    void process_fill(const core::Order& order, uint64_t fill_time_ns) {
        if (order.side == core::Side::BUY) {
            active_long_ = order;
            has_long_ = true;
            peak_favorable_price_ = order.avg_fill_price;
            worst_adverse_price_ = order.avg_fill_price;

            core::TradeAuditEntry empty_audit{};
            if (fill_callback_) {
                fill_callback_(order, empty_audit);
            }
        } else if (order.side == core::Side::SELL && has_long_) {
            update_excursion(order.avg_fill_price);

            core::TradeAuditEntry audit{};
            audit.trade_id = ++next_trade_id_;
            core::copy_fixed_str(audit.symbol, order.symbol);
            audit.side = core::Side::BUY;
            audit.entry_price = active_long_.avg_fill_price;
            audit.exit_price = order.avg_fill_price;
            audit.quantity = order.quantity;
            audit.entry_time_ns = active_long_.updated_time_ns;
            audit.exit_time_ns = fill_time_ns;

            audit.max_adverse_excursion = std::max<core::Price>(0, audit.entry_price - worst_adverse_price_);
            audit.max_favorable_excursion = std::max<core::Price>(0, peak_favorable_price_ - audit.entry_price);

            const double buy_val = core::to_inr(audit.entry_price * static_cast<core::Price>(audit.quantity));
            const double sell_val = core::to_inr(audit.exit_price * static_cast<core::Price>(audit.quantity));
            const double turnover = buy_val + sell_val;

            audit.gross_pnl_inr = sell_val - buy_val;

            // Indian Statutory Charges (NSE Intraday Equity MIS — Zero-Brokerage Gateway)
            audit.stt_inr = sell_val * 0.00025;            // 0.025% STT on sell leg
            audit.exchange_fee_inr = turnover * 0.0000297; // 0.00297% NSE transaction charge (revised)
            audit.sebi_turnover_inr = turnover * 0.000001; // ₹10 per crore SEBI fee
            audit.gst_inr = (audit.exchange_fee_inr + audit.sebi_turnover_inr) * 0.18; // 18% GST on (exchange + SEBI)
            audit.stamp_duty_inr = buy_val * 0.00003;      // 0.003% stamp duty on buy leg

            const double total_friction = audit.total_friction_inr();

            audit.net_pnl_inr = audit.gross_pnl_inr - total_friction;

            if (audit.gross_pnl_inr > 0.0) {
                audit.friction_ratio = total_friction / audit.gross_pnl_inr;
            } else {
                audit.friction_ratio = 1.0;
            }

            cash_balance_inr_ += audit.net_pnl_inr;
            trade_history_.push_back(audit);
            has_long_ = false;
            peak_favorable_price_ = 0;
            worst_adverse_price_ = 0;

            if (fill_callback_) {
                fill_callback_(order, audit);
            }
        }
    }

    double cash_balance_inr_{10000.0};
    std::deque<core::Order> pending_orders_;
    core::Order active_long_{};
    bool has_long_{false};
    core::Price peak_favorable_price_{0};
    core::Price worst_adverse_price_{0};
    uint64_t next_trade_id_{0};
    std::vector<core::TradeAuditEntry> trade_history_;
    OrderFillCallback fill_callback_;
};

} // namespace deltauno::execution
