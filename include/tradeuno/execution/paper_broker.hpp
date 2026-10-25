#pragma once

#include "tradeuno/execution/execution_interface.hpp"
#include "tradeuno/market/order_book.hpp"
#include <deque>
#include <vector>
#include <unordered_map>
#include <cmath>

namespace tradeuno::execution {

class PaperBroker : public IExecutionGateway {
public:
    explicit PaperBroker(double initial_cash_inr = 1000.0)
        : cash_balance_inr_(initial_cash_inr) {}

    bool place_order(const core::Order& order) override {
        // Enforce Zero Look-Ahead Bias: order cannot fill immediately on the tick that triggered it
        // It enters the pending queue to be matched against the NEXT market tick
        pending_orders_.push_back(order);
        return true;
    }

    bool cancel_order(uint64_t client_order_id) override {
        auto it = std::remove_if(pending_orders_.begin(), pending_orders_.end(),
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

    // Called on every new tick (Tick[t+1]) to simulate matching
    void on_next_tick(const core::MarketTick& tick, const market::OrderBook& book) {
        if (pending_orders_.empty()) return;

        std::deque<core::Order> remaining;
        for (auto& order : pending_orders_) {
            if (order.token != tick.token) {
                remaining.push_back(order);
                continue;
            }

            // Must match on subsequent tick
            if (tick.timestamp_ns <= order.created_time_ns) {
                remaining.push_back(order);
                continue;
            }

            match_order(order, book, tick.timestamp_ns);
        }
        pending_orders_ = std::move(remaining);
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

        if (base_price == 0) {
            base_price = book.last_price();
            available_qty = 1000;
        }

        // Almgren-Chriss Slippage Impact Model
        double impact_factor = 0.0001; // Base impact
        if (available_qty > 0) {
            impact_factor += 0.0001 * (static_cast<double>(order.quantity) / static_cast<double>(available_qty));
        }

        core::Price final_fill_price = base_price;
        if (order.side == core::Side::BUY) {
            final_fill_price = static_cast<core::Price>(base_price * (1.0 + impact_factor));
        } else {
            final_fill_price = static_cast<core::Price>(base_price * (1.0 - impact_factor));
        }

        order.status = core::OrderStatus::FILLED;
        order.filled_quantity = order.quantity;
        order.avg_fill_price = final_fill_price;
        order.updated_time_ns = fill_time_ns;

        process_fill(order, fill_time_ns);
    }

    void process_fill(const core::Order& order, uint64_t fill_time_ns) {
        if (order.side == core::Side::BUY) {
            // Opening long position
            active_long_ = order;
            has_long_ = true;
            core::TradeAuditEntry empty_audit{};
            if (fill_callback_) {
                fill_callback_(order, empty_audit);
            }
        } else if (order.side == core::Side::SELL && has_long_) {
            // Closing long position -> calculate statutory taxes and PnL
            core::TradeAuditEntry audit{};
            audit.trade_id = ++next_trade_id_;
#if defined(_MSC_VER)
            strncpy_s(audit.symbol, order.symbol, sizeof(audit.symbol) - 1);
#else
            std::strncpy(audit.symbol, order.symbol, sizeof(audit.symbol) - 1);
#endif
            audit.side = core::Side::BUY;
            audit.entry_price = active_long_.avg_fill_price;
            audit.exit_price = order.avg_fill_price;
            audit.quantity = order.quantity;
            audit.entry_time_ns = active_long_.updated_time_ns;
            audit.exit_time_ns = fill_time_ns;

            double buy_val = core::to_inr(audit.entry_price * audit.quantity);
            double sell_val = core::to_inr(audit.exit_price * audit.quantity);
            double turnover = buy_val + sell_val;

            audit.gross_pnl_inr = sell_val - buy_val;

            // Indian Statutory Charges (Intraday Equity)
            audit.stt_inr = sell_val * 0.00025; // 0.025% on sell
            audit.exchange_fee_inr = turnover * 0.0000345; // 0.00345%
            audit.gst_inr = audit.exchange_fee_inr * 0.18; // 18% GST on exchange fee (zero-brokerage)
            audit.stamp_duty_inr = buy_val * 0.00003; // 0.003% on buy
            audit.sebi_turnover_inr = turnover * 0.000001; // ₹10 per crore

            double total_friction = audit.stt_inr + audit.exchange_fee_inr + audit.gst_inr +
                                   audit.stamp_duty_inr + audit.sebi_turnover_inr;

            audit.net_pnl_inr = audit.gross_pnl_inr - total_friction;

            if (audit.gross_pnl_inr > 0) {
                audit.friction_ratio = total_friction / audit.gross_pnl_inr;
            } else {
                audit.friction_ratio = 1.0;
            }

            cash_balance_inr_ += audit.net_pnl_inr;
            trade_history_.push_back(audit);
            has_long_ = false;

            if (fill_callback_) {
                fill_callback_(order, audit);
            }
        }
    }

    double cash_balance_inr_{1000.0};
    std::deque<core::Order> pending_orders_;
    core::Order active_long_{};
    bool has_long_{false};
    uint64_t next_trade_id_{0};
    std::vector<core::TradeAuditEntry> trade_history_;
    OrderFillCallback fill_callback_;
};

} // namespace tradeuno::execution
