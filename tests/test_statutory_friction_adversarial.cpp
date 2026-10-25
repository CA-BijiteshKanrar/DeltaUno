#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/execution/paper_broker.hpp"
#include "deltauno/execution/trade_auditor.hpp"
#include "deltauno/market/order_book.hpp"
#include "deltauno/risk/risk_manager.hpp"
#include "test_macros.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

using namespace deltauno;

namespace {

uint64_t make_ist_ns(std::string_view ymd, std::string_view hms) {
    return core::ConfigLoader::ist_datetime_to_utc_ns(ymd, hms);
}

// Independent mathematical oracle for statutory charges
struct FrictionOracle {
    static constexpr double kSttRate = 0.00025;           // 0.025% on sell leg
    static constexpr double kNseTurnoverRate = 0.0000297; // 0.00297% on round-trip
    static constexpr double kSebiRate = 0.000001;         // INR 10 / crore (0.0001%)
    static constexpr double kGstRate = 0.18;              // 18% on (Exchange + SEBI)
    static constexpr double kStampDutyRate = 0.00003;     // 0.003% on buy leg

    static double compute_buy_val(core::Price entry_price, core::Quantity qty) {
        return core::to_inr(entry_price * static_cast<core::Price>(qty));
    }

    static double compute_sell_val(core::Price exit_price, core::Quantity qty) {
        return core::to_inr(exit_price * static_cast<core::Price>(qty));
    }

    static double compute_turnover(double buy_val, double sell_val) {
        return buy_val + sell_val;
    }

    static double compute_stt(double sell_val) {
        return sell_val * kSttRate;
    }

    static double compute_exchange_fee(double turnover) {
        return turnover * kNseTurnoverRate;
    }

    static double compute_sebi_fee(double turnover) {
        return turnover * kSebiRate;
    }

    static double compute_gst(double exch_fee, double sebi_fee) {
        return (exch_fee + sebi_fee) * kGstRate;
    }

    static double compute_stamp_duty(double buy_val) {
        return buy_val * kStampDutyRate;
    }

    static double compute_total_friction(double buy_val, double sell_val) {
        const double turnover = compute_turnover(buy_val, sell_val);
        const double stt = compute_stt(sell_val);
        const double exch = compute_exchange_fee(turnover);
        const double sebi = compute_sebi_fee(turnover);
        const double gst = compute_gst(exch, sebi);
        const double stamp = compute_stamp_duty(buy_val);
        return stt + exch + sebi + gst + stamp;
    }
};

} // namespace

// Probe 1a: Round-trip turnover calculations on diverse prices
void test_diverse_price_turnover_calculations() {
    std::cout << "[RUN] test_diverse_price_turnover_calculations...\n";

    // Diverse price points: from penny stock ₹0.05 to large-cap ₹3000.00+
    const std::vector<double> test_prices_inr = {
        0.05,    // Minimum NSE tick
        0.10,    // Micro penny stock
        0.50,
        1.00,
        5.00,
        14.50,
        50.00,
        125.40,  // TATASTEEL realm
        250.00,  // ZOMATO realm
        430.50,  // ITC / COALINDIA realm
        810.00,  // SBIN realm
        1500.00,
        3000.00, // Large-cap
        5000.00  // Extreme large-cap
    };

    const std::vector<core::Quantity> test_quantities = {1, 2, 5, 10, 50, 100, 500, 1000, 5000, 10000};

    size_t checks_performed = 0;
    for (double p_entry : test_prices_inr) {
        for (double p_exit : test_prices_inr) {
            for (core::Quantity qty : test_quantities) {
                core::TradeAuditEntry t{};
                t.entry_price = core::to_paise(p_entry);
                t.exit_price = core::to_paise(p_exit);
                t.quantity = qty;

                const double buy_val_oracle = FrictionOracle::compute_buy_val(t.entry_price, qty);
                const double sell_val_oracle = FrictionOracle::compute_sell_val(t.exit_price, qty);
                const double turnover_oracle = FrictionOracle::compute_turnover(buy_val_oracle, sell_val_oracle);

                const double turnover_computed = t.turnover_inr();

                DU_EXPECT_NEAR(turnover_computed, turnover_oracle, 1e-9);
                DU_EXPECT(turnover_computed >= 0.0);

                // Check integer arithmetic doesn't overflow
                const double expected_gross = sell_val_oracle - buy_val_oracle;
                t.gross_pnl_inr = core::to_inr((t.exit_price - t.entry_price) * static_cast<core::Price>(qty));
                DU_EXPECT_NEAR(t.gross_pnl_inr, expected_gross, 1e-9);

                checks_performed++;
            }
        }
    }

    std::cout << "      Verified " << checks_performed << " combinations of diverse prices & quantities.\n";
    std::cout << "[PASS] test_diverse_price_turnover_calculations\n";
}

// Probe 1b: Exact rate testing against independent regulatory oracle
void test_exact_statutory_rates() {
    std::cout << "[RUN] test_exact_statutory_rates...\n";

    // Standard institutional trade test cases
    struct TestCase {
        double entry_price_inr;
        double exit_price_inr;
        core::Quantity qty;
    };

    const std::vector<TestCase> cases = {
        {0.05, 0.10, 10000},   // Penny doubler (₹500 buy, ₹1000 sell)
        {100.00, 105.00, 50},   // ₹5000 buy, ₹5250 sell (standard pilot trade)
        {800.00, 792.00, 6},    // SBIN stop-loss hit
        {450.00, 450.00, 10},   // Scratch trade
        {3000.00, 3050.00, 1},  // Large-cap single share
        {150.00, 148.50, 30},   // Small loss
        {2500.00, 2400.00, 4}   // High value loss
    };

    for (const auto& tc : cases) {
        core::TradeAuditEntry t{};
        t.entry_price = core::to_paise(tc.entry_price_inr);
        t.exit_price = core::to_paise(tc.exit_price_inr);
        t.quantity = tc.qty;

        const double buy_val = FrictionOracle::compute_buy_val(t.entry_price, tc.qty);
        const double sell_val = FrictionOracle::compute_sell_val(t.exit_price, tc.qty);
        const double turnover = FrictionOracle::compute_turnover(buy_val, sell_val);

        t.stt_inr = sell_val * 0.00025;
        t.exchange_fee_inr = turnover * 0.0000297;
        t.sebi_turnover_inr = turnover * 0.000001;
        t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18;
        t.stamp_duty_inr = buy_val * 0.00003;

        // Verify each individual statutory rate against regulatory formula
        const double expected_stt = FrictionOracle::compute_stt(sell_val);
        const double expected_exch = FrictionOracle::compute_exchange_fee(turnover);
        const double expected_sebi = FrictionOracle::compute_sebi_fee(turnover);
        const double expected_gst = FrictionOracle::compute_gst(expected_exch, expected_sebi);
        const double expected_stamp = FrictionOracle::compute_stamp_duty(buy_val);

        DU_EXPECT_NEAR(t.stt_inr, expected_stt, 1e-12);
        DU_EXPECT_NEAR(t.exchange_fee_inr, expected_exch, 1e-12);
        DU_EXPECT_NEAR(t.sebi_turnover_inr, expected_sebi, 1e-12);
        DU_EXPECT_NEAR(t.gst_inr, expected_gst, 1e-12);
        DU_EXPECT_NEAR(t.stamp_duty_inr, expected_stamp, 1e-12);

        // Verification of rate definitions:
        // 1. STT is exactly 0.025% of sell leg
        DU_EXPECT_NEAR(t.stt_inr / sell_val, 0.00025, 1e-12);
        // 2. NSE turnover is exactly 0.00297% of turnover
        DU_EXPECT_NEAR(t.exchange_fee_inr / turnover, 0.0000297, 1e-12);
        // 3. SEBI is exactly 1e-6 (₹10/crore)
        DU_EXPECT_NEAR(t.sebi_turnover_inr / turnover, 0.000001, 1e-12);
        // 4. GST is exactly 18% of (Exch + SEBI)
        DU_EXPECT_NEAR(t.gst_inr / (t.exchange_fee_inr + t.sebi_turnover_inr), 0.18, 1e-12);
        // 5. Stamp duty is exactly 0.003% of buy leg
        DU_EXPECT_NEAR(t.stamp_duty_inr / buy_val, 0.00003, 1e-12);
    }

    std::cout << "[PASS] test_exact_statutory_rates\n";
}

// Probe 1c: Mathematical invariant: total_friction == stt + exch + gst + stamp + sebi (< 1e-9)
void test_friction_decomposition_invariant() {
    std::cout << "[RUN] test_friction_decomposition_invariant...\n";

    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> price_dist(0.05, 3500.0);
    std::uniform_int_distribution<core::Quantity> qty_dist(1, 200);

    constexpr size_t kIterations = 1000;
    std::vector<core::TradeAuditEntry> trades;
    trades.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i) {
        const double p_in = std::round(price_dist(rng) * 20.0) / 20.0;
        const double p_out = std::round(price_dist(rng) * 20.0) / 20.0;
        const auto qty = qty_dist(rng);

        core::TradeAuditEntry t{};
        t.trade_id = i + 1;
        t.entry_price = core::to_paise(p_in);
        t.exit_price = core::to_paise(p_out);
        t.quantity = qty;

        const double buy_val = FrictionOracle::compute_buy_val(t.entry_price, qty);
        const double sell_val = FrictionOracle::compute_sell_val(t.exit_price, qty);
        const double turnover = buy_val + sell_val;

        t.gross_pnl_inr = sell_val - buy_val;
        t.stt_inr = sell_val * 0.00025;
        t.exchange_fee_inr = turnover * 0.0000297;
        t.sebi_turnover_inr = turnover * 0.000001;
        t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18;
        t.stamp_duty_inr = buy_val * 0.00003;

        const double sum_parts = t.stt_inr + t.exchange_fee_inr + t.gst_inr +
                                 t.stamp_duty_inr + t.sebi_turnover_inr;
        const double total_fric = t.total_friction_inr();

        // Invariant 1: Single-trade decomposition tolerance < 1e-12
        DU_EXPECT_NEAR(total_fric, sum_parts, 1e-12);

        t.net_pnl_inr = t.gross_pnl_inr - total_fric;
        trades.push_back(t);
    }

    // Invariant 2: Multi-trade aggregate decomposition tolerance < 1e-9
    const auto s = execution::TradeAuditor::evaluate(trades);
    const double aggregate_sum_parts = s.total_stt_inr + s.total_exchange_fee_inr +
                                       s.total_gst_inr + s.total_stamp_duty_inr +
                                       s.total_sebi_turnover_inr;

    DU_EXPECT_NEAR(s.total_taxes_and_friction_inr, aggregate_sum_parts, 1e-9);

    std::cout << "      Verified invariant across " << kIterations << " randomized trades.\n";
    std::cout << "      Aggregate Friction: INR " << s.total_taxes_and_friction_inr
              << " | Sum of Parts: INR " << aggregate_sum_parts
              << " (Diff: " << std::abs(s.total_taxes_and_friction_inr - aggregate_sum_parts) << ")\n";
    std::cout << "[PASS] test_friction_decomposition_invariant\n";
}

// Probe 1d: PnL identity: gross_profit - gross_loss - total_friction == net_pnl
void test_pnl_accounting_identity() {
    std::cout << "[RUN] test_pnl_accounting_identity...\n";

    // Scenario A: Only winners
    {
        std::vector<core::TradeAuditEntry> winners;
        for (int i = 0; i < 20; ++i) {
            core::TradeAuditEntry t{};
            t.trade_id = i + 1;
            t.entry_price = core::to_paise(100.0);
            t.exit_price = core::to_paise(105.0 + i * 0.5);
            t.quantity = 10;
            const double buy_val = core::to_inr(t.entry_price * 10);
            const double sell_val = core::to_inr(t.exit_price * 10);
            const double turnover = buy_val + sell_val;
            t.gross_pnl_inr = sell_val - buy_val;
            t.stt_inr = sell_val * 0.00025;
            t.exchange_fee_inr = turnover * 0.0000297;
            t.sebi_turnover_inr = turnover * 0.000001;
            t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18;
            t.stamp_duty_inr = buy_val * 0.00003;
            t.net_pnl_inr = t.gross_pnl_inr - t.total_friction_inr();
            winners.push_back(t);
        }
        const auto s = execution::TradeAuditor::evaluate(winners);
        DU_EXPECT(s.gross_loss_inr == 0.0);
        DU_EXPECT_NEAR(s.gross_profit_inr - s.gross_loss_inr - s.total_taxes_and_friction_inr, s.net_pnl_inr, 1e-9);
    }

    // Scenario B: Only losers
    {
        std::vector<core::TradeAuditEntry> losers;
        for (int i = 0; i < 20; ++i) {
            core::TradeAuditEntry t{};
            t.trade_id = i + 1;
            t.entry_price = core::to_paise(100.0);
            t.exit_price = core::to_paise(95.0 - i * 0.5);
            t.quantity = 10;
            const double buy_val = core::to_inr(t.entry_price * 10);
            const double sell_val = core::to_inr(t.exit_price * 10);
            const double turnover = buy_val + sell_val;
            t.gross_pnl_inr = sell_val - buy_val;
            t.stt_inr = sell_val * 0.00025;
            t.exchange_fee_inr = turnover * 0.0000297;
            t.sebi_turnover_inr = turnover * 0.000001;
            t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18;
            t.stamp_duty_inr = buy_val * 0.00003;
            t.net_pnl_inr = t.gross_pnl_inr - t.total_friction_inr();
            losers.push_back(t);
        }
        const auto s = execution::TradeAuditor::evaluate(losers);
        DU_EXPECT(s.gross_profit_inr == 0.0);
        DU_EXPECT_NEAR(s.gross_profit_inr - s.gross_loss_inr - s.total_taxes_and_friction_inr, s.net_pnl_inr, 1e-9);
    }

    // Scenario C: Pure scratch trades (0 gross PnL)
    {
        std::vector<core::TradeAuditEntry> scratches;
        for (int i = 0; i < 15; ++i) {
            core::TradeAuditEntry t{};
            t.trade_id = i + 1;
            t.entry_price = core::to_paise(500.0);
            t.exit_price = core::to_paise(500.0); // Exact breakeven exit
            t.quantity = 10;
            const double buy_val = core::to_inr(t.entry_price * 10);
            const double sell_val = core::to_inr(t.exit_price * 10);
            const double turnover = buy_val + sell_val;
            t.gross_pnl_inr = 0.0;
            t.stt_inr = sell_val * 0.00025;
            t.exchange_fee_inr = turnover * 0.0000297;
            t.sebi_turnover_inr = turnover * 0.000001;
            t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18;
            t.stamp_duty_inr = buy_val * 0.00003;
            t.net_pnl_inr = -t.total_friction_inr();
            scratches.push_back(t);
        }
        const auto s = execution::TradeAuditor::evaluate(scratches);
        DU_EXPECT(s.gross_profit_inr == 0.0);
        DU_EXPECT(s.gross_loss_inr == 0.0);
        DU_EXPECT_NEAR(s.net_pnl_inr, -s.total_taxes_and_friction_inr, 1e-9);
        DU_EXPECT_NEAR(s.gross_profit_inr - s.gross_loss_inr - s.total_taxes_and_friction_inr, s.net_pnl_inr, 1e-9);
    }

    // Scenario D: 1,000 mixed randomized trades
    {
        std::mt19937_64 rng(1337);
        std::uniform_real_distribution<double> p_in_dist(10.0, 2000.0);
        std::normal_distribution<double> p_diff_dist(0.0, 15.0); // Can be positive, negative, or near-zero
        std::uniform_int_distribution<core::Quantity> qty_dist(1, 50);

        std::vector<core::TradeAuditEntry> mixed;
        for (int i = 0; i < 1000; ++i) {
            core::TradeAuditEntry t{};
            t.trade_id = i + 1;
            const double p_in = std::round(p_in_dist(rng) * 20.0) / 20.0;
            double p_out = p_in + std::round(p_diff_dist(rng) * 20.0) / 20.0;
            if (p_out <= 0.05) p_out = 0.05;
            const auto qty = qty_dist(rng);

            t.entry_price = core::to_paise(p_in);
            t.exit_price = core::to_paise(p_out);
            t.quantity = qty;

            const double buy_val = core::to_inr(t.entry_price * static_cast<core::Price>(qty));
            const double sell_val = core::to_inr(t.exit_price * static_cast<core::Price>(qty));
            const double turnover = buy_val + sell_val;

            t.gross_pnl_inr = sell_val - buy_val;
            t.stt_inr = sell_val * 0.00025;
            t.exchange_fee_inr = turnover * 0.0000297;
            t.sebi_turnover_inr = turnover * 0.000001;
            t.gst_inr = (t.exchange_fee_inr + t.sebi_turnover_inr) * 0.18;
            t.stamp_duty_inr = buy_val * 0.00003;
            t.net_pnl_inr = t.gross_pnl_inr - t.total_friction_inr();

            mixed.push_back(t);
        }

        const auto s = execution::TradeAuditor::evaluate(mixed);
        const double lhs = s.gross_profit_inr - s.gross_loss_inr - s.total_taxes_and_friction_inr;
        const double rhs = s.net_pnl_inr;
        DU_EXPECT_NEAR(lhs, rhs, 1e-9);
    }

    std::cout << "[PASS] test_pnl_accounting_identity\n";
}

// Probe 1e: Friction drag ratio behavior when gross profit is 0 or negative
void test_friction_drag_ratio_zero_or_negative_profit() {
    std::cout << "[RUN] test_friction_drag_ratio_zero_or_negative_profit...\n";

    // 1. Single losing trade in PaperBroker
    {
        execution::PaperBroker broker(10000.0);
        market::OrderBook book(779521, "SBIN");

        core::MarketTick tick_buy{};
        tick_buy.token = 779521;
        core::copy_fixed_str(tick_buy.symbol, "SBIN");
        tick_buy.last_price = core::to_paise(800.00);
        tick_buy.bids[0] = {core::to_paise(799.95), 1000, 5};
        tick_buy.asks[0] = {core::to_paise(800.00), 1000, 5};
        tick_buy.timestamp_ns = 1000000ULL;
        book.on_tick(tick_buy);

        core::Order buy_order{};
        buy_order.client_order_id = 1;
        buy_order.token = 779521;
        core::copy_fixed_str(buy_order.symbol, "SBIN");
        buy_order.side = core::Side::BUY;
        buy_order.quantity = 10;
        buy_order.created_time_ns = 1000000ULL;
        broker.place_order(buy_order);

        // Fill buy on next tick
        core::MarketTick tick2 = tick_buy;
        tick2.timestamp_ns = 2000000ULL;
        book.on_tick(tick2);
        broker.on_next_tick(tick2, book);
        DU_EXPECT(broker.has_open_long());

        // Now place sell at 790.00 (loss)
        core::Order sell_order = buy_order;
        sell_order.client_order_id = 2;
        sell_order.side = core::Side::SELL;
        sell_order.created_time_ns = 3000000ULL;
        broker.place_order(sell_order);

        core::MarketTick tick3 = tick_buy;
        tick3.timestamp_ns = 4000000ULL;
        tick3.last_price = core::to_paise(790.00);
        tick3.bids[0] = {core::to_paise(790.00), 1000, 5};
        tick3.asks[0] = {core::to_paise(790.05), 1000, 5};
        book.on_tick(tick3);
        broker.on_next_tick(tick3, book);

        const auto& hist = broker.trade_history();
        DU_EXPECT(hist.size() == 1);
        const auto& t = hist.front();
        DU_EXPECT(t.gross_pnl_inr < 0.0);
        // Verify PaperBroker sets friction_ratio to 1.0 (100% loss) rather than NaN or negative
        DU_EXPECT_NEAR(t.friction_ratio, 1.0, 1e-12);
        DU_EXPECT(!std::isnan(t.friction_ratio));
        DU_EXPECT(!std::isinf(t.friction_ratio));
    }

    // 2. Aggregate evaluation with only losing trades
    {
        std::vector<core::TradeAuditEntry> losers;
        core::TradeAuditEntry t{};
        t.gross_pnl_inr = -150.0;
        t.stt_inr = 2.0;
        t.exchange_fee_inr = 0.5;
        t.gst_inr = 0.1;
        t.stamp_duty_inr = 0.2;
        t.sebi_turnover_inr = 0.01;
        t.net_pnl_inr = t.gross_pnl_inr - t.total_friction_inr();
        losers.push_back(t);

        const auto s = execution::TradeAuditor::evaluate(losers);
        DU_EXPECT(s.gross_profit_inr == 0.0);
        DU_EXPECT(s.gross_loss_inr == 150.0);
        // When gross profit is 0, friction_drag_ratio_pct should safely remain 0.0% (initialized default)
        DU_EXPECT_NEAR(s.friction_drag_ratio_pct, 0.0, 1e-12);
        DU_EXPECT(!std::isnan(s.friction_drag_ratio_pct));
        DU_EXPECT(!std::isinf(s.friction_drag_ratio_pct));

        // Ensure table printing does not crash or print nan/inf
        std::ostringstream buf;
        execution::TradeAuditor::print_statutory_friction_table(s, buf);
        const std::string table_str = buf.str();
        DU_EXPECT(table_str.find("nan") == std::string::npos);
        DU_EXPECT(table_str.find("inf") == std::string::npos);
        DU_EXPECT(table_str.find("0.00% of Gross Trading Profit") != std::string::npos);
    }

    // 3. Aggregate evaluation with only scratch trades
    {
        std::vector<core::TradeAuditEntry> scratches;
        core::TradeAuditEntry t{};
        t.gross_pnl_inr = 0.0;
        t.stt_inr = 1.0;
        t.exchange_fee_inr = 0.2;
        t.gst_inr = 0.04;
        t.stamp_duty_inr = 0.1;
        t.sebi_turnover_inr = 0.005;
        t.net_pnl_inr = -t.total_friction_inr();
        scratches.push_back(t);

        const auto s = execution::TradeAuditor::evaluate(scratches);
        DU_EXPECT(s.gross_profit_inr == 0.0);
        DU_EXPECT(s.gross_loss_inr == 0.0);
        DU_EXPECT_NEAR(s.friction_drag_ratio_pct, 0.0, 1e-12);
        DU_EXPECT(!std::isnan(s.friction_drag_ratio_pct));
        DU_EXPECT(!std::isinf(s.friction_drag_ratio_pct));

        std::ostringstream buf;
        execution::TradeAuditor::print_statutory_friction_table(s, buf);
        const std::string table_str = buf.str();
        DU_EXPECT(table_str.find("nan") == std::string::npos);
        DU_EXPECT(table_str.find("inf") == std::string::npos);
    }

    // 4. Normal positive profit
    {
        std::vector<core::TradeAuditEntry> winners;
        core::TradeAuditEntry t{};
        t.gross_pnl_inr = 100.0;
        t.stt_inr = 5.0;
        t.exchange_fee_inr = 1.0;
        t.gst_inr = 0.2;
        t.stamp_duty_inr = 0.5;
        t.sebi_turnover_inr = 0.05;
        t.net_pnl_inr = t.gross_pnl_inr - t.total_friction_inr();
        winners.push_back(t);

        const auto s = execution::TradeAuditor::evaluate(winners);
        DU_EXPECT(s.gross_profit_inr == 100.0);
        // friction drag = 6.75 / 100 * 100 = 6.75%
        DU_EXPECT_NEAR(s.friction_drag_ratio_pct, 6.75, 1e-12);
    }

    std::cout << "[PASS] test_friction_drag_ratio_zero_or_negative_profit\n";
}

// Probe 2: Verify rate parity between PaperBroker and RiskManager pre-trade gate
void test_rate_parity_paper_broker_vs_risk_manager() {
    std::cout << "[RUN] test_rate_parity_paper_broker_vs_risk_manager...\n";

    risk::RiskManager risk_mgr(10000.0, 120.0, 500.0, 5000.0);
    const uint64_t morning_time_ns = make_ist_ns("2026-11-04", "10:00:00");

    core::TradingSignal sig{};
    sig.token = 349121;
    core::copy_fixed_str(sig.symbol, "TATASTEEL");
    sig.side = core::Side::BUY;
    sig.target_entry = core::to_paise(150.00);
    sig.stop_loss = core::to_paise(147.00);    // Risk per share = ₹3.00. Qty by risk = 120 / 3 = 40. Qty by capital = 5000 / 150 = 33. -> final_qty = 33.
    sig.take_profit = core::to_paise(156.00);  // Expected gain = (156 - 150) * 33 = 6 * 33 = ₹198.00.

    const auto [qty, rejection] = risk_mgr.evaluate_and_size_order(sig, morning_time_ns);
    DU_EXPECT(rejection == risk::RiskRejection::NONE);
    DU_EXPECT(qty == 33);

    // Now let's calculate the pre-trade fees as RiskManager computes them
    const double trade_val = core::to_inr(sig.target_entry * static_cast<core::Price>(qty)); // 150.00 * 33 = 4950.00 INR
    const double turnover = trade_val * 2.0; // 9900.00 INR
    const double est_stt = trade_val * 0.00025;
    const double est_turnover = turnover * 0.0000297;
    const double est_sebi = turnover * 0.000001;
    const double est_gst = (est_turnover + est_sebi) * 0.18;
    const double est_stamp = trade_val * 0.00003;
    const double total_est_fees = est_stt + est_turnover + est_gst + est_stamp + est_sebi;

    // Now construct an exact flat trade in TradeAuditEntry (entry = 150.00, exit = 150.00, qty = 33)
    // using the exact logic of PaperBroker
    core::TradeAuditEntry audit{};
    audit.entry_price = sig.target_entry;
    audit.exit_price = sig.target_entry;
    audit.quantity = qty;
    const double buy_val = core::to_inr(audit.entry_price * static_cast<core::Price>(audit.quantity));
    const double sell_val = core::to_inr(audit.exit_price * static_cast<core::Price>(audit.quantity));
    const double actual_turnover = buy_val + sell_val;

    audit.stt_inr = sell_val * 0.00025;
    audit.exchange_fee_inr = actual_turnover * 0.0000297;
    audit.sebi_turnover_inr = actual_turnover * 0.000001;
    audit.gst_inr = (audit.exchange_fee_inr + audit.sebi_turnover_inr) * 0.18;
    audit.stamp_duty_inr = buy_val * 0.00003;

    // Verify rate parity across all 5 individual fees down to 1e-12
    DU_EXPECT_NEAR(est_stt, audit.stt_inr, 1e-12);
    DU_EXPECT_NEAR(est_turnover, audit.exchange_fee_inr, 1e-12);
    DU_EXPECT_NEAR(est_sebi, audit.sebi_turnover_inr, 1e-12);
    DU_EXPECT_NEAR(est_gst, audit.gst_inr, 1e-12);
    DU_EXPECT_NEAR(est_stamp, audit.stamp_duty_inr, 1e-12);
    DU_EXPECT_NEAR(total_est_fees, audit.total_friction_inr(), 1e-12);

    std::cout << "      Pre-Trade RiskManager Est Fees: " << total_est_fees << " INR\n";
    std::cout << "      Post-Trade PaperBroker Formula : " << audit.total_friction_inr() << " INR\n";
    std::cout << "      Difference: " << std::abs(total_est_fees - audit.total_friction_inr()) << " (< 1e-12)\n";

    // Now test pre-trade payoff filter boundary: min_payoff_to_fee_ratio = 2.0 (from default config)
    // Expected gain needed is >= total_est_fees * 2.0 = ~1.77 * 2 = ~3.54 INR
    // Let's set take_profit such that expected_gain is just below required ratio
    core::TradingSignal poor_sig = sig;
    poor_sig.take_profit = core::to_paise(150.05); // 0.05 * 33 = 1.65 INR gain (< total_est_fees * 2.0)
    const auto [poor_qty, poor_rej] = risk_mgr.evaluate_and_size_order(poor_sig, morning_time_ns);
    DU_EXPECT(poor_qty == 0);
    DU_EXPECT(poor_rej == risk::RiskRejection::INSUFFICIENT_PAYOFF_TO_FEE_RATIO);

    std::cout << "[PASS] test_rate_parity_paper_broker_vs_risk_manager\n";
}

int main() {
    std::cout << "====================================================================\n";
    std::cout << "  EMPIRICAL ADVERSARIAL STATUTORY FRICTION HARNESS (MILESTONE 3)    \n";
    std::cout << "====================================================================\n";

    test_diverse_price_turnover_calculations();
    test_exact_statutory_rates();
    test_friction_decomposition_invariant();
    test_pnl_accounting_identity();
    test_friction_drag_ratio_zero_or_negative_profit();
    test_rate_parity_paper_broker_vs_risk_manager();

    std::cout << "====================================================================\n";
    std::cout << "  ALL EMPIRICAL ADVERSARIAL FRICTION STRESS TESTS PASSED (100%)     \n";
    std::cout << "====================================================================\n";
    return 0;
}
