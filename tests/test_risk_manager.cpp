#include "deltauno/core/config_loader.hpp"
#include "deltauno/risk/risk_manager.hpp"
#include "test_macros.hpp"

#include <iostream>

using namespace deltauno;

namespace {

uint64_t make_ist_ns(std::string_view ymd, std::string_view hms) {
    return core::ConfigLoader::ist_datetime_to_utc_ns(ymd, hms);
}

core::TradingSignal make_valid_signal() {
    core::TradingSignal sig{};
    sig.token = 349121;
    core::copy_fixed_str(sig.symbol, "TATASTEEL");
    sig.side = core::Side::BUY;
    sig.target_entry = core::to_paise(150.00);
    sig.stop_loss = core::to_paise(147.00); // 3.00 INR risk/share
    sig.take_profit = core::to_paise(155.00);
    sig.is_valid = true;
    return sig;
}

} // namespace

void test_risk_sizing_and_caps() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t morning_ns = make_ist_ns("2026-11-04", "09:45:00");
    const auto sig = make_valid_signal();

    // floor(120 / 3) = 40 shares, capped by floor(5000 / 150) = 33 shares (₹4,950 notional, ₹99 risk <= ₹120)
    const auto [qty, rejection] = rm.evaluate_and_size_order(sig, morning_ns);
    DU_EXPECT(rejection == risk::RiskRejection::NONE);
    DU_EXPECT(qty == 33);

    std::cout << "[PASS] test_risk_sizing_and_caps\n";
}

void test_net_drawdown_killswitch_and_day_rollover() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t day1_ns = make_ist_ns("2026-11-04", "10:00:00");
    const uint64_t day2_ns = make_ist_ns("2026-11-05", "10:00:00");

    // Lose 300 INR, then win 400 INR -> net = +100 INR, peak = +100 INR, drawdown = 0 INR
    core::TradeAuditEntry t1{};
    t1.exit_time_ns = day1_ns;
    t1.net_pnl_inr = -300.0;
    rm.on_trade_closed(t1);
    DU_EXPECT(!rm.is_kill_switch_active());

    core::TradeAuditEntry t2{};
    t2.exit_time_ns = day1_ns + 60'000'000'000ULL;
    t2.net_pnl_inr = +400.0;
    rm.on_trade_closed(t2);
    DU_EXPECT(!rm.is_kill_switch_active());
    DU_EXPECT(rm.daily_drawdown_paise() == 0);

    // Lose 250 INR -> net = -150 INR, peak = +100 INR, peak-to-trough drawdown = 250 INR (< 500 INR)
    core::TradeAuditEntry t3{};
    t3.exit_time_ns = day1_ns + 120'000'000'000ULL;
    t3.net_pnl_inr = -250.0;
    rm.on_trade_closed(t3);
    DU_EXPECT(!rm.is_kill_switch_active());

    // Lose another 250 INR -> peak-to-trough drawdown = 500 INR -> Kill-switch engages!
    core::TradeAuditEntry t4{};
    t4.exit_time_ns = day1_ns + 180'000'000'000ULL;
    t4.net_pnl_inr = -250.0;
    rm.on_trade_closed(t4);
    DU_EXPECT(rm.is_kill_switch_active());

    const auto sig = make_valid_signal();
    const auto [qty1, rej1] = rm.evaluate_and_size_order(sig, day1_ns + 240'000'000'000ULL);
    DU_EXPECT(qty1 == 0);
    DU_EXPECT(rej1 == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

    // Next IST trading day automatically resets daily drawdown & kill-switch
    const auto [qty2, rej2] = rm.evaluate_and_size_order(sig, day2_ns);
    DU_EXPECT(!rm.is_kill_switch_active());
    DU_EXPECT(rej2 == risk::RiskRejection::NONE);
    DU_EXPECT(qty2 == 33);

    std::cout << "[PASS] test_net_drawdown_killswitch_and_day_rollover\n";
}

void test_time_windows_circuit_macro_and_squareoff() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const auto sig = make_valid_signal();

    // Opening range chop (09:20 IST) -> rejected
    {
        const auto [q, r] = rm.evaluate_and_size_order(sig, make_ist_ns("2026-11-04", "09:20:00"));
        DU_EXPECT(q == 0 && r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }
    // Midday chop (12:15 IST) -> rejected
    {
        const auto [q, r] = rm.evaluate_and_size_order(sig, make_ist_ns("2026-11-04", "12:15:00"));
        DU_EXPECT(q == 0 && r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }
    // Afternoon window (14:00 IST) -> accepted
    {
        const auto [q, r] = rm.evaluate_and_size_order(sig, make_ist_ns("2026-11-04", "14:00:00"));
        DU_EXPECT(q == 33 && r == risk::RiskRejection::NONE);
    }
    // Past 15:05 IST cutoff -> rejected
    {
        const auto [q, r] = rm.evaluate_and_size_order(sig, make_ist_ns("2026-11-04", "15:06:00"));
        DU_EXPECT(q == 0 && r == risk::RiskRejection::AUTO_SQUAREOFF_TIME_REACHED);
    }
    // Circuit proximity shield inside RiskManager -> rejected
    {
        const auto [q, r] = rm.evaluate_and_size_order(sig, make_ist_ns("2026-11-04", "10:00:00"), false, true);
        DU_EXPECT(q == 0 && r == risk::RiskRejection::CIRCUIT_LIMIT_PROXIMITY);
    }
    // Macro event freeze (RBI Policy window 2026-12-04 09:45-11:30 IST) -> rejected
    {
        core::MacroCalendar cal;
        cal.add_event({
            "RBI Monetary Policy Statement",
            make_ist_ns("2026-12-04", "09:45:00"),
            make_ist_ns("2026-12-04", "11:30:00"),
        });
        const uint64_t rbi_tick = make_ist_ns("2026-12-04", "10:15:00");
        DU_EXPECT(cal.is_frozen(rbi_tick));
        const auto [q, r] = rm.evaluate_and_size_order(sig, rbi_tick, cal.is_frozen(rbi_tick), false);
        DU_EXPECT(q == 0 && r == risk::RiskRejection::MACRO_EVENT_FREEZE);
    }
    // 15:05 IST Auto-Squareoff Shield on active position
    {
        core::Position pos{};
        pos.avg_entry_price = core::to_paise(150.00);
        pos.current_stop_loss = core::to_paise(148.50);
        pos.take_profit = core::to_paise(153.00);
        const auto [should_exit, reason] =
            rm.check_position_rules(pos, core::to_paise(150.40), make_ist_ns("2026-11-04", "15:05:00"));
        DU_EXPECT(should_exit);
        DU_EXPECT(reason == "AUTO_SQUAREOFF_SHIELD_1505");
    }
    // Statutory friction veto when target gain < 2.5x fees
    {
        core::TradingSignal tiny_tp = sig;
        tiny_tp.take_profit = core::to_paise(150.05); // Only 5 paise target on 150 INR stock
        const auto [q, r] = rm.evaluate_and_size_order(tiny_tp, make_ist_ns("2026-11-04", "10:00:00"));
        DU_EXPECT(q == 0 && r == risk::RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO);
    }

    std::cout << "[PASS] test_time_windows_circuit_macro_and_squareoff\n";
}

void test_breakeven_trailing_stop() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    core::Position pos{};
    pos.avg_entry_price = core::to_paise(150.00);
    pos.current_stop_loss = core::to_paise(148.50);
    pos.take_profit = core::to_paise(153.00);

    const uint64_t morning_ns = make_ist_ns("2026-11-04", "09:45:00");

    // +0.53% gain -> not yet at +1.0% breakeven trigger
    const auto [exit1, reason1] = rm.check_position_rules(pos, core::to_paise(150.80), morning_ns);
    DU_EXPECT(!exit1 && !pos.is_breakeven_set);

    // +1.06% gain -> ratchets SL above entry aligned to 5-paise NSE tick (150.25 INR)
    const auto [exit2, reason2] = rm.check_position_rules(pos, core::to_paise(151.60), morning_ns);
    DU_EXPECT(!exit2 && pos.is_breakeven_set);
    DU_EXPECT(pos.current_stop_loss == core::to_paise(150.25));

    // Pullback to 150.20 triggers zero-loss breakeven exit
    const auto [exit3, reason3] = rm.check_position_rules(pos, core::to_paise(150.20), morning_ns);
    DU_EXPECT(exit3);
    DU_EXPECT(reason3 == "BREAKEVEN_STOP_TRIGGERED");

    std::cout << "[PASS] test_breakeven_trailing_stop\n";
}

void test_risk_manager_multi_day_rollover_invariants() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t day1_ns = make_ist_ns("2026-11-04", "10:00:00");
    const uint64_t day2_ns = make_ist_ns("2026-11-05", "09:45:00");

    // Day 1 trade loss of 400 INR
    core::TradeAuditEntry t1{};
    t1.exit_time_ns = day1_ns;
    t1.net_pnl_inr = -400.0;
    rm.on_trade_closed(t1);

    DU_EXPECT_NEAR(rm.current_capital_inr(), 9600.0, 0.01);
    DU_EXPECT(rm.daily_drawdown_paise() == 40000);
    DU_EXPECT(rm.consecutive_losses() == 1);

    // Rollover to Day 2
    const bool rolled = rm.check_day_rollover(day2_ns);
    DU_EXPECT(rolled);

    // Daily metrics reset
    DU_EXPECT(rm.daily_drawdown_paise() == 0);
    DU_EXPECT(rm.consecutive_losses() == 0);
    DU_EXPECT(!rm.is_kill_switch_active());

    // Cumulative capital preserved
    DU_EXPECT_NEAR(rm.current_capital_inr(), 9600.0, 0.01);
    DU_EXPECT_NEAR(rm.day_starting_capital_inr(), 9600.0, 0.01);

    // Evaluate order on Day 2 in morning window
    const auto sig = make_valid_signal();
    const auto [qty, rej] = rm.evaluate_and_size_order(sig, day2_ns);
    DU_EXPECT(rej == risk::RiskRejection::NONE);
    DU_EXPECT(qty == 33);

    std::cout << "[PASS] test_risk_manager_multi_day_rollover_invariants\n";
}

void test_persistent_cumulative_pilot_stopout_latch() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t day1_ns = make_ist_ns("2026-11-04", "10:00:00");
    const uint64_t day2_ns = make_ist_ns("2026-11-05", "10:00:00");
    const uint64_t day3_ns = make_ist_ns("2026-11-06", "10:00:00");

    // Cumulative loss of 1,600 INR across days (Day 1: -400, Day 2: -400, Day 3: -800)
    core::TradeAuditEntry t1{};
    t1.exit_time_ns = day1_ns;
    t1.net_pnl_inr = -400.0;
    rm.on_trade_closed(t1);
    DU_EXPECT(!rm.is_pilot_stopout_active());

    rm.check_day_rollover(day2_ns);
    core::TradeAuditEntry t2{};
    t2.exit_time_ns = day2_ns;
    t2.net_pnl_inr = -400.0;
    rm.on_trade_closed(t2);
    DU_EXPECT(!rm.is_pilot_stopout_active());

    rm.check_day_rollover(day3_ns);
    core::TradeAuditEntry t3{};
    t3.exit_time_ns = day3_ns;
    t3.net_pnl_inr = -800.0; // Total loss = 1600 INR -> capital = 8400 INR <= 8500 INR
    rm.on_trade_closed(t3);

    // Permanent cumulative pilot stop-out latch must trigger!
    DU_EXPECT(rm.is_pilot_stopout_active());

    const auto sig = make_valid_signal();
    const auto [q1, r1] = rm.evaluate_and_size_order(sig, day3_ns);
    DU_EXPECT(q1 == 0);
    DU_EXPECT(r1 == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

    // Rollover to Day 4 must NOT reset the permanent cumulative pilot stop-out latch!
    const uint64_t day4_ns = make_ist_ns("2026-11-07", "10:00:00");
    rm.check_day_rollover(day4_ns);
    DU_EXPECT(rm.is_pilot_stopout_active());

    const auto [q2, r2] = rm.evaluate_and_size_order(sig, day4_ns);
    DU_EXPECT(q2 == 0);
    DU_EXPECT(r2 == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

    std::cout << "[PASS] test_persistent_cumulative_pilot_stopout_latch\n";
}

void test_kill_switch_history_archiving_and_drawdown_tracking() {
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t day1_ns = make_ist_ns("2026-11-04", "10:00:00");
    const uint64_t day2_ns = make_ist_ns("2026-11-05", "10:00:00");

    // Day 1
    core::TradeAuditEntry t1{};
    t1.exit_time_ns = day1_ns;
    t1.net_pnl_inr = 200.0;
    rm.on_trade_closed(t1);

    core::TradeAuditEntry t2{};
    t2.exit_time_ns = day1_ns + 60'000'000'000ULL;
    t2.net_pnl_inr = -150.0;
    rm.on_trade_closed(t2);

    DU_EXPECT(rm.day_peak_capital_inr() == 10200.0);
    DU_EXPECT(rm.day_trough_capital_inr() == 10000.0);
    DU_EXPECT(rm.max_intraday_drawdown_paise() == 15000); // 150 INR
    DU_EXPECT(rm.day_trade_count() == 2);
    DU_EXPECT(rm.kill_switch_history().empty());

    // Rollover to Day 2 archives Day 1
    const bool rolled = rm.check_day_rollover(day2_ns);
    DU_EXPECT(rolled);
    DU_EXPECT(rm.kill_switch_history().size() == 1);

    const auto& d1_audit = rm.kill_switch_history()[0];
    DU_EXPECT(d1_audit.date_ist == "2026-11-04");
    DU_EXPECT_NEAR(d1_audit.starting_capital_inr, 10000.0, 1e-6);
    DU_EXPECT_NEAR(d1_audit.peak_capital_inr, 10200.0, 1e-6);
    DU_EXPECT_NEAR(d1_audit.trough_capital_inr, 10000.0, 1e-6);
    DU_EXPECT_NEAR(d1_audit.max_drawdown_inr, 150.0, 1e-6);
    DU_EXPECT(d1_audit.status == risk::KillSwitchStatus::CLEAR);
    DU_EXPECT(d1_audit.trade_count == 2);

    // Day 2 trade that trips the ₹500 kill-switch
    core::TradeAuditEntry t3{};
    t3.exit_time_ns = day2_ns + 30'000'000'000ULL;
    t3.net_pnl_inr = -550.0;
    rm.on_trade_closed(t3);

    DU_EXPECT(rm.is_kill_switch_active());
    DU_EXPECT(rm.day_trade_count() == 1);

    // Finalize session at EOF (archives Day 2)
    rm.finalize_session();
    DU_EXPECT(rm.kill_switch_history().size() == 2);

    const auto& d2_audit = rm.kill_switch_history()[1];
    DU_EXPECT(d2_audit.date_ist == "2026-11-05");
    DU_EXPECT_NEAR(d2_audit.starting_capital_inr, 10050.0, 1e-6);
    DU_EXPECT(d2_audit.status == risk::KillSwitchStatus::TRIPPED);
    DU_EXPECT(d2_audit.tripped_time_ns == t3.exit_time_ns);
    DU_EXPECT(d2_audit.trade_count == 1);

    // Calling finalize_session again must be idempotent
    rm.finalize_session();
    DU_EXPECT(rm.kill_switch_history().size() == 2);

    std::cout << "[PASS] test_kill_switch_history_archiving_and_drawdown_tracking\n";
}

int main() {
    std::cout << "Running RiskManager tests...\n";
    test_risk_sizing_and_caps();
    test_net_drawdown_killswitch_and_day_rollover();
    test_time_windows_circuit_macro_and_squareoff();
    test_breakeven_trailing_stop();
    test_risk_manager_multi_day_rollover_invariants();
    test_persistent_cumulative_pilot_stopout_latch();
    test_kill_switch_history_archiving_and_drawdown_tracking();
    std::cout << "All RiskManager tests passed!\n";
    return 0;
}
