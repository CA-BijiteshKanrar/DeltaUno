#include "tradeuno/risk/risk_manager.hpp"
#include <cassert>
#include <iostream>

using namespace tradeuno;

void test_risk_sizing_and_caps() {
    risk::RiskManager rm(1000.0, 12.0, 50.0, 500.0);

    // 09:45 IST timestamp: (09:45:00 IST = 04:15:00 UTC = 15300s from midnight UTC)
    // 1730002500000000000ULL
    uint64_t morning_ist_ns = (1730000000ULL - (1730000000ULL % 86400) + 15300ULL) * 1'000'000'000ULL;

    core::TradingSignal sig{};
    sig.target_entry = core::to_paise(150.00);
    sig.stop_loss = core::to_paise(147.00); // 3.00 INR risk per share
    sig.take_profit = core::to_paise(155.00);

    // Expected quantity: floor(12 / 3) = 4 shares. 4 * 150 = 600 > 500 max pos value!
    // So qty capped by capital: floor(500 / 150) = 3 shares!
    // 3 shares * 3 INR risk = 9 INR risk <= 12 INR cap!
    auto [qty, rejection] = rm.evaluate_and_size_order(sig, morning_ist_ns);
    assert(rejection == risk::RiskRejection::NONE);
    assert(qty == 3);

    std::cout << "[PASS] test_risk_sizing_and_caps (Sized to " << qty << " shares)\n";
}

void test_daily_drawdown_killswitch() {
    risk::RiskManager rm(1000.0, 12.0, 50.0, 500.0);

    // Simulate 4 losing trades of 12.50 INR each = 50 INR loss
    core::TradeAuditEntry t1{};
    t1.net_pnl_inr = -25.0;
    rm.on_trade_closed(t1);
    assert(!rm.is_kill_switch_active());

    core::TradeAuditEntry t2{};
    t2.net_pnl_inr = -25.0;
    rm.on_trade_closed(t2);
    assert(rm.is_kill_switch_active()); // Total loss = -50 INR -> Kill switch triggers!

    // Subsequent order must be rejected
    uint64_t morning_ist_ns = (1730000000ULL - (1730000000ULL % 86400) + 15300ULL) * 1'000'000'000ULL;
    core::TradingSignal sig{};
    sig.target_entry = core::to_paise(150.00);
    sig.stop_loss = core::to_paise(148.00);
    sig.take_profit = core::to_paise(155.00);

    auto [qty, rejection] = rm.evaluate_and_size_order(sig, morning_ist_ns);
    assert(qty == 0);
    assert(rejection == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

    std::cout << "[PASS] test_daily_drawdown_killswitch\n";
}

void test_breakeven_trailing_stop() {
    risk::RiskManager rm(1000.0, 12.0, 50.0, 500.0);
    core::Position pos{};
    pos.avg_entry_price = core::to_paise(150.00);
    pos.current_stop_loss = core::to_paise(148.50); // initial 1% SL
    pos.take_profit = core::to_paise(153.00);

    uint64_t morning_ist_ns = (1730000000ULL - (1730000000ULL % 86400) + 15300ULL) * 1'000'000'000ULL;

    // Price moves to 150.80 (+0.53% gain) -> Breakeven not triggered yet
    auto [exit1, reason1] = rm.check_position_rules(pos, core::to_paise(150.80), morning_ist_ns);
    assert(!exit1);
    assert(!pos.is_breakeven_set);

    // Price moves to 151.60 (+1.06% gain >= 1.0%) -> Breakeven MUST trigger!
    auto [exit2, reason2] = rm.check_position_rules(pos, core::to_paise(151.60), morning_ist_ns);
    assert(!exit2);
    assert(pos.is_breakeven_set);
    // Stop loss should now be above entry price (150.00 + 0.15% = 150.22 paise)
    assert(pos.current_stop_loss > pos.avg_entry_price);

    // Now price drops back down to 150.20 -> Breakeven stop should trigger exit with ZERO loss!
    auto [exit3, reason3] = rm.check_position_rules(pos, core::to_paise(150.20), morning_ist_ns);
    assert(exit3);
    assert(reason3 == "BREAKEVEN_STOP_TRIGGERED");

    std::cout << "[PASS] test_breakeven_trailing_stop\n";
}

int main() {
    std::cout << "Running RiskManager tests...\n";
    test_risk_sizing_and_caps();
    test_daily_drawdown_killswitch();
    test_breakeven_trailing_stop();
    std::cout << "All RiskManager tests passed!\n";
    return 0;
}
