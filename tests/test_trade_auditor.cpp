#include "deltauno/execution/trade_auditor.hpp"
#include "test_macros.hpp"

#include <iostream>
#include <sstream>
#include <vector>

using namespace deltauno;

void test_auditor_metrics_and_breakeven_identity() {
    std::vector<core::TradeAuditEntry> trades;

    // 1. Winning trade (+9.00 gross, -0.20 fees = +8.80 net)
    core::TradeAuditEntry win{};
    win.trade_id = 1;
    core::copy_fixed_str(win.symbol, "TATASTEEL");
    win.entry_price = core::to_paise(150.00);
    win.exit_price = core::to_paise(159.00);
    win.quantity = 1;
    win.max_adverse_excursion = 30;    // 0.30 INR
    win.max_favorable_excursion = 320; // 3.20 INR
    win.gross_pnl_inr = 9.00;
    win.stt_inr = 0.11;
    win.exchange_fee_inr = 0.05;
    win.gst_inr = 0.01;
    win.stamp_duty_inr = 0.02;
    win.sebi_turnover_inr = 0.01;
    win.net_pnl_inr = 8.80;
    trades.push_back(win);

    // 2. Losing trade (-4.50 gross, -0.20 fees = -4.70 net)
    core::TradeAuditEntry loss{};
    loss.trade_id = 2;
    core::copy_fixed_str(loss.symbol, "SBIN");
    loss.entry_price = core::to_paise(800.00);
    loss.exit_price = core::to_paise(795.50);
    loss.quantity = 1;
    loss.max_adverse_excursion = 160; // 1.60 INR
    loss.max_favorable_excursion = 20;  // 0.20 INR
    loss.gross_pnl_inr = -4.50;
    loss.stt_inr = 0.11;
    loss.exchange_fee_inr = 0.05;
    loss.gst_inr = 0.01;
    loss.stamp_duty_inr = 0.02;
    loss.sebi_turnover_inr = 0.01;
    loss.net_pnl_inr = -4.70;
    trades.push_back(loss);

    // 3. Zero-loss breakeven scratch trade (+0.22 gross, -0.20 fees = +0.02 net within ±0.05 band)
    core::TradeAuditEntry scratch{};
    scratch.trade_id = 3;
    core::copy_fixed_str(scratch.symbol, "ITC");
    scratch.entry_price = core::to_paise(450.00);
    scratch.exit_price = core::to_paise(450.22);
    scratch.quantity = 1;
    scratch.max_adverse_excursion = 20;  // 0.20 INR
    scratch.max_favorable_excursion = 170; // 1.70 INR
    scratch.gross_pnl_inr = 0.22;
    scratch.stt_inr = 0.11;
    scratch.exchange_fee_inr = 0.05;
    scratch.gst_inr = 0.01;
    scratch.stamp_duty_inr = 0.02;
    scratch.sebi_turnover_inr = 0.01;
    scratch.net_pnl_inr = 0.02;
    trades.push_back(scratch);

    const auto s = execution::TradeAuditor::evaluate(trades);

    DU_EXPECT(s.total_trades == 3);
    DU_EXPECT(s.winning_trades == 1);
    DU_EXPECT(s.losing_trades == 1);
    DU_EXPECT(s.breakeven_trades == 1);

    // Verify exact PnL accounting identity: Gross Profit - Gross Loss - Friction == Net PnL
    DU_EXPECT_NEAR(s.gross_profit_inr, 9.22, 1e-9);
    DU_EXPECT_NEAR(s.gross_loss_inr, 4.50, 1e-9);
    DU_EXPECT_NEAR(s.total_taxes_and_friction_inr, 0.60, 1e-9);
    DU_EXPECT_NEAR(s.gross_profit_inr - s.gross_loss_inr - s.total_taxes_and_friction_inr, s.net_pnl_inr, 1e-9);
    DU_EXPECT_NEAR(s.profit_factor, 9.22 / 4.50, 1e-6);
    DU_EXPECT_NEAR(s.avg_mae_paise, (30.0 + 160.0 + 20.0) / 3.0, 1e-6);
    DU_EXPECT_NEAR(s.avg_mfe_paise, (320.0 + 20.0 + 170.0) / 3.0, 1e-6);

    std::ostringstream report_buf;
    execution::TradeAuditor::print_report(s, report_buf);
    DU_EXPECT(report_buf.str().find("DELTAUNO PERFORMANCE AUDIT REPORT") != std::string::npos);

    std::cout << "[PASS] test_auditor_metrics_and_breakeven_identity\n";
}

void test_multi_day_session_partitioning() {
    execution::TradeAuditor auditor(10000.0);

    // Days:
    // Day 1: 2026-11-02 (epoch day 20760)
    // Day 2: 2026-11-03 (epoch day 20761) - zero trade day!
    // Day 3: 2026-11-04 (epoch day 20762)
    auditor.on_session_start(20760, "2026-11-02");
    auditor.on_session_start(20761, "2026-11-03");
    auditor.on_session_start(20762, "2026-11-04");

    std::vector<core::TradeAuditEntry> trades;

    // Day 1 trades
    core::TradeAuditEntry t1{};
    t1.trade_id = 1;
    t1.entry_time_ns = 20760ULL * 86400ULL * 1'000'000'000ULL;
    t1.exit_time_ns = t1.entry_time_ns + 1800'000'000'000ULL;
    t1.gross_pnl_inr = 89.25;
    t1.stt_inr = 0.50;
    t1.exchange_fee_inr = 0.35;
    t1.gst_inr = 0.15;
    t1.stamp_duty_inr = 0.20;
    t1.sebi_turnover_inr = 0.05;
    t1.net_pnl_inr = 88.00;
    trades.push_back(t1);

    core::TradeAuditEntry t2{};
    t2.trade_id = 2;
    t2.entry_time_ns = 20760ULL * 86400ULL * 1'000'000'000ULL + 3600'000'000'000ULL;
    t2.exit_time_ns = t2.entry_time_ns + 1800'000'000'000ULL;
    t2.gross_pnl_inr = -23.00;
    t2.stt_inr = 0.40;
    t2.exchange_fee_inr = 0.30;
    t2.gst_inr = 0.12;
    t2.stamp_duty_inr = 0.15;
    t2.sebi_turnover_inr = 0.03;
    t2.net_pnl_inr = -24.00;
    trades.push_back(t2);

    // Day 3 trades (Day 2 has zero trades)
    core::TradeAuditEntry t3{};
    t3.trade_id = 3;
    t3.entry_time_ns = 20762ULL * 86400ULL * 1'000'000'000ULL;
    t3.exit_time_ns = t3.entry_time_ns + 1800'000'000'000ULL;
    t3.gross_pnl_inr = 40.00;
    t3.stt_inr = 0.30;
    t3.exchange_fee_inr = 0.25;
    t3.gst_inr = 0.10;
    t3.stamp_duty_inr = 0.10;
    t3.sebi_turnover_inr = 0.05;
    t3.net_pnl_inr = 39.20;
    trades.push_back(t3);

    const auto report = auditor.generate_report(trades);

    DU_EXPECT(report.daily_sessions.size() == 3);

    // Day 1 verification
    const auto& d1 = report.daily_sessions[0];
    DU_EXPECT(d1.date_ist == "2026-11-02");
    DU_EXPECT(d1.total_trades == 2);
    DU_EXPECT(d1.winning_trades == 1);
    DU_EXPECT(d1.losing_trades == 1);
    DU_EXPECT_NEAR(d1.starting_equity_inr, 10000.00, 1e-6);
    DU_EXPECT_NEAR(d1.net_pnl_inr, 64.00, 1e-6);
    DU_EXPECT_NEAR(d1.ending_equity_inr, 10064.00, 1e-6);
    DU_EXPECT(!d1.kill_switch_triggered);

    // Day 2 verification (Zero-Trade Day preserved)
    const auto& d2 = report.daily_sessions[1];
    DU_EXPECT(d2.date_ist == "2026-11-03");
    DU_EXPECT(d2.total_trades == 0);
    DU_EXPECT_NEAR(d2.starting_equity_inr, 10064.00, 1e-6);
    DU_EXPECT_NEAR(d2.net_pnl_inr, 0.00, 1e-6);
    DU_EXPECT_NEAR(d2.ending_equity_inr, 10064.00, 1e-6);
    DU_EXPECT(!d2.kill_switch_triggered);

    // Day 3 verification
    const auto& d3 = report.daily_sessions[2];
    DU_EXPECT(d3.date_ist == "2026-11-04");
    DU_EXPECT(d3.total_trades == 1);
    DU_EXPECT_NEAR(d3.starting_equity_inr, 10064.00, 1e-6);
    DU_EXPECT_NEAR(d3.net_pnl_inr, 39.20, 1e-6);
    DU_EXPECT_NEAR(d3.ending_equity_inr, 10103.20, 1e-6);
    DU_EXPECT(!d3.kill_switch_triggered);

    // Overall aggregate verification
    DU_EXPECT_NEAR(report.aggregate.ending_capital_inr, 10103.20, 1e-6);
    DU_EXPECT_NEAR(report.aggregate.net_return_pct, 1.032, 1e-3);
    DU_EXPECT(report.aggregate.total_sessions == 3);
    DU_EXPECT(report.aggregate.profitable_days == 2);
    DU_EXPECT(report.aggregate.scratch_days == 1);

    std::cout << "[PASS] test_multi_day_session_partitioning\n";
}

void test_itemized_statutory_friction_audit() {
    core::TradeAuditEntry t{};
    t.trade_id = 100;
    core::copy_fixed_str(t.symbol, "TATASTEEL");
    t.entry_price = core::to_paise(100.00);
    t.exit_price = core::to_paise(105.00);
    t.quantity = 50;

    const double buy_val = 5000.00;
    const double sell_val = 5250.00;
    const double turnover = 10250.00;

    t.gross_pnl_inr = sell_val - buy_val; // +250.00 INR
    t.stt_inr = sell_val * 0.00025;       // 1.3125 INR
    t.exchange_fee_inr = turnover * 0.0000297; // 0.304425 INR
    t.sebi_turnover_inr = turnover * 0.000001; // 0.01025 INR
    t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18; // 0.0566415 INR
    t.stamp_duty_inr = buy_val * 0.00003; // 0.15 INR

    const double total_fric = t.total_friction_inr();
    t.net_pnl_inr = t.gross_pnl_inr - total_fric;

    DU_EXPECT_NEAR(t.turnover_inr(), 10250.00, 1e-6);
    DU_EXPECT_NEAR(t.total_friction_inr(), 1.8338165, 1e-6);

    std::vector<core::TradeAuditEntry> trades{t};
    const auto s = execution::TradeAuditor::evaluate(trades);

    DU_EXPECT_NEAR(s.total_stt_inr, 1.3125, 1e-6);
    DU_EXPECT_NEAR(s.total_exchange_fee_inr, 0.304425, 1e-6);
    DU_EXPECT_NEAR(s.total_gst_inr, 0.0566415, 1e-6);
    DU_EXPECT_NEAR(s.total_stamp_duty_inr, 0.15, 1e-6);
    DU_EXPECT_NEAR(s.total_sebi_turnover_inr, 0.01025, 1e-6);
    DU_EXPECT_NEAR(s.total_turnover_inr, 10250.00, 1e-6);
    DU_EXPECT_NEAR(s.total_taxes_and_friction_inr, 1.8338165, 1e-6);

    // Verify strict accounting identity
    DU_EXPECT_NEAR(s.gross_profit_inr - s.gross_loss_inr - s.total_taxes_and_friction_inr, s.net_pnl_inr, 1e-9);

    std::cout << "[PASS] test_itemized_statutory_friction_audit\n";
}

void test_intraday_drawdown_and_kill_switch_audit() {
    std::vector<core::TradeAuditEntry> trades;
    const uint64_t base_ns = 20760ULL * 86400ULL * 1'000'000'000ULL;

    // Sequence of 4 intraday trades: +50, -40, -80, +30
    // Running Net: 0 -> +50 -> +10 -> -70 -> -40
    // Intraday Peak: 0 -> +50 -> +50 -> +50 -> +50
    // Peak-to-Trough Drawdown: 50 - (-70) = 120 INR
    double pnl_seq[] = {50.0, -40.0, -80.0, 30.0};
    for (size_t i = 0; i < 4; ++i) {
        core::TradeAuditEntry t{};
        t.trade_id = i + 1;
        t.entry_time_ns = base_ns + i * 3600'000'000'000ULL;
        t.exit_time_ns = t.entry_time_ns + 1800'000'000'000ULL;
        t.gross_pnl_inr = pnl_seq[i];
        t.net_pnl_inr = pnl_seq[i];
        trades.push_back(t);
    }

    // 1. Evaluate with ₹500 limit (kill switch CLEAR)
    const auto rep_safe = execution::TradeAuditor::evaluate_multi_day(trades, 10000.0, {}, {}, 500.0);
    DU_EXPECT(rep_safe.daily_sessions.size() == 1);
    DU_EXPECT_NEAR(rep_safe.daily_sessions[0].max_intraday_drawdown_inr, 120.0, 1e-6);
    DU_EXPECT(!rep_safe.daily_sessions[0].kill_switch_triggered);
    DU_EXPECT(rep_safe.aggregate.total_kill_switch_triggers == 0);

    // 2. Evaluate with ₹100 limit (kill switch TRIPPED)
    const auto rep_tripped = execution::TradeAuditor::evaluate_multi_day(trades, 10000.0, {}, {}, 100.0);
    DU_EXPECT(rep_tripped.daily_sessions.size() == 1);
    DU_EXPECT_NEAR(rep_tripped.daily_sessions[0].max_intraday_drawdown_inr, 120.0, 1e-6);
    DU_EXPECT(rep_tripped.daily_sessions[0].kill_switch_triggered);
    DU_EXPECT(rep_tripped.aggregate.total_kill_switch_triggers == 1);

    std::cout << "[PASS] test_intraday_drawdown_and_kill_switch_audit\n";
}

void test_equity_curve_and_high_water_mark_drawdowns() {
    std::vector<core::TradeAuditEntry> trades;
    // Day 1 (epoch 20760): +300 INR net
    core::TradeAuditEntry t1{};
    t1.entry_time_ns = 20760ULL * 86400ULL * 1'000'000'000ULL;
    t1.exit_time_ns = t1.entry_time_ns + 1000;
    t1.gross_pnl_inr = 300.0;
    t1.net_pnl_inr = 300.0;
    trades.push_back(t1);

    // Day 2 (epoch 20761): -100 INR net
    core::TradeAuditEntry t2{};
    t2.entry_time_ns = 20761ULL * 86400ULL * 1'000'000'000ULL;
    t2.exit_time_ns = t2.entry_time_ns + 1000;
    t2.gross_pnl_inr = -100.0;
    t2.net_pnl_inr = -100.0;
    trades.push_back(t2);

    // Day 3 (epoch 20762): +200 INR net
    core::TradeAuditEntry t3{};
    t3.entry_time_ns = 20762ULL * 86400ULL * 1'000'000'000ULL;
    t3.exit_time_ns = t3.entry_time_ns + 1000;
    t3.gross_pnl_inr = 200.0;
    t3.net_pnl_inr = 200.0;
    trades.push_back(t3);

    const auto rep = execution::TradeAuditor::evaluate_multi_day(trades, 10000.0);
    DU_EXPECT(rep.equity_curve.size() == 3);

    // Day 1
    const auto& pt1 = rep.equity_curve[0];
    DU_EXPECT_NEAR(pt1.starting_equity_inr, 10000.0, 1e-6);
    DU_EXPECT_NEAR(pt1.closing_equity_inr, 10300.0, 1e-6);
    DU_EXPECT_NEAR(pt1.daily_return_pct, 3.0, 1e-6);
    DU_EXPECT_NEAR(pt1.cumulative_return_pct, 3.0, 1e-6);
    DU_EXPECT_NEAR(pt1.high_water_mark_inr, 10300.0, 1e-6);
    DU_EXPECT_NEAR(pt1.drawdown_from_hwm_inr, 0.0, 1e-6);

    // Day 2
    const auto& pt2 = rep.equity_curve[1];
    DU_EXPECT_NEAR(pt2.starting_equity_inr, 10300.0, 1e-6);
    DU_EXPECT_NEAR(pt2.closing_equity_inr, 10200.0, 1e-6);
    DU_EXPECT_NEAR(pt2.daily_return_pct, (-100.0 / 10300.0) * 100.0, 1e-4);
    DU_EXPECT_NEAR(pt2.cumulative_return_pct, 2.0, 1e-6);
    DU_EXPECT_NEAR(pt2.high_water_mark_inr, 10300.0, 1e-6);
    DU_EXPECT_NEAR(pt2.drawdown_from_hwm_inr, 100.0, 1e-6);
    DU_EXPECT_NEAR(pt2.drawdown_from_hwm_pct, (100.0 / 10300.0) * 100.0, 1e-4);

    // Day 3
    const auto& pt3 = rep.equity_curve[2];
    DU_EXPECT_NEAR(pt3.starting_equity_inr, 10200.0, 1e-6);
    DU_EXPECT_NEAR(pt3.closing_equity_inr, 10400.0, 1e-6);
    DU_EXPECT_NEAR(pt3.high_water_mark_inr, 10400.0, 1e-6);
    DU_EXPECT_NEAR(pt3.drawdown_from_hwm_inr, 0.0, 1e-6);

    // Macro aggregates
    DU_EXPECT_NEAR(rep.aggregate.peak_capital_inr, 10400.0, 1e-6);
    DU_EXPECT_NEAR(rep.aggregate.max_portfolio_drawdown_inr, 100.0, 1e-6);

    std::cout << "[PASS] test_equity_curve_and_high_water_mark_drawdowns\n";
}

void test_ascii_table_formatting_and_zero_trade_days() {
    execution::TradeAuditor auditor(10000.0);
    auditor.on_session_start(20760, "2026-11-02");
    auditor.on_session_start(20761, "2026-11-03"); // zero trade day

    std::vector<core::TradeAuditEntry> trades;
    core::TradeAuditEntry t{};
    t.entry_time_ns = 20760ULL * 86400ULL * 1'000'000'000ULL;
    t.exit_time_ns = t.entry_time_ns + 1000;
    t.gross_pnl_inr = 50.0;
    t.stt_inr = 0.50;
    t.exchange_fee_inr = 0.20;
    t.gst_inr = 0.05;
    t.stamp_duty_inr = 0.10;
    t.sebi_turnover_inr = 0.02;
    t.net_pnl_inr = 49.13;
    trades.push_back(t);

    const auto report = auditor.generate_report(trades);

    std::ostringstream ss;
    execution::TradeAuditor::print_multi_day_report(report, ss);
    const std::string out = ss.str();

    DU_EXPECT(out.find("DELTAUNO INSTITUTIONAL PERFORMANCE AUDITOR") != std::string::npos);
    DU_EXPECT(out.find("30-DAY SESSION-BY-SESSION PERFORMANCE BREAKDOWN") != std::string::npos);
    DU_EXPECT(out.find("ITEMIZED STATUTORY CHARGES AUDIT") != std::string::npos);
    DU_EXPECT(out.find("DAILY KILL-SWITCH & DRAWDOWN AUDIT TRAIL") != std::string::npos);
    DU_EXPECT(out.find("30-DAY PORTFOLIO EQUITY CURVE PROGRESSION") != std::string::npos);
    DU_EXPECT(out.find("COMPREHENSIVE 30-DAY STATISTICAL METRICS") != std::string::npos);
    DU_EXPECT(out.find("2026-11-03") != std::string::npos); // Zero trade day is printed

    std::ostringstream ss_fric;
    execution::TradeAuditor::print_statutory_friction_table(report.aggregate, ss_fric);
    const std::string out_fric = ss_fric.str();
    DU_EXPECT(out_fric.find("Securities Transaction Tax (STT)") != std::string::npos);
    DU_EXPECT(out_fric.find("NSE Transaction Turnover Charges") != std::string::npos);
    DU_EXPECT(out_fric.find("Goods & Services Tax (GST)") != std::string::npos);
    DU_EXPECT(out_fric.find("State Stamp Duty") != std::string::npos);
    DU_EXPECT(out_fric.find("SEBI Regulatory Turnover Fee") != std::string::npos);

    std::cout << "[PASS] test_ascii_table_formatting_and_zero_trade_days\n";
}

int main() {
    std::cout << "Running TradeAuditor tests...\n";
    test_auditor_metrics_and_breakeven_identity();
    test_multi_day_session_partitioning();
    test_itemized_statutory_friction_audit();
    test_intraday_drawdown_and_kill_switch_audit();
    test_equity_curve_and_high_water_mark_drawdowns();
    test_ascii_table_formatting_and_zero_trade_days();
    std::cout << "All TradeAuditor tests passed!\n";
    return 0;
}
