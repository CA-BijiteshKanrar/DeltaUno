#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/ring_buffer.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/execution/paper_broker.hpp"
#include "deltauno/market/csv_feed.hpp"
#include "deltauno/market/order_book.hpp"
#include "deltauno/market/watchdog.hpp"
#include "deltauno/risk/risk_manager.hpp"
#include "deltauno/strategy/momentum_breakout.hpp"
#include "deltauno/strategy/signal_ingestor.hpp"
#include "test_macros.hpp"

#include <atomic>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace deltauno;

namespace {

enum class PositionLifecycle : uint8_t {
    FLAT = 0,
    PENDING_ENTRY = 1,
    OPEN = 2,
    PENDING_EXIT = 3
};

std::string format_ist_hms(uint64_t time_ns) {
    const uint64_t sec_ist = (time_ns / 1'000'000'000ULL) + 19800ULL;
    const uint64_t h = (sec_ist / 3600ULL) % 24ULL;
    const uint64_t m = (sec_ist / 60ULL) % 60ULL;
    const uint64_t s = sec_ist % 60ULL;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", h, m, s);
    return std::string(buf);
}

std::string format_ist_date(uint64_t time_ns) {
    const time_t sec = static_cast<time_t>((time_ns / 1'000'000'000ULL) + 19800ULL);
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

} // namespace

int main() {
    std::cout << "====================================================================================\n";
    std::cout << "  EMPIRICAL CHALLENGER: 30-DAY HISTORICAL REPLAY TRADE TIMING & AUCTION AUDIT       \n";
    std::cout << "====================================================================================\n";

    const std::string csv_path = "data/historical_30days_nse.csv";
    const std::string config_dir = "config";

    const core::RiskConfig risk_cfg = core::ConfigLoader::load_risk_config(config_dir + "/risk_limits.json");
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

    risk::RiskManager risk_mgr(risk_cfg);
    execution::PaperBroker broker(risk_cfg.initial_capital_inr);
    market::Watchdog watchdog(risk_cfg.heartbeat_timeout_seconds);

    strategy::MomentumBreakoutStrategy momentum_strat(
        2.5, 0.55, whitelist, 5, risk_cfg.circuit_limit_buffer_pct);

    core::Position current_position{};
    core::TradingSignal pending_entry_signal{};
    PositionLifecycle lifecycle = PositionLifecycle::FLAT;
    uint64_t next_order_id = 1;

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

            if (pending_entry_signal.is_valid && pending_entry_signal.target_entry > 0) {
                const core::Price sl_dist = pending_entry_signal.target_entry - pending_entry_signal.stop_loss;
                const core::Price tp_dist = pending_entry_signal.take_profit - pending_entry_signal.target_entry;
                current_position.current_stop_loss =
                    core::floor_to_tick(order.avg_fill_price - sl_dist, tick_step);
                current_position.take_profit =
                    core::ceil_to_tick(order.avg_fill_price + tp_dist, tick_step);
            }
            current_position.initial_stop_loss = current_position.current_stop_loss;
            current_position.is_breakeven_set = false;
            current_position.open_time_ns = order.updated_time_ns;

            risk_mgr.set_has_active_position(true);
            watchdog.set_has_open_position(true);
        } else if (order.side == core::Side::SELL) {
            lifecycle = PositionLifecycle::FLAT;
            pending_entry_signal = core::TradingSignal{};
            risk_mgr.on_trade_closed(audit);
            momentum_strat.on_trade_fill(audit);
            watchdog.set_has_open_position(false);
        }
    });

    auto try_submit_signal = [&](const core::TradingSignal& sig,
                                 const market::OrderBook& book,
                                 uint64_t current_time_ns) {
        if (lifecycle != PositionLifecycle::FLAT) return;

        const bool macro_frozen = macro_calendar.is_frozen(current_time_ns);
        const bool near_circuit = book.is_near_circuit(risk_cfg.circuit_limit_buffer_pct);

        const auto [sized_qty, rejection] =
            risk_mgr.evaluate_and_size_order(sig, current_time_ns, macro_frozen, near_circuit);
        if (rejection != risk::RiskRejection::NONE) {
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
            risk_mgr.set_has_active_position(true);
        }
    };

    strategy::SignalIngestor bs_detector(
        whitelist, 2.0, risk_cfg.max_15s_volume_burst_multiple, 0.20, risk_cfg.circuit_limit_buffer_pct, 5);

    auto tick_ring = std::make_unique<core::SPSCRingBuffer<core::MarketTick, 4096>>();
    auto alert_ring = std::make_unique<core::SPSCRingBuffer<core::ExternalAlert, 256>>();
    std::atomic<bool> producer_done{false};

    market::CSVFeed feed(csv_path, false, 5);
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

    std::thread producer_thread([&]() {
        feed.start();
        producer_done.store(true, std::memory_order_release);
    });

    int64_t current_ist_day = -1;

    while (!producer_done.load(std::memory_order_acquire) || !tick_ring->empty() || !alert_ring->empty()) {
        if (auto tick_opt = tick_ring->pop()) {
            const core::MarketTick& tick = *tick_opt;

            if (tick.timestamp_ns > 0) {
                const int64_t tick_ist_day =
                    static_cast<int64_t>((tick.timestamp_ns / 1'000'000'000ULL + 19800ULL) / 86400ULL);
                if (current_ist_day != -1 && tick_ist_day != current_ist_day) {
                    if (lifecycle != PositionLifecycle::FLAT) {
                        lifecycle = PositionLifecycle::FLAT;
                        current_position = core::Position{};
                        pending_entry_signal = core::TradingSignal{};
                    }
                    risk_mgr.check_day_rollover(tick.timestamp_ns);
                    momentum_strat.reset_daily_session();
                    for (auto& [tok, book_inst] : books) {
                        book_inst.reset_daily_session();
                    }
                    watchdog.set_has_open_position(false);
                } else if (current_ist_day == -1) {
                    risk_mgr.check_day_rollover(tick.timestamp_ns);
                }
                current_ist_day = tick_ist_day;
            }

            watchdog.on_tick(tick.timestamp_ns);

            auto [book_it, inserted] = books.try_emplace(tick.token, tick.token, std::string(tick.symbol));
            market::OrderBook& book = book_it->second;

            book.on_tick(tick);
            broker.on_next_tick(tick, book);
            momentum_strat.on_tick(tick, book);

            while (auto alert_opt = alert_ring->pop()) {
                const core::ExternalAlert& alert = *alert_opt;
                const uint32_t tok = (alert.token != 0) ? alert.token : symbol_to_token[alert.symbol];
                auto b_it = books.find(tok);
                if (b_it == books.end() || b_it->second.best_ask() <= 0) {
                    continue;
                }
                const auto [ext_sig, veto] = bs_detector.validate_external_alert(
                    alert, b_it->second, momentum_strat.latest_volume_multiple(b_it->second.token()));
                if (veto == strategy::VetoReason::NONE && ext_sig.has_value()) {
                    try_submit_signal(*ext_sig, b_it->second, tick.timestamp_ns);
                }
            }

            if (lifecycle == PositionLifecycle::OPEN && tick.token == current_position.token) {
                const core::Price tick_step = tick_sizes_paise.count(tick.token) ? tick_sizes_paise[tick.token] : 5;
                const auto [should_exit, exit_reason] =
                    risk_mgr.check_position_rules(current_position, tick.last_price, tick.timestamp_ns, tick_step);

                if (should_exit) {
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

            if (lifecycle == PositionLifecycle::FLAT) {
                if (const auto sig_opt = momentum_strat.generate_signal(book);
                    sig_opt.has_value() && sig_opt->is_valid) {
                    try_submit_signal(*sig_opt, book, tick.timestamp_ns);
                }
            }
        } else {
            std::this_thread::yield();
        }
    }

    if (producer_thread.joinable()) {
        producer_thread.join();
    }

    const auto& trades = broker.trade_history();
    std::cout << "Replay Complete. Total Completed Trades: " << trades.size() << "\n\n";

    std::cout << "===================================================================================================\n";
    std::cout << " #   Symbol       Date        Entry IST   Exit IST    Qty   Entry (INR) Exit (INR)  Net PnL   Status   \n";
    std::cout << "===================================================================================================\n";

    size_t auction_violations = 0;
    size_t after_squareoff_violations = 0;

    for (size_t i = 0; i < trades.size(); ++i) {
        const auto& t = trades[i];
        const uint64_t entry_sec_ist = ((t.entry_time_ns / 1'000'000'000ULL) + 19800ULL) % 86400ULL;
        const uint64_t exit_sec_ist = ((t.exit_time_ns / 1'000'000'000ULL) + 19800ULL) % 86400ULL;

        // Opening auction window check: 09:15:00 to 09:30:00 IST (33,300 to 34,200 seconds)
        const bool entered_during_auction = (entry_sec_ist >= 33300ULL && entry_sec_ist < 34200ULL);
        if (entered_during_auction) {
            auction_violations++;
        }

        // Auto-squareoff check: all positions must exit by 15:05:15 IST (54,315 seconds)
        const bool exit_after_cutoff = (exit_sec_ist > 54315ULL);
        if (exit_after_cutoff) {
            after_squareoff_violations++;
        }

        const bool pass = !entered_during_auction && !exit_after_cutoff;

        std::cout << std::setw(2) << (i + 1) << "  "
                  << std::left << std::setw(12) << t.symbol << " "
                  << format_ist_date(t.entry_time_ns) << "  "
                  << format_ist_hms(t.entry_time_ns) << "    "
                  << format_ist_hms(t.exit_time_ns) << "    "
                  << std::right << std::setw(4) << t.quantity << "  "
                  << std::fixed << std::setprecision(2) << std::setw(10) << core::to_inr(t.entry_price) << "  "
                  << std::setw(10) << core::to_inr(t.exit_price) << "  "
                  << std::showpos << std::setw(7) << t.net_pnl_inr << "   "
                  << std::noshowpos
                  << (pass ? "VALID" : "VIOLATION") << "\n";
    }

    std::cout << "===================================================================================================\n";
    std::cout << "AUDIT SUMMARY:\n";
    std::cout << "  Total Trades Evaluated       : " << trades.size() << "\n";
    std::cout << "  Opening Auction Violations   : " << auction_violations << " (Trades during 09:15-09:30 IST)\n";
    std::cout << "  Late Squareoff Violations    : " << after_squareoff_violations << " (Trades open past 15:05:15 IST)\n";
    std::cout << "  Overall Timing Integrity    : " << (auction_violations == 0 && after_squareoff_violations == 0 ? "100% PASS" : "FAIL") << "\n";
    std::cout << "===================================================================================================\n";

    DU_EXPECT(trades.size() == 48);
    DU_EXPECT(auction_violations == 0);
    DU_EXPECT(after_squareoff_violations == 0);

    return (auction_violations == 0 && after_squareoff_violations == 0) ? 0 : 1;
}
