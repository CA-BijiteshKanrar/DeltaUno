#include "tradeuno/core/types.hpp"
#include "tradeuno/core/ring_buffer.hpp"
#include "tradeuno/core/async_logger.hpp"
#include "tradeuno/core/state_journal.hpp"
#include "tradeuno/market/order_book.hpp"
#include "tradeuno/market/csv_feed.hpp"
#include "tradeuno/market/watchdog.hpp"
#include "tradeuno/strategy/momentum_breakout.hpp"
#include "tradeuno/strategy/signal_ingestor.hpp"
#include "tradeuno/risk/risk_manager.hpp"
#include "tradeuno/execution/paper_broker.hpp"
#include "tradeuno/execution/trade_auditor.hpp"

#include <iostream>
#include <memory>
#include <iomanip>

using namespace tradeuno;

int main(int argc, char* argv[]) {
    std::string csv_path = "data/sample_nse_ticks.csv";
    if (argc > 1) {
        csv_path = argv[1];
    }

    // 1. Initialize Non-Blocking Async Logger
    core::AsyncLogger::instance().init("deltauno.log");
    LOG_INFO("=================================================");
    LOG_INFO("   DeltaUno Low-Latency C++ Trading Engine       ");
    LOG_INFO("   Target: NSE/BSE Equities | Micro-Capital: ₹1,000");
    LOG_INFO("=================================================");

    // 2. State Journal / Crash Recovery
    core::StateJournal journal("deltauno_state.json");
    auto recovered_pos = journal.recover_position();
    if (recovered_pos.has_value()) {
        LOG_WARN("CRASH RECOVERY: Found open position for " + std::string(recovered_pos->symbol) +
                 " Qty: " + std::to_string(recovered_pos->quantity) +
                 " @ Entry: " + std::to_string(core::to_inr(recovered_pos->avg_entry_price)) + " INR");
    } else {
        LOG_INFO("State Journal clean. No orphaned positions found.");
    }

    // 3. Initialize Risk Manager with ₹1,000 starting capital
    risk::RiskManager risk_mgr(1000.0, 12.0, 50.0, 500.0);
    LOG_INFO("Risk Manager active: Capital=1000 INR | MaxRisk/Trade=12 INR | KillSwitch=50 INR");

    // 4. Initialize Execution Gateway (Paper Broker)
    execution::PaperBroker broker(1000.0);

    // 5. Initialize Strategy and BS Detector Ingestor
    strategy::MomentumBreakoutStrategy momentum_strat(2.0, 0.25);
    strategy::SignalIngestor bs_detector;

    // 6. Setup Order Book and Tracking State
    market::OrderBook book(349121, "TATASTEEL");
    core::Position current_position{};
    bool in_trade = false;
    uint64_t next_order_id = 1;

    // 7. Register broker fill callback
    broker.register_fill_callback([&](const core::Order& order, const core::TradeAuditEntry& audit) {
        if (order.side == core::Side::BUY) {
            in_trade = true;
            current_position.token = order.token;
#if defined(_MSC_VER)
            strncpy_s(current_position.symbol, order.symbol, sizeof(current_position.symbol) - 1);
#else
            std::strncpy(current_position.symbol, order.symbol, sizeof(current_position.symbol) - 1);
#endif
            current_position.side = core::Side::BUY;
            current_position.quantity = order.filled_quantity;
            current_position.avg_entry_price = order.avg_fill_price;
            current_position.current_price = order.avg_fill_price;
            current_position.current_stop_loss = static_cast<core::Price>(order.avg_fill_price * 0.990);
            current_position.initial_stop_loss = current_position.current_stop_loss;
            current_position.take_profit = static_cast<core::Price>(order.avg_fill_price * 1.018);
            current_position.is_breakeven_set = false;
            current_position.open_time_ns = order.updated_time_ns;

            risk_mgr.set_has_active_position(true);
            journal.save_position(current_position);

            std::ostringstream ss;
            ss << "FILLED BUY: " << order.symbol << " Qty=" << order.filled_quantity
               << " @ " << std::fixed << std::setprecision(2) << core::to_inr(order.avg_fill_price) << " INR"
               << " | SL=" << core::to_inr(current_position.current_stop_loss)
               << " | TP=" << core::to_inr(current_position.take_profit);
            LOG_TRADE(ss.str());
        } else if (order.side == core::Side::SELL) {
            in_trade = false;
            risk_mgr.on_trade_closed(audit);
            journal.clear_position();

            std::ostringstream ss;
            ss << "FILLED SELL: " << order.symbol << " Qty=" << audit.quantity
               << " @ " << std::fixed << std::setprecision(2) << core::to_inr(audit.exit_price) << " INR"
               << " | Gross PnL: " << (audit.gross_pnl_inr >= 0 ? "+" : "") << audit.gross_pnl_inr << " INR"
               << " | Taxes: -" << (audit.stt_inr + audit.exchange_fee_inr + audit.gst_inr + audit.stamp_duty_inr) << " INR"
               << " | Net PnL: " << (audit.net_pnl_inr >= 0 ? "+" : "") << audit.net_pnl_inr << " INR"
               << " | Balance: " << broker.cash_balance_inr() << " INR";
            LOG_TRADE(ss.str());
        }
    });

    // 8. Market Feed Replayer
    market::CSVFeed feed(csv_path);
    LOG_INFO("Connecting to CSV Market Feed: " + csv_path);

    feed.register_callback([&](const core::MarketTick& tick) {
        // Update Order Book
        book.on_tick(tick);

        // A. Match pending orders on Tick[t+1] (Zero Look-Ahead Bias)
        broker.on_next_tick(tick, book);

        // B. If currently in trade, evaluate position risk rules (Trailing stop / profit / 15:05 cutoff)
        if (in_trade) {
            auto [should_exit, exit_reason] = risk_mgr.check_position_rules(current_position, tick.last_price, tick.timestamp_ns);
            if (should_exit) {
                LOG_INFO("Position Exit Triggered: " + exit_reason + " @ " + std::to_string(core::to_inr(tick.last_price)) + " INR");

                core::Order exit_order{};
                exit_order.client_order_id = next_order_id++;
                exit_order.token = current_position.token;
#if defined(_MSC_VER)
                strncpy_s(exit_order.symbol, current_position.symbol, sizeof(exit_order.symbol) - 1);
#else
                std::strncpy(exit_order.symbol, current_position.symbol, sizeof(exit_order.symbol) - 1);
#endif
                exit_order.side = core::Side::SELL;
                exit_order.type = core::OrderType::MARKET;
                exit_order.quantity = current_position.quantity;
                exit_order.created_time_ns = tick.timestamp_ns;

                broker.place_order(exit_order);
            }
            return;
        }

        // C. Update strategy with tick
        momentum_strat.on_tick(tick, book);

        // D. Generate signal
        auto signal_opt = momentum_strat.generate_signal(book);
        if (signal_opt.has_value() && signal_opt->is_valid) {
            const auto& sig = signal_opt.value();

            // Evaluate risk & size order
            auto [sized_qty, rejection] = risk_mgr.evaluate_and_size_order(sig, tick.timestamp_ns);
            if (rejection != risk::RiskRejection::NONE) {
                LOG_DEBUG("Order rejected by RiskManager: " + std::string(risk::risk_rejection_to_string(rejection)));
                return;
            }

            // Create Order
            core::Order new_order{};
            new_order.client_order_id = next_order_id++;
            new_order.token = sig.token;
#if defined(_MSC_VER)
            strncpy_s(new_order.symbol, sig.symbol, sizeof(new_order.symbol) - 1);
#else
            std::strncpy(new_order.symbol, sig.symbol, sizeof(new_order.symbol) - 1);
#endif
            new_order.side = sig.side;
            new_order.type = core::OrderType::MARKET;
            new_order.price = sig.target_entry;
            new_order.quantity = sized_qty;
            new_order.created_time_ns = tick.timestamp_ns;

            std::ostringstream ss;
            ss << "SIGNAL ACCEPTED: " << sig.symbol << " BUY Qty=" << sized_qty
               << " @ Target=" << core::to_inr(sig.target_entry) << " INR"
               << " | OFI=" << std::fixed << std::setprecision(2) << book.composite_ofi();
            LOG_INFO(ss.str());

            // Place order into execution gateway
            broker.place_order(new_order);
        }
    });

    // 9. Run the feed
    LOG_INFO("Starting Market Feed Processing...");
    feed.start();
    LOG_INFO("Feed replay complete.");

    // 10. Post-Trade Performance Audit Report
    auto audit_summary = execution::TradeAuditor::evaluate(broker.trade_history());
    execution::TradeAuditor::print_report(audit_summary, std::cout);

    LOG_INFO("Engine shutdown cleanly. DeltaUno session concluded.");
    core::AsyncLogger::instance().shutdown();

    return 0;
}
