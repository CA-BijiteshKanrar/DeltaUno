#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/execution/trade_auditor.hpp"
#include "deltauno/risk/risk_manager.hpp"
#include "test_macros.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using namespace deltauno;

namespace {

uint64_t make_ist_ns(std::string_view ymd, std::string_view hms) {
    return core::ConfigLoader::ist_datetime_to_utc_ns(ymd, hms);
}

core::TradingSignal make_valid_signal(uint32_t token = 349121, const char* symbol = "TATASTEEL") {
    core::TradingSignal sig{};
    sig.token = token;
    core::copy_fixed_str(sig.symbol, symbol);
    sig.side = core::Side::BUY;
    sig.target_entry = core::to_paise(150.00);
    sig.stop_loss = core::to_paise(147.00); // 3.00 INR risk/share
    sig.take_profit = core::to_paise(155.00);
    sig.is_valid = true;
    return sig;
}

core::TradeAuditEntry make_closed_trade(
    uint64_t trade_id,
    uint64_t entry_ns,
    uint64_t exit_ns,
    double gross_pnl_inr,
    double friction_inr = 0.50,
    const char* symbol = "TATASTEEL") {
    core::TradeAuditEntry t{};
    t.trade_id = trade_id;
    core::copy_fixed_str(t.symbol, symbol);
    t.side = core::Side::BUY;
    t.entry_price = core::to_paise(150.00);
    t.exit_price = core::to_paise(150.00 + gross_pnl_inr / 10.0);
    t.quantity = 10;
    t.entry_time_ns = entry_ns;
    t.exit_time_ns = exit_ns;
    t.gross_pnl_inr = gross_pnl_inr;
    t.stt_inr = friction_inr * 0.70;
    t.exchange_fee_inr = friction_inr * 0.16;
    t.gst_inr = friction_inr * 0.03;
    t.stamp_duty_inr = friction_inr * 0.08;
    t.sebi_turnover_inr = friction_inr * 0.03;
    t.net_pnl_inr = gross_pnl_inr - friction_inr;
    t.max_adverse_excursion = 20;
    t.max_favorable_excursion = 50;
    return t;
}

} // namespace

// ============================================================================
// PROBE 1: Intraday Peak-to-Trough Drawdown with Early Gains & Partial Recovery
// ============================================================================
void test_intraday_peak_to_trough_drawdown_and_recovery() {
    std::cout << "[RUN] Probe 1: Intraday Peak-to-Trough Drawdown and Partial Recovery...\n";

    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    execution::TradeAuditor auditor(10000.0);

    const uint64_t d1_t0 = make_ist_ns("2026-11-04", "09:15:00");
    const uint64_t d1_t1 = make_ist_ns("2026-11-04", "10:00:00");
    const uint64_t d1_t2 = make_ist_ns("2026-11-04", "11:30:00");
    const uint64_t d1_t3 = make_ist_ns("2026-11-04", "14:15:00");
    const uint64_t d2_t0 = make_ist_ns("2026-11-05", "09:15:00");

    rm.check_day_rollover(d1_t0);
    auditor.on_session_start(
        execution::TradeAuditor::get_ist_day_index(d1_t0),
        execution::TradeAuditor::format_ist_date(d1_t0));

    std::vector<core::TradeAuditEntry> trades;

    // Trade 1: Early Gain of +350.00 INR net (+351.00 gross, -1.00 fee)
    // Starting equity: 10,000 -> 10,350. Peak = 10,350.
    auto t1 = make_closed_trade(1, d1_t1 - 60'000'000'000ULL, d1_t1, 351.00, 1.00);
    rm.on_trade_closed(t1);
    trades.push_back(t1);

    DU_EXPECT_NEAR(rm.current_capital_inr(), 10350.00, 1e-6);
    DU_EXPECT_NEAR(rm.day_peak_capital_inr(), 10350.00, 1e-6);
    DU_EXPECT_NEAR(rm.day_trough_capital_inr(), 10000.00, 1e-6);
    DU_EXPECT(rm.daily_drawdown_paise() == 0);
    DU_EXPECT(rm.max_intraday_drawdown_paise() == 0);
    DU_EXPECT(!rm.is_kill_switch_active());

    // Trade 2: Severe Dip of -420.00 INR net (-419.00 gross, -1.00 fee)
    // Equity: 10,350 -> 9,930. Trough = 9,930. Peak-to-trough drop = 420.00 INR.
    auto t2 = make_closed_trade(2, d1_t2 - 60'000'000'000ULL, d1_t2, -419.00, 1.00);
    rm.on_trade_closed(t2);
    trades.push_back(t2);

    DU_EXPECT_NEAR(rm.current_capital_inr(), 9930.00, 1e-6);
    DU_EXPECT_NEAR(rm.day_peak_capital_inr(), 10350.00, 1e-6);
    DU_EXPECT_NEAR(rm.day_trough_capital_inr(), 9930.00, 1e-6);
    DU_EXPECT(rm.daily_drawdown_paise() == 42000); // 420 INR
    DU_EXPECT(rm.max_intraday_drawdown_paise() == 42000);
    DU_EXPECT(!rm.is_kill_switch_active()); // 420 < 500 limit

    // Trade 3: Partial Recovery of +270.00 INR net (+271.00 gross, -1.00 fee)
    // Equity: 9,930 -> 10,200. Closing daily net PnL = +200.00 INR!
    // Closing drawdown from day peak is 10,350 - 10,200 = 150.00 INR.
    auto t3 = make_closed_trade(3, d1_t3 - 60'000'000'000ULL, d1_t3, 271.00, 1.00);
    rm.on_trade_closed(t3);
    trades.push_back(t3);

    DU_EXPECT_NEAR(rm.current_capital_inr(), 10200.00, 1e-6);
    DU_EXPECT_NEAR(rm.day_peak_capital_inr(), 10350.00, 1e-6);
    DU_EXPECT_NEAR(rm.day_trough_capital_inr(), 9930.00, 1e-6);
    DU_EXPECT(rm.daily_drawdown_paise() == 15000); // Closing drawdown is 150 INR

    // CRITICAL ADVERSARIAL ASSERTION: max_intraday_drawdown_paise MUST retain 420.00 INR
    // and MUST NOT be overwritten by the closing drawdown of 150.00 INR!
    DU_EXPECT(rm.max_intraday_drawdown_paise() == 42000);
    DU_EXPECT(!rm.is_kill_switch_active());

    // Rollover to Day 2 archives Day 1
    rm.check_day_rollover(d2_t0);
    DU_EXPECT(rm.kill_switch_history().size() == 1);

    const auto& ks_audit = rm.kill_switch_history()[0];
    DU_EXPECT(ks_audit.date_ist == "2026-11-04");
    DU_EXPECT_NEAR(ks_audit.starting_capital_inr, 10000.00, 1e-6);
    DU_EXPECT_NEAR(ks_audit.peak_capital_inr, 10350.00, 1e-6);
    DU_EXPECT_NEAR(ks_audit.trough_capital_inr, 9930.00, 1e-6);
    DU_EXPECT_NEAR(ks_audit.max_drawdown_inr, 420.00, 1e-6);
    DU_EXPECT_NEAR(ks_audit.max_drawdown_pct, (420.0 / 10350.0) * 100.0, 1e-4);
    DU_EXPECT(ks_audit.status == risk::KillSwitchStatus::CLEAR);
    DU_EXPECT(ks_audit.trade_count == 3);

    // Now evaluate via TradeAuditor and assert exact parity
    const auto report = auditor.generate_report(trades, rm.kill_switch_history());
    DU_EXPECT(report.daily_sessions.size() == 1);

    const auto& session = report.daily_sessions[0];
    DU_EXPECT(session.date_ist == "2026-11-04");
    DU_EXPECT(session.total_trades == 3);
    DU_EXPECT(session.winning_trades == 2);
    DU_EXPECT(session.losing_trades == 1);
    DU_EXPECT_NEAR(session.starting_equity_inr, 10000.00, 1e-6);
    DU_EXPECT_NEAR(session.ending_equity_inr, 10200.00, 1e-6);
    DU_EXPECT_NEAR(session.net_pnl_inr, 200.00, 1e-6);
    DU_EXPECT_NEAR(session.gross_pnl_inr, 203.00, 1e-6);
    DU_EXPECT_NEAR(session.total_friction_inr, 3.00, 1e-6);

    // CRITICAL ADVERSARIAL ASSERTION: TradeAuditor MUST report 420.00 INR drawdown
    DU_EXPECT_NEAR(session.max_intraday_drawdown_inr, 420.00, 1e-6);
    DU_EXPECT(!session.kill_switch_triggered);

    std::cout << "[PASS] Probe 1: Peak-to-trough drawdown (420.00 INR) verified. Not overwritten by closing drawdown (150.00 INR).\n";
}

// ============================================================================
// PROBE 2: Kill-Switch Trip Boundary and Hard Latch Enforcement
// ============================================================================
void test_kill_switch_trip_boundary_and_latch() {
    std::cout << "[RUN] Probe 2: Kill-Switch Trip Boundary and Hard Latch Enforcement...\n";

    // Sub-probe 2.1: Sub-threshold non-trip at 499.95 INR drawdown
    {
        risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
        const uint64_t t1 = make_ist_ns("2026-11-04", "10:00:00");
        rm.check_day_rollover(t1);

        auto trade_sub = make_closed_trade(1, t1 - 1000, t1, -499.00, 0.95); // net = -499.95 INR
        rm.on_trade_closed(trade_sub);

        DU_EXPECT(rm.daily_drawdown_paise() == 49995);
        DU_EXPECT(!rm.is_kill_switch_active());

        // Can still place orders
        const auto sig = make_valid_signal();
        const auto [q, r] = rm.evaluate_and_size_order(sig, t1 + 10'000'000'000ULL);
        DU_EXPECT(r == risk::RiskRejection::NONE);
        DU_EXPECT(q > 0);
    }

    // Sub-probe 2.2: Exact boundary trip at 500.00 INR drawdown
    {
        risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
        const uint64_t t1 = make_ist_ns("2026-11-04", "10:00:00");
        rm.check_day_rollover(t1);

        auto trade_trip = make_closed_trade(1, t1 - 1000, t1, -499.00, 1.00); // net = -500.00 INR
        rm.on_trade_closed(trade_trip);

        DU_EXPECT(rm.daily_drawdown_paise() == 50000);
        DU_EXPECT(rm.is_kill_switch_active());

        // Hard latch verification: evaluate_and_size_order must veto with EXCEEDS_DAILY_DRAWDOWN_LIMIT
        const auto sig = make_valid_signal();
        const auto [q, r] = rm.evaluate_and_size_order(sig, t1 + 10'000'000'000ULL);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

        // Finalize and verify TRIPPED status in kill_switch_history
        rm.finalize_session();
        DU_EXPECT(rm.kill_switch_history().size() == 1);
        const auto& ks = rm.kill_switch_history()[0];
        DU_EXPECT(ks.status == risk::KillSwitchStatus::TRIPPED);
        DU_EXPECT(ks.tripped_time_ns == t1);
        DU_EXPECT_NEAR(ks.max_drawdown_inr, 500.00, 1e-6);
        DU_EXPECT_NEAR(ks.max_drawdown_pct, 5.00, 1e-4);
    }

    // Sub-probe 2.3: Peak-to-trough trip after positive run (Peak 10,200 -> Drop 700 -> 9,500)
    {
        risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
        execution::TradeAuditor auditor(10000.0);
        const uint64_t t1 = make_ist_ns("2026-11-04", "10:00:00");
        const uint64_t t2 = make_ist_ns("2026-11-04", "10:30:00");
        rm.check_day_rollover(t1);
        auditor.on_session_start(
            execution::TradeAuditor::get_ist_day_index(t1),
            execution::TradeAuditor::format_ist_date(t1));

        std::vector<core::TradeAuditEntry> trades;
        auto tr1 = make_closed_trade(1, t1 - 1000, t1, 201.00, 1.00); // net = +200.00
        rm.on_trade_closed(tr1);
        trades.push_back(tr1);

        auto tr2 = make_closed_trade(2, t2 - 1000, t2, -699.00, 1.00); // net = -700.00
        rm.on_trade_closed(tr2);
        trades.push_back(tr2);

        DU_EXPECT(rm.is_kill_switch_active());
        DU_EXPECT(rm.max_intraday_drawdown_paise() == 70000); // 700 INR peak-to-trough

        rm.finalize_session();
        const auto report = auditor.generate_report(trades, rm.kill_switch_history());
        DU_EXPECT(report.daily_sessions.size() == 1);
        DU_EXPECT(report.daily_sessions[0].kill_switch_triggered);
        DU_EXPECT_NEAR(report.daily_sessions[0].max_intraday_drawdown_inr, 700.00, 1e-6);
        DU_EXPECT(report.aggregate.total_kill_switch_triggers == 1);
    }

    // Sub-probe 2.4: Rollover recovery: Kill-switch resets at 09:15 next day
    {
        risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
        const uint64_t d1 = make_ist_ns("2026-11-04", "10:00:00");
        const uint64_t d2 = make_ist_ns("2026-11-05", "10:00:00");
        rm.check_day_rollover(d1);

        auto tr = make_closed_trade(1, d1 - 1000, d1, -500.00, 0.50);
        rm.on_trade_closed(tr);
        DU_EXPECT(rm.is_kill_switch_active());

        // Rollover to Day 2
        rm.check_day_rollover(d2);
        DU_EXPECT(!rm.is_kill_switch_active());
        DU_EXPECT(rm.daily_drawdown_paise() == 0);

        const auto sig = make_valid_signal();
        const auto [q, r] = rm.evaluate_and_size_order(sig, d2);
        DU_EXPECT(r == risk::RiskRejection::NONE);
        DU_EXPECT(q > 0);
    }

    std::cout << "[PASS] Probe 2: Kill-switch boundary trip, hard latch, and rollover recovery verified.\n";
}

// ============================================================================
// PROBE 3: Multi-Day Rollover Progression & Capital Continuity (0.000000 Leak)
// ============================================================================
void test_multi_day_capital_continuity_zero_leak() {
    std::cout << "[RUN] Probe 3: Multi-Day Capital Continuity (0.000000 INR Leak)...\n";

    execution::TradeAuditor auditor(10000.0);
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);

    // 10 consecutive sessions
    constexpr int kNumDays = 10;
    const double net_pnls[kNumDays] = {
        +84.3725,   // Day 1
        -32.1850,   // Day 2
        +112.5400,  // Day 3
        -78.9100,   // Day 4
        +45.2200,   // Day 5
        0.0000,     // Day 6 (zero trade day)
        -15.4500,   // Day 7
        +68.9000,   // Day 8
        0.0000,     // Day 9 (zero trade day)
        +104.1125   // Day 10
    };

    std::vector<core::TradeAuditEntry> trades;

    for (int day = 0; day < kNumDays; ++day) {
        std::ostringstream ss;
        ss << "2026-11-" << std::setw(2) << std::setfill('0') << (day + 2);
        const uint64_t day_start_ns = make_ist_ns(ss.str(), "09:15:00");
        const int64_t day_index = execution::TradeAuditor::get_ist_day_index(day_start_ns);
        const std::string date_str = execution::TradeAuditor::format_ist_date(day_start_ns);

        rm.check_day_rollover(day_start_ns);
        auditor.on_session_start(day_index, date_str);

        if (std::abs(net_pnls[day]) > 1e-6) {
            const double gross = net_pnls[day] + 1.25;
            auto t = make_closed_trade(day + 1, day_start_ns + 1800'000'000'000ULL, day_start_ns + 3600'000'000'000ULL, gross, 1.25);
            rm.on_trade_closed(t);
            trades.push_back(t);
        }
    }
    rm.finalize_session();

    const auto report = auditor.generate_report(trades, rm.kill_switch_history());

    DU_EXPECT(report.daily_sessions.size() == kNumDays);
    DU_EXPECT(report.equity_curve.size() == kNumDays);
    DU_EXPECT(report.kill_switch_history.size() == kNumDays);

    double expected_cumulative_capital = 10000.0;

    for (size_t d = 0; d < kNumDays; ++d) {
        const auto& s_curr = report.daily_sessions[d];
        const auto& eq_curr = report.equity_curve[d];
        const auto& ks_curr = report.kill_switch_history[d];

        // 1. Session starting equity must strictly equal expected cumulative capital
        DU_EXPECT_NEAR(s_curr.starting_equity_inr, expected_cumulative_capital, 1e-12);
        DU_EXPECT_NEAR(eq_curr.starting_equity_inr, expected_cumulative_capital, 1e-12);
        DU_EXPECT_NEAR(ks_curr.starting_capital_inr, expected_cumulative_capital, 1e-12);

        // 2. Net PnL must match exact prescribed value
        DU_EXPECT_NEAR(s_curr.net_pnl_inr, net_pnls[d], 1e-12);
        DU_EXPECT_NEAR(eq_curr.daily_net_pnl_inr, net_pnls[d], 1e-12);

        expected_cumulative_capital += net_pnls[d];

        // 3. Ending equity must match exactly
        DU_EXPECT_NEAR(s_curr.ending_equity_inr, expected_cumulative_capital, 1e-12);
        DU_EXPECT_NEAR(eq_curr.closing_equity_inr, expected_cumulative_capital, 1e-12);

        // 4. Consecutive session continuity: Day[d+1].starting == Day[d].ending
        if (d + 1 < kNumDays) {
            const auto& s_next = report.daily_sessions[d + 1];
            const auto& eq_next = report.equity_curve[d + 1];
            DU_EXPECT_NEAR(s_next.starting_equity_inr, s_curr.ending_equity_inr, 1e-12);
            DU_EXPECT_NEAR(eq_next.starting_equity_inr, eq_curr.closing_equity_inr, 1e-12);
        }
    }

    // Global balance sheet identity
    DU_EXPECT_NEAR(report.aggregate.ending_capital_inr, expected_cumulative_capital, 1e-12);
    DU_EXPECT_NEAR(rm.current_capital_inr(), expected_cumulative_capital, 1e-12);

    std::cout << "[PASS] Probe 3: 10-day capital continuity verified with 0.000000000000 INR leak.\n";
}

// ============================================================================
// PROBE 4: Consecutive Zero-Trade Sessions (5 Days with 0 Trades)
// ============================================================================
void test_consecutive_zero_trade_sessions() {
    std::cout << "[RUN] Probe 4: Consecutive Zero-Trade Sessions (5 Days with 0 Trades)...\n";

    execution::TradeAuditor auditor(10000.0);
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);

    // Day 1: Trades +150.00 net
    // Days 2, 3, 4, 5, 6: 0 trades (5 consecutive zero-trade days)
    // Day 7: Trades +50.00 net
    std::vector<core::TradeAuditEntry> trades;

    for (int d = 0; d < 7; ++d) {
        std::ostringstream ss;
        ss << "2026-11-" << std::setw(2) << std::setfill('0') << (d + 2);
        const uint64_t day_start_ns = make_ist_ns(ss.str(), "09:15:00");
        const int64_t day_index = execution::TradeAuditor::get_ist_day_index(day_start_ns);
        const std::string date_str = execution::TradeAuditor::format_ist_date(day_start_ns);

        rm.check_day_rollover(day_start_ns);
        auditor.on_session_start(day_index, date_str);

        if (d == 0) {
            auto t = make_closed_trade(1, day_start_ns + 1000, day_start_ns + 2000, 151.00, 1.00); // +150 net
            rm.on_trade_closed(t);
            trades.push_back(t);
        } else if (d == 6) {
            auto t = make_closed_trade(2, day_start_ns + 1000, day_start_ns + 2000, 51.00, 1.00); // +50 net
            rm.on_trade_closed(t);
            trades.push_back(t);
        }
    }
    rm.finalize_session();

    const auto report = auditor.generate_report(trades, rm.kill_switch_history());

    DU_EXPECT(report.daily_sessions.size() == 7);
    DU_EXPECT(report.equity_curve.size() == 7);
    DU_EXPECT(report.kill_switch_history.size() == 7);

    // Verify Day 1
    DU_EXPECT_NEAR(report.daily_sessions[0].ending_equity_inr, 10150.00, 1e-6);

    // Verify Days 2 through 6 (indices 1 through 5)
    for (size_t i = 1; i <= 5; ++i) {
        const auto& sa = report.daily_sessions[i];
        const auto& eq = report.equity_curve[i];
        const auto& ks = report.kill_switch_history[i];

        // 1. Starting capital == Closing capital == 10,150.00
        DU_EXPECT_NEAR(sa.starting_equity_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(sa.ending_equity_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(eq.starting_equity_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(eq.closing_equity_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(ks.starting_capital_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(ks.peak_capital_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(ks.trough_capital_inr, 10150.00, 1e-12);

        // 2. 0 trades recorded
        DU_EXPECT(sa.total_trades == 0);
        DU_EXPECT(sa.winning_trades == 0);
        DU_EXPECT(sa.losing_trades == 0);
        DU_EXPECT(sa.breakeven_trades == 0);
        DU_EXPECT(eq.trade_count == 0);
        DU_EXPECT(ks.trade_count == 0);

        // 3. 0 PnL and friction
        DU_EXPECT_NEAR(sa.net_pnl_inr, 0.00, 1e-12);
        DU_EXPECT_NEAR(sa.gross_pnl_inr, 0.00, 1e-12);
        DU_EXPECT_NEAR(sa.total_friction_inr, 0.00, 1e-12);
        DU_EXPECT_NEAR(eq.daily_net_pnl_inr, 0.00, 1e-12);
        DU_EXPECT_NEAR(eq.daily_return_pct, 0.00, 1e-12);

        // 4. Safe kill-switch status & zero drawdown
        DU_EXPECT_NEAR(sa.max_intraday_drawdown_inr, 0.00, 1e-12);
        DU_EXPECT(!sa.kill_switch_triggered);
        DU_EXPECT(ks.status == risk::KillSwitchStatus::CLEAR);
        DU_EXPECT_NEAR(ks.max_drawdown_inr, 0.00, 1e-12);

        // 5. Equity curve preserved at peak
        DU_EXPECT_NEAR(eq.high_water_mark_inr, 10150.00, 1e-12);
        DU_EXPECT_NEAR(eq.drawdown_from_hwm_inr, 0.00, 1e-12);
        DU_EXPECT_NEAR(eq.drawdown_from_hwm_pct, 0.00, 1e-12);
    }

    // Verify Day 7 picks up seamlessly from 10,150.00 to 10,200.00
    DU_EXPECT_NEAR(report.daily_sessions[6].starting_equity_inr, 10150.00, 1e-12);
    DU_EXPECT_NEAR(report.daily_sessions[6].ending_equity_inr, 10200.00, 1e-12);
    DU_EXPECT_NEAR(report.aggregate.ending_capital_inr, 10200.00, 1e-12);

    std::cout << "[PASS] Probe 4: 5 consecutive zero-trade sessions verified with starting == closing equity and SAFE status.\n";
}

// ============================================================================
// PROBE 5: Running High-Water Mark (HWM) and Drawdown from HWM Across Days
// ============================================================================
void test_running_portfolio_high_water_mark_drawdown() {
    std::cout << "[RUN] Probe 5: Running Portfolio High-Water Mark & Drawdown from HWM...\n";

    execution::TradeAuditor auditor(10000.0);

    const double daily_deltas[8] = {+400.0, +250.0, -300.0, -250.0, +200.0, +400.0, -100.0, +50.0};
    const double expected_closing[8] = {10400.0, 10650.0, 10350.0, 10100.0, 10300.0, 10700.0, 10600.0, 10650.0};
    const double expected_hwm[8] = {10400.0, 10650.0, 10650.0, 10650.0, 10650.0, 10700.0, 10700.0, 10700.0};
    const double expected_dd_inr[8] = {0.0, 0.0, 300.0, 550.0, 350.0, 0.0, 100.0, 50.0};

    std::vector<core::TradeAuditEntry> trades;

    for (int d = 0; d < 8; ++d) {
        std::ostringstream ss;
        ss << "2026-11-" << std::setw(2) << std::setfill('0') << (d + 2);
        const uint64_t day_start_ns = make_ist_ns(ss.str(), "09:15:00");
        const int64_t day_index = execution::TradeAuditor::get_ist_day_index(day_start_ns);
        const std::string date_str = execution::TradeAuditor::format_ist_date(day_start_ns);
        auditor.on_session_start(day_index, date_str);

        auto t = make_closed_trade(d + 1, day_start_ns + 1000'000'000ULL, day_start_ns + 2000'000'000ULL, daily_deltas[d] + 1.0, 1.0);
        trades.push_back(t);
    }

    const auto report = auditor.generate_report(trades);

    DU_EXPECT(report.equity_curve.size() == 8);

    for (size_t i = 0; i < 8; ++i) {
        const auto& pt = report.equity_curve[i];
        DU_EXPECT_NEAR(pt.closing_equity_inr, expected_closing[i], 1e-6);
        DU_EXPECT_NEAR(pt.high_water_mark_inr, expected_hwm[i], 1e-6);
        DU_EXPECT_NEAR(pt.drawdown_from_hwm_inr, expected_dd_inr[i], 1e-6);

        const double expected_dd_pct = (expected_dd_inr[i] / expected_hwm[i]) * 100.0;
        DU_EXPECT_NEAR(pt.drawdown_from_hwm_pct, expected_dd_pct, 1e-4);
    }

    // Macro aggregates
    DU_EXPECT_NEAR(report.aggregate.peak_capital_inr, 10700.00, 1e-6);
    DU_EXPECT_NEAR(report.aggregate.max_portfolio_drawdown_inr, 550.00, 1e-6);
    DU_EXPECT_NEAR(report.aggregate.max_portfolio_drawdown_pct, (550.0 / 10650.0) * 100.0, 1e-4);

    std::cout << "[PASS] Probe 5: Running HWM ratcheting and drawdown from HWM verified.\n";
}

// ============================================================================
// PROBE 6: Permanent Cumulative Pilot Stop-Out Latch vs Daily Kill-Switch
// ============================================================================
void test_cumulative_pilot_stopout_latch_persistence() {
    std::cout << "[RUN] Probe 6: Permanent Cumulative Pilot Stop-Out Latch Persistence...\n";

    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t d1 = make_ist_ns("2026-11-04", "10:00:00");
    const uint64_t d2 = make_ist_ns("2026-11-05", "10:00:00");
    const uint64_t d3 = make_ist_ns("2026-11-06", "10:00:00");

    rm.check_day_rollover(d1);

    // Day 1 loss: 450 INR (capital = 9,550)
    auto t1 = make_closed_trade(1, d1 - 1000, d1, -449.0, 1.0);
    rm.on_trade_closed(t1);
    DU_EXPECT(!rm.is_pilot_stopout_active());

    // Day 2 loss: 450 INR (capital = 9,100)
    rm.check_day_rollover(d2);
    auto t2 = make_closed_trade(2, d2 - 1000, d2, -449.0, 1.0);
    rm.on_trade_closed(t2);
    DU_EXPECT(!rm.is_pilot_stopout_active());

    // Day 3 loss: 601 INR (capital = 8,499 <= 8,500 threshold)
    rm.check_day_rollover(d3);
    auto t3 = make_closed_trade(3, d3 - 1000, d3, -600.0, 1.0);
    rm.on_trade_closed(t3);

    // Must activate permanent pilot stopout latch!
    DU_EXPECT(rm.is_pilot_stopout_active());

    // Even if we rollover to Day 4, 5, 6... the latch must NEVER clear
    for (int d = 7; d <= 15; ++d) {
        std::ostringstream ss;
        ss << "2026-11-" << std::setw(2) << std::setfill('0') << d;
        const uint64_t next_day = make_ist_ns(ss.str(), "10:00:00");
        rm.check_day_rollover(next_day);

        DU_EXPECT(rm.is_pilot_stopout_active());

        const auto sig = make_valid_signal();
        const auto [q, r] = rm.evaluate_and_size_order(sig, next_day);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);
    }

    std::cout << "[PASS] Probe 6: Permanent cumulative pilot stopout latch verified across multi-day rollovers.\n";
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  RUNNING ADVERSARIAL M3 INTRADAY DRAWDOWN & PROGRESSION SUITE   \n";
    std::cout << "=================================================================\n";

    test_intraday_peak_to_trough_drawdown_and_recovery();
    test_kill_switch_trip_boundary_and_latch();
    test_multi_day_capital_continuity_zero_leak();
    test_consecutive_zero_trade_sessions();
    test_running_portfolio_high_water_mark_drawdown();
    test_cumulative_pilot_stopout_latch_persistence();

    std::cout << "=================================================================\n";
    std::cout << "  ALL 6 ADVERSARIAL M3 CHALLENGE PROBES PASSED 100% CLEANLY!     \n";
    std::cout << "=================================================================\n";
    return 0;
}
