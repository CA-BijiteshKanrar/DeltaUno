#include "deltauno/core/async_logger.hpp"
#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/ring_buffer.hpp"
#include "deltauno/core/state_journal.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/execution/paper_broker.hpp"
#include "deltauno/execution/trade_auditor.hpp"
#include "deltauno/market/csv_feed.hpp"
#include "deltauno/market/order_book.hpp"
#include "deltauno/market/watchdog.hpp"
#include "deltauno/risk/risk_manager.hpp"
#include "deltauno/strategy/momentum_breakout.hpp"
#include "deltauno/strategy/signal_ingestor.hpp"

#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

using namespace deltauno;

namespace {

enum class PositionLifecycle : uint8_t {
    FLAT = 0,
    PENDING_ENTRY = 1,
    OPEN = 2,
    PENDING_EXIT = 3
};

std::atomic<bool> g_shutdown_requested{false};
market::CSVFeed* g_active_feed_ptr{nullptr};

void handle_signal(int /* sig */) {
    g_shutdown_requested.store(true, std::memory_order_release);
    if (g_active_feed_ptr != nullptr) {
        g_active_feed_ptr->stop();
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string csv_path = "data/sample_nse_ticks.csv";
    std::string config_dir = "config";
    std::string cli_webhook_json;
    std::string run_mode = "both"; // "both" (default), "compounder", or "harvester"
    double initial_capital_override = 0.0;
    double harv_capital_override = 0.0;
    double omni_capital_override = 0.0;
    bool realtime_pacing = false;
    bool live_tail = false;
    bool emit_events = false;
    bool verbose_logs = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--csv" && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (arg == "--config-dir" && i + 1 < argc) {
            config_dir = argv[++i];
        } else if (arg == "--webhook" && i + 1 < argc) {
            cli_webhook_json = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            run_mode = argv[++i];
        } else if (arg == "--capital" && i + 1 < argc) {
            initial_capital_override = std::stod(argv[++i]);
        } else if (arg == "--harv-capital" && i + 1 < argc) {
            harv_capital_override = std::stod(argv[++i]);
        } else if (arg == "--omni-capital" && i + 1 < argc) {
            omni_capital_override = std::stod(argv[++i]);
        } else if (arg == "--realtime") {
            realtime_pacing = true;
        } else if (arg == "--live-tail") {
            live_tail = true;
            emit_events = true;
        } else if (arg == "--emit-events") {
            emit_events = true;
        } else if (arg == "--verbose") {
            verbose_logs = true;
        } else if (!arg.empty() && arg[0] != '-') {
            csv_path = arg;
        }
    }

    // 1. Initialize Lock-Free Ring-Buffer Async Logger
    core::AsyncLogger::instance().init(
        "deltauno.log",
        verbose_logs ? core::LogLevel::DEBUG : core::LogLevel::INFO);

    // 2. Load Runtime Configurations (Risk Limits, Liquid Whitelist, Macro Event Calendar)
    core::RiskConfig risk_cfg = core::ConfigLoader::load_risk_config(config_dir + "/risk_limits.json");

    if (initial_capital_override > 0.0) {

        risk_cfg.initial_capital_inr = initial_capital_override;

    }
    LOG_INFO("=================================================");
    LOG_INFO("   DeltaUno Low-Latency C++20 Trading Engine     ");
    {
        std::ostringstream banner_cap;
        banner_cap << "   Target: NSE/BSE Equities | Capital: INR "
                   << std::fixed << std::setprecision(0) << risk_cfg.initial_capital_inr;
        LOG_INFO(banner_cap.str());
    }
    LOG_INFO("=================================================");
    const auto instruments = core::ConfigLoader::load_instruments(config_dir + "/instruments.json");
    const auto whitelist = core::ConfigLoader::to_whitelist_set(instruments);
    const core::MacroCalendar macro_calendar = core::ConfigLoader::load_macro_calendar(config_dir + "/calendar_events.json");

    std::unordered_map<uint32_t, market::OrderBook> books;
    std::unordered_map<uint32_t, core::Price> tick_sizes_paise;
    std::unordered_map<std::string, uint32_t> symbol_to_token;
    for (const auto& inst : instruments) {
        books.emplace(inst.token, market::OrderBook(inst.token, inst.symbol));
        tick_sizes_paise.emplace(inst.token, inst.tick_size_paise);
        symbol_to_token.emplace(inst.symbol, inst.token);
    }

    // 3. Initialize Execution Gateway, Watchdog & WAL State Journal
    execution::PaperBroker broker(risk_cfg.initial_capital_inr);
    execution::TradeAuditor trade_auditor(risk_cfg.initial_capital_inr);
    market::Watchdog watchdog(risk_cfg.heartbeat_timeout_seconds);
    watchdog.register_alert_callback([](const std::string& alert_msg) {
        LOG_WARN(alert_msg);
    });

    {
        std::ostringstream cfg_msg;
        cfg_msg << std::fixed << std::setprecision(0)
                << "Risk Manager active: Capital=" << risk_cfg.initial_capital_inr << " INR"
                << " | MaxRisk/Trade=" << risk_cfg.max_risk_per_trade_inr << " INR"
                << " | KillSwitch=" << risk_cfg.max_daily_drawdown_inr << " INR"
                << " | Whitelist=" << whitelist.size() << " symbols"
                << " | MacroEvents=" << macro_calendar.size();
        LOG_INFO(cfg_msg.str());
    }

    // 4. Crash Recovery via Atomic Write-Ahead State Journal
    core::StateJournal journal("deltauno_state.json");
    core::Position current_position{};
    core::TradingSignal pending_entry_signal{};
    PositionLifecycle lifecycle = PositionLifecycle::FLAT;
    uint64_t next_order_id = 1;

    const auto recovered_pos = journal.recover_position();
    if (recovered_pos.has_value()) {
        current_position = *recovered_pos;
        lifecycle = PositionLifecycle::OPEN;
        broker.restore_open_position(current_position);
        watchdog.set_has_open_position(true);

        std::ostringstream rec_msg;
        rec_msg << "CRASH RECOVERY: Restored active position for " << current_position.symbol
                << " Qty=" << current_position.quantity
                << " @ Entry=" << std::fixed << std::setprecision(2)
                << core::to_inr(current_position.avg_entry_price) << " INR"
                << " | SL=" << core::to_inr(current_position.current_stop_loss)
                << " | TP=" << core::to_inr(current_position.take_profit);
        LOG_WARN(rec_msg.str());
    } else {
        LOG_INFO("State Journal clean. No orphaned positions found.");
    }

    // 5. Initialize Strategy Pipeline & "BS Detector" Signal Ingestor
    const bool primary_is_harvester = (run_mode == "harvester");
    const bool enable_parallel_harvester = (run_mode == "both");

    core::RiskConfig primary_risk_cfg = risk_cfg;
    if (primary_is_harvester) {
        primary_risk_cfg.max_position_value_inr = 5000.0;
        primary_risk_cfg.breakeven_trigger_pct = 1.0;
        primary_risk_cfg.afternoon_end_sec = 14 * 3600 + 45 * 60;
        primary_risk_cfg.enable_ofi_harvest = true;
    }
    risk::RiskManager active_risk_mgr(primary_risk_cfg);
    if (recovered_pos.has_value()) {
        active_risk_mgr.set_has_active_position(true);
    }

    strategy::MomentumBreakoutStrategy momentum_strat(
        2.5,
        primary_is_harvester ? 0.55 : risk_cfg.min_composite_ofi,
        whitelist,
        5,
        risk_cfg.circuit_limit_buffer_pct,
        risk_cfg.take_profit_pct,
        risk_cfg.stop_loss_pct,
        risk_cfg.min_bid_depth_ratio,
        primary_is_harvester ? strategy::StrategyMode::HARVESTER : strategy::StrategyMode::COMPOUNDER);
    strategy::SignalIngestor bs_detector(
        whitelist, 2.0, risk_cfg.max_15s_volume_burst_multiple, 0.20, risk_cfg.circuit_limit_buffer_pct, 5);

    // 5b. Initialize Parallel Track B (L2 Microstructure Harvester) when running in Dual-Mode ("both")
    core::RiskConfig harv_cfg = risk_cfg;
    harv_cfg.max_position_value_inr = 5000.0;
    harv_cfg.breakeven_trigger_pct = 1.0;
    harv_cfg.afternoon_end_sec = 14 * 3600 + 45 * 60;
    harv_cfg.enable_ofi_harvest = true;

    risk::RiskManager harv_risk_mgr(harv_cfg);
    const double harv_initial_cap = (harv_capital_override > 0.0) ? harv_capital_override : risk_cfg.initial_capital_inr;
    execution::PaperBroker harv_broker(harv_initial_cap);
    execution::TradeAuditor harv_auditor(harv_initial_cap);
    strategy::MomentumBreakoutStrategy harv_strat(
        2.5, 0.55, whitelist, 5, risk_cfg.circuit_limit_buffer_pct,
        1.80, 1.00, 1.25, strategy::StrategyMode::HARVESTER);
    core::Position harv_position{};
    core::TradingSignal harv_pending_signal{};
    PositionLifecycle harv_lifecycle = PositionLifecycle::FLAT;
    uint64_t harv_next_order_id = 1000001;

    harv_broker.register_fill_callback([&](const core::Order& order, const core::TradeAuditEntry& audit) {
        const core::Price tick_step = tick_sizes_paise.count(order.token) ? tick_sizes_paise[order.token] : 5;
        if (order.side == core::Side::BUY) {
            harv_lifecycle = PositionLifecycle::OPEN;
            harv_position = core::Position{};
            harv_position.token = order.token;
            core::copy_fixed_str(harv_position.symbol, order.symbol);
            harv_position.side = core::Side::BUY;
            harv_position.quantity = order.filled_quantity;
            harv_position.avg_entry_price = order.avg_fill_price;
            harv_position.current_price = order.avg_fill_price;
            harv_position.peak_favorable_price = order.avg_fill_price;
            harv_position.worst_adverse_price = order.avg_fill_price;
            if (harv_pending_signal.is_valid && harv_pending_signal.target_entry > 0) {
                const core::Price sl_dist = harv_pending_signal.target_entry - harv_pending_signal.stop_loss;
                const core::Price tp_dist = harv_pending_signal.take_profit - harv_pending_signal.target_entry;
                harv_position.current_stop_loss = core::floor_to_tick(order.avg_fill_price - sl_dist, tick_step);
                harv_position.take_profit = core::ceil_to_tick(order.avg_fill_price + tp_dist, tick_step);
            } else {
                harv_position.current_stop_loss = core::floor_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 0.990), tick_step);
                harv_position.take_profit = core::ceil_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 1.018), tick_step);
            }
            harv_position.initial_stop_loss = harv_position.current_stop_loss;
            harv_position.is_breakeven_set = false;
            harv_position.open_time_ns = order.updated_time_ns;
            harv_risk_mgr.set_has_active_position(true);
            if (live_tail) {
                std::cout << "[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"HARVESTER\",\"side\":\"BUY\",\"symbol\":\""
                          << order.symbol << "\",\"qty\":" << order.filled_quantity
                          << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(order.avg_fill_price)
                          << ",\"sl\":" << core::to_inr(harv_position.current_stop_loss)
                          << ",\"tp\":" << core::to_inr(harv_position.take_profit)
                          << ",\"balance\":" << harv_broker.cash_balance_inr() << "}\n" << std::flush;
            }
        } else if (order.side == core::Side::SELL) {
            harv_lifecycle = PositionLifecycle::FLAT;
            harv_pending_signal = core::TradingSignal{};
            harv_risk_mgr.on_trade_closed(audit);
            harv_strat.on_trade_fill(audit);
            if (live_tail) {
                const double total_taxes = audit.stt_inr + audit.exchange_fee_inr + audit.gst_inr +
                                           audit.stamp_duty_inr + audit.sebi_turnover_inr;
                std::cout << "[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"HARVESTER\",\"side\":\"SELL\",\"symbol\":\""
                          << order.symbol << "\",\"qty\":" << audit.quantity
                          << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(audit.exit_price)
                          << ",\"gross_pnl\":" << audit.gross_pnl_inr
                          << ",\"taxes\":" << total_taxes
                          << ",\"net_pnl\":" << audit.net_pnl_inr
                          << ",\"balance\":" << harv_broker.cash_balance_inr() << "}\n" << std::flush;
            }
        }
    });

    auto try_submit_harv_signal = [&](const core::TradingSignal& sig,
                                      const market::OrderBook& book,
                                      uint64_t current_time_ns) {
        if (harv_lifecycle != PositionLifecycle::FLAT) return;
        const bool macro_frozen = macro_calendar.is_frozen(current_time_ns);
        const bool near_circuit = book.is_near_circuit(harv_cfg.circuit_limit_buffer_pct);
        const auto [sized_qty, rejection] =
            harv_risk_mgr.evaluate_and_size_order(sig, current_time_ns, macro_frozen, near_circuit);
        if (rejection != risk::RiskRejection::NONE) return;

        core::Order new_order{};
        new_order.client_order_id = harv_next_order_id++;
        new_order.token = sig.token;
        core::copy_fixed_str(new_order.symbol, sig.symbol);
        new_order.side = sig.side;
        new_order.type = core::OrderType::MARKET;
        new_order.price = sig.target_entry;
        new_order.quantity = sized_qty;
        new_order.created_time_ns = current_time_ns;

        if (harv_broker.place_order(new_order)) {
            harv_lifecycle = PositionLifecycle::PENDING_ENTRY;
            harv_pending_signal = sig;
            harv_risk_mgr.set_has_active_position(true);
        }
    };

    // 5c. Initialize Parallel Track C (Bi-Directional Omni: Dual-Slot 1-Long + 1-Short Book per Moskowitz et al. 2012)
    core::RiskConfig omni_cfg = risk_cfg;
    omni_cfg.morning_start_sec = 9 * 3600 + 35 * 60;
    omni_cfg.morning_end_sec = 14 * 3600 + 45 * 60;
    omni_cfg.afternoon_start_sec = 13 * 3600 + 30 * 60;
    omni_cfg.afternoon_end_sec = 14 * 3600 + 45 * 60;
    omni_cfg.breakeven_trigger_pct = 0.90;
    omni_cfg.enable_ofi_harvest = false;

    risk::RiskManager omni_risk_mgr(omni_cfg);
    core::RiskConfig omni_short_cfg = omni_cfg;
    omni_short_cfg.breakeven_trigger_pct = 0.45;
    risk::RiskManager omni_short_risk_mgr(omni_short_cfg);
    const double omni_initial_cap = (omni_capital_override > 0.0) ? omni_capital_override : risk_cfg.initial_capital_inr;
    double omni_shared_balance = omni_initial_cap;
    std::vector<core::TradeAuditEntry> omni_combined_trades;
    execution::PaperBroker omni_broker(omni_initial_cap);
    execution::PaperBroker omni_short_broker(omni_initial_cap);
    execution::TradeAuditor omni_auditor(omni_initial_cap);
    strategy::MomentumBreakoutStrategy omni_strat(
        2.5, risk_cfg.min_composite_ofi, whitelist, 5, risk_cfg.circuit_limit_buffer_pct,
        risk_cfg.take_profit_pct, risk_cfg.stop_loss_pct, risk_cfg.min_bid_depth_ratio,
        strategy::StrategyMode::OMNI);
    core::Position omni_position{};
    core::Position omni_short_position{};
    core::TradingSignal omni_pending_signal{};
    core::TradingSignal omni_short_pending_signal{};
    PositionLifecycle omni_lifecycle = PositionLifecycle::FLAT;
    PositionLifecycle omni_short_lifecycle = PositionLifecycle::FLAT;
    uint64_t omni_next_order_id = 2000001;

    omni_broker.register_fill_callback([&](const core::Order& order, const core::TradeAuditEntry& audit) {
        const core::Price tick_step = tick_sizes_paise.count(order.token) ? tick_sizes_paise[order.token] : 5;
        if (audit.quantity == 0) {
            omni_lifecycle = PositionLifecycle::OPEN;
            omni_position = core::Position{};
            omni_position.token = order.token;
            core::copy_fixed_str(omni_position.symbol, order.symbol);
            omni_position.side = order.side;
            omni_position.quantity = order.filled_quantity;
            omni_position.avg_entry_price = order.avg_fill_price;
            omni_position.current_price = order.avg_fill_price;
            omni_position.peak_favorable_price = order.avg_fill_price;
            omni_position.worst_adverse_price = order.avg_fill_price;
            if (omni_pending_signal.is_valid && omni_pending_signal.target_entry > 0) {
                const core::Price sl_dist = omni_pending_signal.target_entry - omni_pending_signal.stop_loss;
                const core::Price tp_dist = omni_pending_signal.take_profit - omni_pending_signal.target_entry;
                omni_position.current_stop_loss = core::floor_to_tick(order.avg_fill_price - sl_dist, tick_step);
                omni_position.take_profit = core::ceil_to_tick(order.avg_fill_price + tp_dist, tick_step);
            } else {
                omni_position.current_stop_loss = core::floor_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 0.9915), tick_step);
                omni_position.take_profit = core::ceil_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 1.0175), tick_step);
            }
            omni_position.initial_stop_loss = omni_position.current_stop_loss;
            omni_position.is_breakeven_set = false;
            omni_position.open_time_ns = order.updated_time_ns;
            omni_risk_mgr.set_has_active_position(true);
            if (live_tail || emit_events) {
                std::ostringstream ev_ss;
                ev_ss << "\n[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"OMNI\",\"ts_ns\":" << order.updated_time_ns << ",\"side\":\"BUY"
                      << "\",\"symbol\":\"" << order.symbol << "\",\"qty\":" << order.filled_quantity
                      << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(order.avg_fill_price)
                      << ",\"sl\":" << core::to_inr(omni_position.current_stop_loss)
                      << ",\"tp\":" << core::to_inr(omni_position.take_profit)
                      << ",\"balance\":" << omni_shared_balance << "}\n";
                std::cout << ev_ss.str() << std::flush;
            }
        } else {
            omni_lifecycle = PositionLifecycle::FLAT;
            omni_pending_signal = core::TradingSignal{};
            omni_risk_mgr.on_trade_closed(audit);
            omni_strat.on_trade_fill(audit);
            omni_shared_balance += audit.net_pnl_inr;
            omni_combined_trades.push_back(audit);
            if (live_tail || emit_events) {
                const double total_taxes = audit.stt_inr + audit.exchange_fee_inr + audit.gst_inr +
                                           audit.stamp_duty_inr + audit.sebi_turnover_inr;
                std::ostringstream ev_ss;
                ev_ss << "\n[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"OMNI\",\"ts_ns\":" << audit.exit_time_ns << ",\"side\":\"SELL"
                      << "\",\"symbol\":\"" << order.symbol << "\",\"qty\":" << audit.quantity
                      << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(audit.exit_price)
                      << ",\"gross_pnl\":" << audit.gross_pnl_inr
                      << ",\"taxes\":" << total_taxes
                      << ",\"net_pnl\":" << audit.net_pnl_inr
                      << ",\"balance\":" << omni_shared_balance << "}\n";
                std::cout << ev_ss.str() << std::flush;
            }
        }
    });

    omni_short_broker.register_fill_callback([&](const core::Order& order, const core::TradeAuditEntry& audit) {
        const core::Price tick_step = tick_sizes_paise.count(order.token) ? tick_sizes_paise[order.token] : 5;
        if (audit.quantity == 0) {
            omni_short_lifecycle = PositionLifecycle::OPEN;
            omni_short_position = core::Position{};
            omni_short_position.token = order.token;
            core::copy_fixed_str(omni_short_position.symbol, order.symbol);
            omni_short_position.side = order.side;
            omni_short_position.quantity = order.filled_quantity;
            omni_short_position.avg_entry_price = order.avg_fill_price;
            omni_short_position.current_price = order.avg_fill_price;
            omni_short_position.peak_favorable_price = order.avg_fill_price;
            omni_short_position.worst_adverse_price = order.avg_fill_price;
            if (omni_short_pending_signal.is_valid && omni_short_pending_signal.target_entry > 0) {
                const core::Price sl_dist = omni_short_pending_signal.stop_loss - omni_short_pending_signal.target_entry;
                const core::Price tp_dist = omni_short_pending_signal.target_entry - omni_short_pending_signal.take_profit;
                omni_short_position.current_stop_loss = core::ceil_to_tick(order.avg_fill_price + sl_dist, tick_step);
                omni_short_position.take_profit = core::floor_to_tick(order.avg_fill_price - tp_dist, tick_step);
            } else {
                omni_short_position.current_stop_loss = core::ceil_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 1.0085), tick_step);
                omni_short_position.take_profit = core::floor_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 0.9825), tick_step);
            }
            omni_short_position.initial_stop_loss = omni_short_position.current_stop_loss;
            omni_short_position.is_breakeven_set = false;
            omni_short_position.open_time_ns = order.updated_time_ns;
            omni_short_risk_mgr.set_has_active_position(true);
            if (live_tail || emit_events) {
                std::ostringstream ev_ss;
                ev_ss << "\n[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"OMNI\",\"ts_ns\":" << order.updated_time_ns << ",\"side\":\"SHORT"
                      << "\",\"symbol\":\"" << order.symbol << "\",\"qty\":" << order.filled_quantity
                      << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(order.avg_fill_price)
                      << ",\"sl\":" << core::to_inr(omni_short_position.current_stop_loss)
                      << ",\"tp\":" << core::to_inr(omni_short_position.take_profit)
                      << ",\"balance\":" << omni_shared_balance << "}\n";
                std::cout << ev_ss.str() << std::flush;
            }
        } else {
            omni_short_lifecycle = PositionLifecycle::FLAT;
            omni_short_pending_signal = core::TradingSignal{};
            omni_short_risk_mgr.on_trade_closed(audit);
            omni_strat.on_trade_fill(audit);
            omni_shared_balance += audit.net_pnl_inr;
            omni_combined_trades.push_back(audit);
            if (live_tail || emit_events) {
                const double total_taxes = audit.stt_inr + audit.exchange_fee_inr + audit.gst_inr +
                                           audit.stamp_duty_inr + audit.sebi_turnover_inr;
                std::ostringstream ev_ss;
                ev_ss << "\n[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"OMNI\",\"ts_ns\":" << audit.exit_time_ns << ",\"side\":\"COVER"
                      << "\",\"symbol\":\"" << order.symbol << "\",\"qty\":" << audit.quantity
                      << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(audit.exit_price)
                      << ",\"gross_pnl\":" << audit.gross_pnl_inr
                      << ",\"taxes\":" << total_taxes
                      << ",\"net_pnl\":" << audit.net_pnl_inr
                      << ",\"balance\":" << omni_shared_balance << "}\n";
                std::cout << ev_ss.str() << std::flush;
            }
        }
    });

    auto try_submit_omni_signal = [&](const core::TradingSignal& sig,
                                      const market::OrderBook& book,
                                      uint64_t current_time_ns) {
        const bool macro_frozen = macro_calendar.is_frozen(current_time_ns);
        const bool near_circuit = book.is_near_circuit(omni_cfg.circuit_limit_buffer_pct);
        if (sig.side == core::Side::BUY) {
            if (omni_lifecycle != PositionLifecycle::FLAT) return;
            const auto [sized_qty, rejection] =
                omni_risk_mgr.evaluate_and_size_order(sig, current_time_ns, macro_frozen, near_circuit);
            if (rejection != risk::RiskRejection::NONE) return;
            core::Order new_order{};
            new_order.client_order_id = omni_next_order_id++;
            new_order.token = sig.token;
            core::copy_fixed_str(new_order.symbol, sig.symbol);
            new_order.side = sig.side;
            new_order.type = core::OrderType::MARKET;
            new_order.price = sig.target_entry;
            new_order.quantity = sized_qty;
            new_order.created_time_ns = current_time_ns;
            if (omni_broker.place_order(new_order)) {
                omni_lifecycle = PositionLifecycle::PENDING_ENTRY;
                omni_pending_signal = sig;
                omni_risk_mgr.set_has_active_position(true);
            }
        } else {
            if (omni_short_lifecycle != PositionLifecycle::FLAT) return;
            const auto [sized_qty, rejection] =
                omni_short_risk_mgr.evaluate_and_size_order(sig, current_time_ns, macro_frozen, near_circuit);
            if (rejection != risk::RiskRejection::NONE) return;
            core::Order new_order{};
            new_order.client_order_id = omni_next_order_id++;
            new_order.token = sig.token;
            core::copy_fixed_str(new_order.symbol, sig.symbol);
            new_order.side = sig.side;
            new_order.type = core::OrderType::MARKET;
            new_order.price = sig.target_entry;
            new_order.quantity = sized_qty;
            new_order.created_time_ns = current_time_ns;
            if (omni_short_broker.place_order(new_order)) {
                omni_short_lifecycle = PositionLifecycle::PENDING_ENTRY;
                omni_short_pending_signal = sig;
                omni_short_risk_mgr.set_has_active_position(true);
            }
        }
    };

    // 6. Register Execution Gateway Fill Callback
    broker.register_fill_callback([&](const core::Order& order, const core::TradeAuditEntry& audit) {
        const core::Price tick_step = tick_sizes_paise.count(order.token) ? tick_sizes_paise[order.token] : 5;

        if (order.side == core::Side::BUY) {
            lifecycle = PositionLifecycle::OPEN;
            current_position = core::Position{};
            current_position.token = order.token;
            core::copy_fixed_str(current_position.symbol, order.symbol);
            current_position.side = core::Side::BUY;
            current_position.quantity = order.filled_quantity;
            current_position.avg_entry_price = order.avg_fill_price;
            current_position.current_price = order.avg_fill_price;
            current_position.peak_favorable_price = order.avg_fill_price;
            current_position.worst_adverse_price = order.avg_fill_price;

            // Preserve signal SL/TP distances shifted by execution slippage and aligned to NSE tick size
            if (pending_entry_signal.is_valid && pending_entry_signal.target_entry > 0) {
                const core::Price sl_dist = pending_entry_signal.target_entry - pending_entry_signal.stop_loss;
                const core::Price tp_dist = pending_entry_signal.take_profit - pending_entry_signal.target_entry;
                current_position.current_stop_loss =
                    core::floor_to_tick(order.avg_fill_price - sl_dist, tick_step);
                current_position.take_profit =
                    core::ceil_to_tick(order.avg_fill_price + tp_dist, tick_step);
            } else {
                current_position.current_stop_loss = core::floor_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 0.990), tick_step);
                current_position.take_profit = core::ceil_to_tick(
                    static_cast<core::Price>(static_cast<double>(order.avg_fill_price) * 1.018), tick_step);
            }
            current_position.initial_stop_loss = current_position.current_stop_loss;
            current_position.is_breakeven_set = false;
            current_position.open_time_ns = order.updated_time_ns;

            active_risk_mgr.set_has_active_position(true);
            watchdog.set_has_open_position(true);
            journal.save_position(current_position);

            std::ostringstream ss;
            ss << "FILLED BUY: " << order.symbol << " Qty=" << order.filled_quantity
               << " @ " << std::fixed << std::setprecision(2) << core::to_inr(order.avg_fill_price) << " INR"
               << " | SL=" << core::to_inr(current_position.current_stop_loss)
               << " | TP=" << core::to_inr(current_position.take_profit);
            LOG_TRADE(ss.str());
            if (live_tail) {
                const char* trk = primary_is_harvester ? "HARVESTER" : "COMPOUNDER";
                std::cout << "[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"" << trk
                          << "\",\"side\":\"BUY\",\"symbol\":\"" << order.symbol
                          << "\",\"qty\":" << order.filled_quantity
                          << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(order.avg_fill_price)
                          << ",\"sl\":" << core::to_inr(current_position.current_stop_loss)
                          << ",\"tp\":" << core::to_inr(current_position.take_profit)
                          << ",\"balance\":" << broker.cash_balance_inr() << "}\n" << std::flush;
            }
        } else if (order.side == core::Side::SELL) {
            lifecycle = PositionLifecycle::FLAT;
            pending_entry_signal = core::TradingSignal{};
            active_risk_mgr.on_trade_closed(audit);
            momentum_strat.on_trade_fill(audit);
            watchdog.set_has_open_position(false);
            journal.clear_position();

            const double total_taxes = audit.stt_inr + audit.exchange_fee_inr + audit.gst_inr +
                                       audit.stamp_duty_inr + audit.sebi_turnover_inr;

            std::ostringstream ss;
            ss << "FILLED SELL: " << order.symbol << " Qty=" << audit.quantity
               << " @ " << std::fixed << std::setprecision(2) << core::to_inr(audit.exit_price) << " INR"
               << " | Gross PnL: " << (audit.gross_pnl_inr >= 0.0 ? "+" : "") << audit.gross_pnl_inr << " INR"
               << " | Taxes: -" << total_taxes << " INR"
               << " | Net PnL: " << (audit.net_pnl_inr >= 0.0 ? "+" : "") << audit.net_pnl_inr << " INR"
               << " | MAE: " << core::to_inr(audit.max_adverse_excursion) << " INR"
               << " | MFE: " << core::to_inr(audit.max_favorable_excursion) << " INR"
               << " | Balance: " << broker.cash_balance_inr() << " INR";
            LOG_TRADE(ss.str());
            if (live_tail) {
                const char* trk = primary_is_harvester ? "HARVESTER" : "COMPOUNDER";
                std::cout << "[LIVE_EVENT] {\"type\":\"FILL\",\"track\":\"" << trk
                          << "\",\"side\":\"SELL\",\"symbol\":\"" << order.symbol
                          << "\",\"qty\":" << audit.quantity
                          << ",\"price\":" << std::fixed << std::setprecision(2) << core::to_inr(audit.exit_price)
                          << ",\"gross_pnl\":" << audit.gross_pnl_inr
                          << ",\"taxes\":" << total_taxes
                          << ",\"net_pnl\":" << audit.net_pnl_inr
                          << ",\"balance\":" << broker.cash_balance_inr() << "}\n" << std::flush;
            }
        }
    });

    // Helper to evaluate and submit a validated TradingSignal through the 13-Shield Risk Manager
    auto try_submit_signal = [&](const core::TradingSignal& sig,
                                 const market::OrderBook& book,
                                 uint64_t current_time_ns) {
        if (lifecycle != PositionLifecycle::FLAT) return;

        const bool macro_frozen = macro_calendar.is_frozen(current_time_ns);
        const bool near_circuit = book.is_near_circuit(risk_cfg.circuit_limit_buffer_pct);

        const auto [sized_qty, rejection] =
            active_risk_mgr.evaluate_and_size_order(sig, current_time_ns, macro_frozen, near_circuit);
        if (rejection != risk::RiskRejection::NONE) {
            LOG_DEBUG(std::string("Order rejected by RiskManager: ") + risk::risk_rejection_to_string(rejection));
            return;
        }

        core::Order new_order{};
        new_order.client_order_id = next_order_id++;
        new_order.token = sig.token;
        core::copy_fixed_str(new_order.symbol, sig.symbol);
        new_order.side = sig.side;
        new_order.type = core::OrderType::MARKET;
        new_order.price = sig.target_entry;
        new_order.quantity = sized_qty;
        new_order.created_time_ns = current_time_ns;

        if (broker.place_order(new_order)) {
            lifecycle = PositionLifecycle::PENDING_ENTRY;
            pending_entry_signal = sig;
            active_risk_mgr.set_has_active_position(true);

            std::ostringstream ss;
            ss << "SIGNAL ACCEPTED ("
               << (sig.source == core::SignalSource::EXTERNAL_WEBHOOK ? "WEBHOOK" : "MOMENTUM")
               << "): " << sig.symbol << " BUY Qty=" << sized_qty
               << " @ Target=" << std::fixed << std::setprecision(2) << core::to_inr(sig.target_entry) << " INR"
               << " | OFI=" << book.composite_ofi();
            LOG_INFO(ss.str());
        }
    };

    // 7. Lock-Free SPSC Ingestion Queues (Market Feed Producer -> Hot Engine Consumer)
    auto tick_ring = std::make_unique<core::SPSCRingBuffer<core::MarketTick, 4096>>();
    auto alert_ring = std::make_unique<core::SPSCRingBuffer<core::ExternalAlert, 256>>();
    std::atomic<bool> producer_done{false};
    std::atomic<bool> feed_opened{true};

    if (!cli_webhook_json.empty()) {
        if (auto parsed = strategy::SignalIngestor::parse_webhook_json(cli_webhook_json)) {
            if (parsed->token == 0 && symbol_to_token.count(parsed->symbol)) {
                parsed->token = symbol_to_token[parsed->symbol];
            }
            (void)alert_ring->push(*parsed);
        } else {
            LOG_WARN("Failed to parse --webhook JSON payload; ignoring external alert.");
        }
    }

    market::CSVFeed feed(csv_path, realtime_pacing, 5, live_tail);
    g_active_feed_ptr = &feed;
    LOG_INFO(std::string("Connecting to CSV Market Feed: ") + csv_path + (live_tail ? " [LIVE-TAIL MODE]" : ""));

    feed.register_callback([&](const core::MarketTick& tick) {
        while (!tick_ring->push(tick)) {
            std::this_thread::yield();
        }
    });
    feed.register_alert_callback([&](const core::ExternalAlert& alert) {
        while (!alert_ring->push(alert)) {
            std::this_thread::yield();
        }
    });

    watchdog.start_monitoring();
    LOG_INFO("Starting Market Feed & Lock-Free SPSC Pipeline...");

    std::thread producer_thread([&]() {
        if (!feed.start()) {
            feed_opened.store(false, std::memory_order_release);
        }
        producer_done.store(true, std::memory_order_release);
    });

    // 8. Core Single-Threaded Event Loop (Zero Locks on Hot Trading Path)
    int64_t current_ist_day = -1;

    while (!producer_done.load(std::memory_order_acquire) || !tick_ring->empty() || !alert_ring->empty()) {
        bool did_work = false;

        if (auto tick_opt = tick_ring->pop()) {
            did_work = true;
            const core::MarketTick& tick = *tick_opt;

            // --- SESSION DAY BOUNDARY ROLLOVER COORDINATION (09:15 IST) ---
            if (tick.timestamp_ns > 0) {
                const int64_t tick_ist_day =
                    static_cast<int64_t>((tick.timestamp_ns / 1'000'000'000ULL + 19800ULL) / 86400ULL);
                if (current_ist_day != -1 && tick_ist_day != current_ist_day) {
                    std::ostringstream roll_msg;
                    roll_msg << "=== SESSION ROLLOVER: Transitioning to IST Day " << tick_ist_day
                             << " | Carried Portfolio Equity: " << std::fixed << std::setprecision(2)
                             << broker.cash_balance_inr() << " INR ===";
                    LOG_INFO(roll_msg.str());

                    // 1. Invariant safety: Assert flat lifecycle and clear any orphaned state
                    if (lifecycle != PositionLifecycle::FLAT) {
                        LOG_WARN("OVERNIGHT INVARIANT VIOLATION: Position active across session rollover! Forcing FLAT.");
                        lifecycle = PositionLifecycle::FLAT;
                        current_position = core::Position{};
                        pending_entry_signal = core::TradingSignal{};
                        journal.clear_position();
                    }

                    // 2. Coordinated Rollover Dispatch
                    active_risk_mgr.check_day_rollover(tick.timestamp_ns);
                    trade_auditor.on_session_start(tick_ist_day, execution::TradeAuditor::format_ist_date(tick.timestamp_ns));
                    momentum_strat.reset_daily_session();
                    if (enable_parallel_harvester) {
                        harv_lifecycle = PositionLifecycle::FLAT;
                        harv_position = core::Position{};
                        harv_pending_signal = core::TradingSignal{};
                        harv_risk_mgr.check_day_rollover(tick.timestamp_ns);
                        harv_auditor.on_session_start(tick_ist_day, execution::TradeAuditor::format_ist_date(tick.timestamp_ns));
                        harv_strat.reset_daily_session();

                        omni_lifecycle = PositionLifecycle::FLAT;
                        omni_position = core::Position{};
                        omni_pending_signal = core::TradingSignal{};
                        omni_risk_mgr.check_day_rollover(tick.timestamp_ns);
                        omni_short_lifecycle = PositionLifecycle::FLAT;
                        omni_short_position = core::Position{};
                        omni_short_pending_signal = core::TradingSignal{};
                        omni_short_risk_mgr.check_day_rollover(tick.timestamp_ns);
                        omni_auditor.on_session_start(tick_ist_day, execution::TradeAuditor::format_ist_date(tick.timestamp_ns));
                        omni_strat.reset_daily_session();
                    }
                    for (auto& [tok, book_inst] : books) {
                        book_inst.reset_daily_session();
                    }
                    watchdog.set_has_open_position(false);
                } else if (current_ist_day == -1) {
                    active_risk_mgr.check_day_rollover(tick.timestamp_ns);
                    trade_auditor.on_session_start(tick_ist_day, execution::TradeAuditor::format_ist_date(tick.timestamp_ns));
                    if (enable_parallel_harvester) {
                        harv_risk_mgr.check_day_rollover(tick.timestamp_ns);
                        harv_auditor.on_session_start(tick_ist_day, execution::TradeAuditor::format_ist_date(tick.timestamp_ns));
                        omni_risk_mgr.check_day_rollover(tick.timestamp_ns);
                        omni_short_lifecycle = PositionLifecycle::FLAT;
                        omni_short_position = core::Position{};
                        omni_short_pending_signal = core::TradingSignal{};
                        omni_short_risk_mgr.check_day_rollover(tick.timestamp_ns);
                        omni_auditor.on_session_start(tick_ist_day, execution::TradeAuditor::format_ist_date(tick.timestamp_ns));
                    }
                }
                current_ist_day = tick_ist_day;
            }
            // --- END SESSION ROLLOVER ---

            watchdog.on_tick(tick.timestamp_ns);

            auto [book_it, inserted] = books.try_emplace(tick.token, tick.token, std::string(tick.symbol));
            market::OrderBook& book = book_it->second;

            // A. Update L2 Order Book & Multi-Horizon OFI
            book.on_tick(tick);

            // B. Match pending orders strictly on Tick[t+1] (Zero Look-Ahead Bias)
            broker.on_next_tick(tick, book);
            if (enable_parallel_harvester) {
                harv_broker.on_next_tick(tick, book);
                omni_broker.on_next_tick(tick, book);
                omni_short_broker.on_next_tick(tick, book);
            }

            // C. Always advance strategy rolling volume & VWAP state (even while holding a position!)
            momentum_strat.on_tick(tick, book);
            if (enable_parallel_harvester) {
                harv_strat.on_tick(tick, book);
                omni_strat.on_tick(tick, book);
            }

            // D. Drain any queued external trader webhook alerts once local order book is live
            while (auto alert_opt = alert_ring->pop()) {
                const core::ExternalAlert& alert = *alert_opt;
                const uint32_t tok = (alert.token != 0) ? alert.token : symbol_to_token[alert.symbol];
                auto b_it = books.find(tok);
                if (b_it == books.end() || b_it->second.best_ask() <= 0) {
                    LOG_WARN(std::string("WEBHOOK VETOED [") + alert.symbol + "]: No live L2 book depth");
                    continue;
                }
                const auto [ext_sig, veto] = bs_detector.validate_external_alert(
                    alert, b_it->second, momentum_strat.latest_volume_multiple(b_it->second.token()));
                if (veto != strategy::VetoReason::NONE || !ext_sig.has_value()) {
                    LOG_WARN(std::string("WEBHOOK VETOED [") + alert.symbol + "]: " +
                             strategy::veto_reason_to_string(veto));
                } else {
                    try_submit_signal(*ext_sig, b_it->second, tick.timestamp_ns);
                    if (enable_parallel_harvester) {
                        try_submit_harv_signal(*ext_sig, b_it->second, tick.timestamp_ns);
                        try_submit_omni_signal(*ext_sig, b_it->second, tick.timestamp_ns);
                    }
                }
            }

            // D2. Manage Parallel Track B (Harvester) Open Position & Signal Generation
            if (enable_parallel_harvester) {
                if (harv_lifecycle == PositionLifecycle::OPEN && tick.token == harv_position.token) {
                    const core::Price tick_step = tick_sizes_paise.count(tick.token) ? tick_sizes_paise[tick.token] : 5;
                    const auto [h_should_exit, h_exit_reason] =
                        harv_risk_mgr.check_position_rules(harv_position, tick.last_price, tick.timestamp_ns, tick_step, &book);
                    if (h_should_exit) {
                        core::Order exit_order{};
                        exit_order.client_order_id = harv_next_order_id++;
                        exit_order.token = harv_position.token;
                        core::copy_fixed_str(exit_order.symbol, harv_position.symbol);
                        exit_order.side = core::Side::SELL;
                        exit_order.type = core::OrderType::MARKET;
                        exit_order.quantity = harv_position.quantity;
                        exit_order.created_time_ns = tick.timestamp_ns;
                        if (harv_broker.place_order(exit_order)) {
                            harv_lifecycle = PositionLifecycle::PENDING_EXIT;
                        }
                    }
                } else if (harv_lifecycle == PositionLifecycle::FLAT) {
                    if (const auto h_sig = harv_strat.generate_signal(book);
                        h_sig.has_value() && h_sig->is_valid) {
                        try_submit_harv_signal(*h_sig, book, tick.timestamp_ns);
                    }
                }

                // D3. Manage Parallel Track C (Bi-Directional Omni: Independent Long + Short Slots)
                if (omni_lifecycle == PositionLifecycle::OPEN && tick.token == omni_position.token) {
                    const core::Price tick_step = tick_sizes_paise.count(tick.token) ? tick_sizes_paise[tick.token] : 5;
                    const auto [o_should_exit, o_exit_reason] =
                        omni_risk_mgr.check_position_rules(omni_position, tick.last_price, tick.timestamp_ns, tick_step, &book);
                    if (o_should_exit) {
                        core::Order exit_order{};
                        exit_order.client_order_id = omni_next_order_id++;
                        exit_order.token = omni_position.token;
                        core::copy_fixed_str(exit_order.symbol, omni_position.symbol);
                        exit_order.side = core::Side::SELL;
                        exit_order.type = core::OrderType::MARKET;
                        exit_order.quantity = omni_position.quantity;
                        exit_order.created_time_ns = tick.timestamp_ns;
                        if (omni_broker.place_order(exit_order)) {
                            omni_lifecycle = PositionLifecycle::PENDING_EXIT;
                        }
                    }
                }
                if (omni_short_lifecycle == PositionLifecycle::OPEN && tick.token == omni_short_position.token) {
                    const core::Price tick_step = tick_sizes_paise.count(tick.token) ? tick_sizes_paise[tick.token] : 5;
                    const auto [os_should_exit, os_exit_reason] =
                        omni_short_risk_mgr.check_position_rules(omni_short_position, tick.last_price, tick.timestamp_ns, tick_step, &book);
                    if (os_should_exit) {
                        core::Order exit_order{};
                        exit_order.client_order_id = omni_next_order_id++;
                        exit_order.token = omni_short_position.token;
                        core::copy_fixed_str(exit_order.symbol, omni_short_position.symbol);
                        exit_order.side = core::Side::BUY;
                        exit_order.type = core::OrderType::MARKET;
                        exit_order.quantity = omni_short_position.quantity;
                        exit_order.created_time_ns = tick.timestamp_ns;
                        if (omni_short_broker.place_order(exit_order)) {
                            omni_short_lifecycle = PositionLifecycle::PENDING_EXIT;
                        }
                    }
                }
                if (omni_lifecycle == PositionLifecycle::FLAT || omni_short_lifecycle == PositionLifecycle::FLAT) {
                    const uint32_t busy_tok = (omni_lifecycle != PositionLifecycle::FLAT) ? omni_position.token
                                            : ((omni_short_lifecycle != PositionLifecycle::FLAT) ? omni_short_position.token : 0);
                    omni_strat.set_omni_slot_availability(
                        omni_lifecycle == PositionLifecycle::FLAT,
                        omni_short_lifecycle == PositionLifecycle::FLAT,
                        busy_tok);
                    if (const auto o_sig = omni_strat.generate_signal(book);
                        o_sig.has_value() && o_sig->is_valid) {
                        try_submit_omni_signal(*o_sig, book, tick.timestamp_ns);
                    }
                }
            }

            // E. Manage open position (Break-Even Trailing Stop, SL/TP, 15:05 IST Auto-Squareoff Shield)
            if (lifecycle == PositionLifecycle::OPEN && tick.token == current_position.token) {
                const bool was_be = current_position.is_breakeven_set;
                const core::Price tick_step = tick_sizes_paise.count(tick.token) ? tick_sizes_paise[tick.token] : 5;
                const auto [should_exit, exit_reason] =
                    active_risk_mgr.check_position_rules(current_position, tick.last_price, tick.timestamp_ns, tick_step, &book);

                if (!was_be && current_position.is_breakeven_set) {
                    journal.save_position(current_position);
                    std::ostringstream be_msg;
                    be_msg << "BREAKEVEN RATCHETED: " << current_position.symbol
                           << " SL moved to " << std::fixed << std::setprecision(2)
                           << core::to_inr(current_position.current_stop_loss) << " INR (Zero-Loss locked)";
                    LOG_INFO(be_msg.str());
                }

                if (should_exit) {
                    std::ostringstream ex_msg;
                    ex_msg << "Position Exit Triggered: " << exit_reason
                           << " @ " << std::fixed << std::setprecision(2)
                           << core::to_inr(tick.last_price) << " INR";
                    LOG_INFO(ex_msg.str());

                    core::Order exit_order{};
                    exit_order.client_order_id = next_order_id++;
                    exit_order.token = current_position.token;
                    core::copy_fixed_str(exit_order.symbol, current_position.symbol);
                    exit_order.side = core::Side::SELL;
                    exit_order.type = core::OrderType::MARKET;
                    exit_order.quantity = current_position.quantity;
                    exit_order.created_time_ns = tick.timestamp_ns;

                    if (broker.place_order(exit_order)) {
                        lifecycle = PositionLifecycle::PENDING_EXIT;
                    }
                }
                continue;
            }

            // F. Evaluate internal Momentum Breakout Strategy when flat
            if (lifecycle == PositionLifecycle::FLAT) {
                if (const auto sig_opt = momentum_strat.generate_signal(book);
                    sig_opt.has_value() && sig_opt->is_valid) {
                    try_submit_signal(*sig_opt, book, tick.timestamp_ns);
                }
            }
        }

        if (!did_work) {
            std::this_thread::yield();
        }
    }

    if (producer_thread.joinable()) {
        producer_thread.join();
    }
    watchdog.stop_monitoring();

    if (!feed_opened.load(std::memory_order_acquire)) {
        LOG_ERROR("Failed to open CSV market feed: " + csv_path);
        core::AsyncLogger::instance().shutdown();
        return 1;
    }

    LOG_INFO("Feed replay complete.");
    core::AsyncLogger::instance().flush();

    // 9. Post-Trade Performance Audit Report
    active_risk_mgr.finalize_session();
    const auto multi_day_report =
        trade_auditor.generate_report(broker.trade_history(), active_risk_mgr.kill_switch_history());
    std::stringstream report_buf;
    execution::TradeAuditor::print_multi_day_report(multi_day_report, report_buf);
    if (enable_parallel_harvester) {
        harv_risk_mgr.finalize_session();
        const auto harv_report =
            harv_auditor.generate_report(harv_broker.trade_history(), harv_risk_mgr.kill_switch_history());
        execution::TradeAuditor::print_dual_mode_comparison(multi_day_report, harv_report, report_buf);

        omni_risk_mgr.finalize_session();
        const auto omni_report =
            omni_auditor.generate_report(omni_combined_trades, omni_risk_mgr.kill_switch_history());
        report_buf << "\n============================================================================================================================\n"
                   << "               TRACK C (BI-DIRECTIONAL OMNI: LONG + SHORT + FULL-DAY) SUMMARY                                               \n"
                   << "============================================================================================================================\n"
                   << "  Initial Capital: INR " << std::fixed << std::setprecision(2) << omni_report.aggregate.initial_capital_inr
                   << " | Ending Equity: INR " << omni_report.aggregate.ending_capital_inr
                   << " | Net PnL: " << (omni_report.aggregate.net_pnl_inr >= 0 ? "+" : "") << omni_report.aggregate.net_pnl_inr << " INR"
                   << " (" << (omni_report.aggregate.net_return_pct >= 0 ? "+" : "") << omni_report.aggregate.net_return_pct << "%)"
                   << " | Trades: " << omni_report.aggregate.total_trades
                   << " (WR: " << omni_report.aggregate.win_rate_pct << "%)\n"
                   << "============================================================================================================================\n";
    }
    std::cout << report_buf.str();
    std::string report_line;
    while (std::getline(report_buf, report_line)) {
        LOG_INFO(report_line);
    }

    LOG_INFO("Engine shutdown cleanly. DeltaUno session concluded.");
    core::AsyncLogger::instance().shutdown();

    return 0;
}
