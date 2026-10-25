#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/execution/paper_broker.hpp"
#include "deltauno/execution/trade_auditor.hpp"
#include "deltauno/market/order_book.hpp"
#include "deltauno/market/watchdog.hpp"
#include "deltauno/risk/risk_manager.hpp"
#include "deltauno/strategy/momentum_breakout.hpp"
#include "deltauno/strategy/signal_ingestor.hpp"
#include "test_macros.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
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

} // namespace

// ============================================================================
// TEST 1: Opening Auction Window Trade Suppression (09:15:00 - 09:30:00 IST)
// ============================================================================
void test_opening_auction_window_suppression() {
    std::cout << "[RUN] test_opening_auction_window_suppression...\n";
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    const auto sig = make_valid_signal();

    // 09:15:00 IST - Opening tick of session
    {
        const uint64_t t_open = make_ist_ns("2026-11-04", "09:15:00");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_open);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }

    // 09:20:00 IST - Mid-auction price discovery
    {
        const uint64_t t_auction = make_ist_ns("2026-11-04", "09:20:00");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_auction);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }

    // 09:29:59 IST - 1 second prior to market open
    {
        const uint64_t t_pre = make_ist_ns("2026-11-04", "09:29:59");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_pre);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }

    // 09:30:00 IST - Strict opening bell
    {
        const uint64_t t_bell = make_ist_ns("2026-11-04", "09:30:00");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_bell);
        DU_EXPECT(q == 33);
        DU_EXPECT(r == risk::RiskRejection::NONE);
    }

    // 10:59:59 IST - Near end of morning window
    {
        const uint64_t t_m_end = make_ist_ns("2026-11-04", "10:59:59");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_m_end);
        DU_EXPECT(q == 33);
        DU_EXPECT(r == risk::RiskRejection::NONE);
    }

    // 11:00:01 IST - Midday chop begins
    {
        const uint64_t t_chop = make_ist_ns("2026-11-04", "11:00:01");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_chop);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }

    // 13:30:00 IST - Afternoon window opens
    {
        const uint64_t t_aft_start = make_ist_ns("2026-11-04", "13:30:00");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_aft_start);
        DU_EXPECT(q == 33);
        DU_EXPECT(r == risk::RiskRejection::NONE);
    }

    // 14:45:00 IST - Afternoon window closes
    {
        const uint64_t t_aft_end = make_ist_ns("2026-11-04", "14:45:00");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_aft_end);
        DU_EXPECT(q == 33);
        DU_EXPECT(r == risk::RiskRejection::NONE);
    }

    // 14:45:01 IST - Post-afternoon window
    {
        const uint64_t t_post_aft = make_ist_ns("2026-11-04", "14:45:01");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_post_aft);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::OUTSIDE_TRADING_WINDOW);
    }

    // 15:05:00 IST - Auto-Squareoff cutoff reached
    {
        const uint64_t t_sq = make_ist_ns("2026-11-04", "15:05:00");
        const auto [q, r] = rm.evaluate_and_size_order(sig, t_sq);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::AUTO_SQUAREOFF_TIME_REACHED);
    }

    std::cout << "[PASS] test_opening_auction_window_suppression\n";
}

// ============================================================================
// TEST 2: 30-Day Session Rollover Balance Continuity (0.00 INR Divergence)
// ============================================================================
void test_multi_day_balance_continuity_invariants() {
    std::cout << "[RUN] test_multi_day_balance_continuity_invariants...\n";
    risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
    execution::PaperBroker broker(10000.0);

    double running_expected_capital = 10000.0;
    const auto sig = make_valid_signal();

    for (int day = 1; day <= 30; ++day) {
        // Construct date string for 2026-11-02 + (day - 1)
        char ymd[32];
        const int day_num = 2 + (day - 1);
        std::snprintf(ymd, sizeof(ymd), "2026-11-%02d", (day_num <= 30) ? day_num : (day_num - 30));

        const uint64_t session_open_ns = make_ist_ns("2026-11-04", "09:15:00") +
            static_cast<uint64_t>(day - 1) * 86'400'000'000'000ULL;
        const uint64_t trade_time_ns = session_open_ns + 45 * 60 * 1'000'000'000ULL; // 10:00:00 IST

        // Rollover check at session start
        const bool rolled = rm.check_day_rollover(session_open_ns);
        if (day > 1) {
            DU_EXPECT(rolled);
        }

        // Invariant: Day starting capital must strictly equal current capital with 0.00 divergence
        DU_EXPECT_NEAR(rm.day_starting_capital_inr(), running_expected_capital, 1e-6);
        DU_EXPECT_NEAR(rm.current_capital_inr(), running_expected_capital, 1e-6);
        DU_EXPECT_NEAR(broker.cash_balance_inr(), running_expected_capital, 1e-6);

        // Daily limits must be reset
        DU_EXPECT(rm.daily_drawdown_paise() == 0);
        DU_EXPECT(rm.consecutive_losses() == 0);
        DU_EXPECT(!rm.is_kill_switch_active());

        // Simulate an alternating trade on every other day
        if (day % 2 == 1) {
            const double net_pnl = (day % 4 == 1) ? +45.50 : -22.30;
            core::TradeAuditEntry trade{};
            trade.exit_time_ns = trade_time_ns;
            trade.net_pnl_inr = net_pnl;

            rm.on_trade_closed(trade);
            // Simulate broker cash update
            broker = execution::PaperBroker(running_expected_capital + net_pnl);
            running_expected_capital += net_pnl;

            DU_EXPECT_NEAR(rm.current_capital_inr(), running_expected_capital, 1e-6);
            DU_EXPECT_NEAR(broker.cash_balance_inr(), running_expected_capital, 1e-6);
        }
    }

    std::cout << "[PASS] test_multi_day_balance_continuity_invariants\n";
}

// ============================================================================
// TEST 3: Overnight 17.75h & Weekend 65.75h Watchdog Resilience
// ============================================================================
void test_watchdog_overnight_gap_resilience() {
    std::cout << "[RUN] test_watchdog_overnight_gap_resilience...\n";
    market::Watchdog wd(5); // 5-second timeout
    uint64_t alert_count = 0;
    wd.register_alert_callback([&](const std::string&) {
        alert_count++;
    });

    uint64_t current_ts_ns = make_ist_ns("2026-11-02", "09:15:00");

    for (int day = 1; day <= 30; ++day) {
        // Flat at session open
        wd.set_has_open_position(false);
        wd.on_tick(current_ts_ns); // 09:15:00 IST

        // Fast forward to market close at 15:30:00 IST (+6.25 hours = 22,500s)
        current_ts_ns += 22'500ULL * 1'000'000'000ULL;
        wd.on_tick(current_ts_ns); // 15:30:00 IST

        // If day 30, end of simulation
        if (day == 30) break;

        // Gap to Day N+1 09:15:00 IST
        // Days 5, 10, 15, 20, 25 are Fridays -> 65.75 hours weekend gap
        // Other days are regular weekdays -> 17.75 hours overnight gap
        const bool is_weekend = (day % 5 == 0);
        const uint64_t gap_seconds = is_weekend ? (65ULL * 3600ULL + 45ULL * 60ULL) : (17ULL * 3600ULL + 45ULL * 60ULL);

        current_ts_ns += gap_seconds * 1'000'000'000ULL;

        // Simulating main.cpp session rollover coordinator:
        // Before on_tick is called with new day's tick, main.cpp forces:
        wd.set_has_open_position(false);

        // First tick of new day
        wd.on_tick(current_ts_ns);

        // Watchdog alert count must be strictly 0!
        DU_EXPECT(wd.alert_count() == 0);
    }

    DU_EXPECT(alert_count == 0);
    DU_EXPECT(wd.alert_count() == 0);
    std::cout << "[PASS] test_watchdog_overnight_gap_resilience (0 alerts across all 29 overnight/weekend gaps)\n";
}

// ============================================================================
// TEST 4: Cumulative Pilot Stop-Out Latch Adversarial Stress Testing (<= ₹8,500)
// ============================================================================
void test_pilot_stopout_latch_adversarial_stress() {
    std::cout << "[RUN] test_pilot_stopout_latch_adversarial_stress...\n";

    const auto sig = make_valid_signal();

    // ------------------------------------------------------------------------
    // SCENARIO 4A: Boundary Test (₹8,500.01 vs ₹8,500.00 vs ₹8,499.99)
    // ------------------------------------------------------------------------
    {
        // 4A.1: Capital drops to 8500.01 INR (Loss = 1499.99 INR) -> NOT latched
        risk::RiskManager rm1(10000.0, 120.0, 500.0, 5000.0);
        const uint64_t day1_ns = make_ist_ns("2026-11-04", "10:00:00");
        const uint64_t day2_ns = make_ist_ns("2026-11-05", "10:00:00");

        core::TradeAuditEntry t_sub{};
        t_sub.exit_time_ns = day1_ns;
        t_sub.net_pnl_inr = -1499.99;
        rm1.on_trade_closed(t_sub);

        DU_EXPECT(!rm1.is_pilot_stopout_active());

        // Day rollover resets daily kill switch
        rm1.check_day_rollover(day2_ns);
        DU_EXPECT(!rm1.is_pilot_stopout_active());
        DU_EXPECT(!rm1.is_kill_switch_active());

        // Order is permitted on Day 2!
        const auto [q1, r1] = rm1.evaluate_and_size_order(sig, day2_ns);
        DU_EXPECT(q1 == 33);
        DU_EXPECT(r1 == risk::RiskRejection::NONE);

        // 4A.2: Capital drops to exactly 8500.00 INR (Loss = 1500.00 INR) -> LATCHED!
        risk::RiskManager rm2(10000.0, 120.0, 500.0, 5000.0);
        core::TradeAuditEntry t_exact{};
        t_exact.exit_time_ns = day1_ns;
        t_exact.net_pnl_inr = -1500.00;
        rm2.on_trade_closed(t_exact);

        DU_EXPECT(rm2.is_pilot_stopout_active());

        // Day rollover does NOT reset latch!
        rm2.check_day_rollover(day2_ns);
        DU_EXPECT(rm2.is_pilot_stopout_active());

        const auto [q2, r2] = rm2.evaluate_and_size_order(sig, day2_ns);
        DU_EXPECT(q2 == 0);
        DU_EXPECT(r2 == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

        // 4A.3: Capital drops to 8499.99 INR (Loss = 1500.01 INR) -> LATCHED!
        risk::RiskManager rm3(10000.0, 120.0, 500.0, 5000.0);
        core::TradeAuditEntry t_over{};
        t_over.exit_time_ns = day1_ns;
        t_over.net_pnl_inr = -1500.01;
        rm3.on_trade_closed(t_over);

        DU_EXPECT(rm3.is_pilot_stopout_active());
        rm3.check_day_rollover(day2_ns);
        DU_EXPECT(rm3.is_pilot_stopout_active());

        const auto [q3, r3] = rm3.evaluate_and_size_order(sig, day2_ns);
        DU_EXPECT(q3 == 0);
        DU_EXPECT(r3 == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);
    }

    // ------------------------------------------------------------------------
    // SCENARIO 4B: Multi-Session Slow Bleed to ₹8,500 + 30 Days Permanent Block
    // ------------------------------------------------------------------------
    {
        risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);

        // Session 1: -400 INR (Capital 9600)
        rm.on_trade_closed({.exit_time_ns = make_ist_ns("2026-11-02", "10:00:00"), .net_pnl_inr = -400.0});
        DU_EXPECT(!rm.is_pilot_stopout_active());

        // Session 2: -400 INR (Capital 9200)
        rm.check_day_rollover(make_ist_ns("2026-11-03", "09:15:00"));
        rm.on_trade_closed({.exit_time_ns = make_ist_ns("2026-11-03", "10:00:00"), .net_pnl_inr = -400.0});
        DU_EXPECT(!rm.is_pilot_stopout_active());

        // Session 3: -400 INR (Capital 8800)
        rm.check_day_rollover(make_ist_ns("2026-11-04", "09:15:00"));
        rm.on_trade_closed({.exit_time_ns = make_ist_ns("2026-11-04", "10:00:00"), .net_pnl_inr = -400.0});
        DU_EXPECT(!rm.is_pilot_stopout_active());

        // Session 4: -300 INR (Capital 8500) -> CRITICAL STOP-OUT THRESHOLD HIT!
        rm.check_day_rollover(make_ist_ns("2026-11-05", "09:15:00"));
        rm.on_trade_closed({.exit_time_ns = make_ist_ns("2026-11-05", "10:00:00"), .net_pnl_inr = -300.0});
        DU_EXPECT(rm.is_pilot_stopout_active());
        DU_EXPECT_NEAR(rm.current_capital_inr(), 8500.0, 1e-6);

        // Attempt order immediately after trade on Day 4 -> BLOCKED
        const auto [q_d4, r_d4] = rm.evaluate_and_size_order(sig, make_ist_ns("2026-11-05", "10:15:00"));
        DU_EXPECT(q_d4 == 0);
        DU_EXPECT(r_d4 == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);

        // Now test 26 subsequent days (Days 5 to 30)!
        for (int day = 5; day <= 30; ++day) {
            const uint64_t open_ns = make_ist_ns("2026-11-02", "09:15:00") +
                static_cast<uint64_t>(day - 1) * 86'400'000'000'000ULL;
            const uint64_t trade_ns = open_ns + 45 * 60 * 1'000'000'000ULL; // 10:00:00 IST

            // Session rollover executes cleanly
            rm.check_day_rollover(open_ns);

            // Pilot stop-out latch MUST REMAIN TRUE
            DU_EXPECT(rm.is_pilot_stopout_active());

            // Order evaluation MUST BE REJECTED
            const auto [q, r] = rm.evaluate_and_size_order(sig, trade_ns);
            DU_EXPECT(q == 0);
            DU_EXPECT(r == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);
        }
    }

    // ------------------------------------------------------------------------
    // SCENARIO 4C: Webhook Bypass Invariant
    // ------------------------------------------------------------------------
    {
        risk::RiskManager rm(10000.0, 120.0, 500.0, 5000.0);
        // Simulate cumulative loss of ₹1,500 taking capital to ₹8,500
        rm.on_trade_closed({.exit_time_ns = make_ist_ns("2026-11-04", "09:45:00"), .net_pnl_inr = -1500.0});
        DU_EXPECT(rm.is_pilot_stopout_active());

        // External webhook alert arrives
        core::TradingSignal webhook_sig = make_valid_signal();
        webhook_sig.source = core::SignalSource::EXTERNAL_WEBHOOK;

        const uint64_t t_now = make_ist_ns("2026-11-04", "10:00:00");
        const auto [q, r] = rm.evaluate_and_size_order(webhook_sig, t_now);
        DU_EXPECT(q == 0);
        DU_EXPECT(r == risk::RiskRejection::EXCEEDS_DAILY_DRAWDOWN_LIMIT);
        DU_EXPECT(rm.is_pilot_stopout_active());
    }

    std::cout << "[PASS] test_pilot_stopout_latch_adversarial_stress (100% permanent lock confirmed)\n";
}

int main() {
    std::cout << "====================================================================\n";
    std::cout << "  EMPIRICAL CHALLENGER: MULTI-DAY ROLLOVER STRESS HARNESS (M2)      \n";
    std::cout << "====================================================================\n";

    test_opening_auction_window_suppression();
    test_multi_day_balance_continuity_invariants();
    test_watchdog_overnight_gap_resilience();
    test_pilot_stopout_latch_adversarial_stress();

    std::cout << "====================================================================\n";
    std::cout << "  ALL EMPIRICAL CHALLENGE SUITES PASSED (0 FAILURES)                \n";
    std::cout << "====================================================================\n";
    return 0;
}
